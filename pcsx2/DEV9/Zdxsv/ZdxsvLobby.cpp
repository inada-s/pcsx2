// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ZdxsvLobby.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // std::min/std::max; the CMake build doesn't define it, the MSBuild one does
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

namespace Zdxsv
{
	namespace
	{
		std::mutex g_mtx;
		std::function<void(const std::string&)> g_log;
		BattleInfo g_info;
		std::function<void(const BattleInfo&)> g_listener;

		void Log(const std::string& s)
		{
			std::function<void(const std::string&)> log;
			{
				std::lock_guard lock(g_mtx);
				log = g_log;
			}
			if (log)
				log("zdxsv: " + s);
		}

		std::string AddrString(uint32_t ip, uint16_t port)
		{
			const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
			return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
				   std::to_string(b[3]) + ":" + std::to_string(port);
		}
	} // namespace

	void SetLogger(std::function<void(const std::string&)> log)
	{
		std::lock_guard lock(g_mtx);
		g_log = std::move(log);
	}

	// "a.b.c.d:port" -> network byte order ip, host order port
	static bool ParseAddr(const std::string& s, uint32_t& ip, uint16_t& port)
	{
		const size_t colon = s.rfind(':');
		in_addr addr{};
		if (colon == std::string::npos || inet_pton(AF_INET, s.substr(0, colon).c_str(), &addr) != 1)
			return false;
		const int p = std::atoi(s.c_str() + colon + 1);
		if (p <= 0 || p > 0xFFFF)
			return false;
		std::memcpy(&ip, &addr, 4);
		port = static_cast<uint16_t>(p);
		return true;
	}

	// "a.b.c.d:port" or "[v6]:port" -> numeric ip (as inet_ntop writes it), host order port
	static bool ParsePeerAddr(const std::string& s, PeerAddr& out)
	{
		const size_t colon = s.rfind(':');
		if (colon == std::string::npos)
			return false;
		std::string host = s.substr(0, colon);
		const bool v6 = host.size() > 2 && host.front() == '[' && host.back() == ']';
		if (v6)
			host = host.substr(1, host.size() - 2);
		const int p = std::atoi(s.c_str() + colon + 1);
		if (p <= 0 || p > 0xFFFF)
			return false;
		char buf[INET6_ADDRSTRLEN] = {};
		in6_addr a6{};
		in_addr a4{};
		if (v6 ? (inet_pton(AF_INET6, host.c_str(), &a6) != 1 || !inet_ntop(AF_INET6, &a6, buf, sizeof(buf))) :
				 (inet_pton(AF_INET, host.c_str(), &a4) != 1 || !inet_ntop(AF_INET, &a4, buf, sizeof(buf))))
			return false;
		out = {buf, static_cast<uint16_t>(p)};
		return true;
	}

	bool ParseBattleInfo(const std::string& body, BattleInfo& out)
	{
		std::map<std::string, std::string> kv;
		std::istringstream in(body);
		std::string line;
		while (std::getline(in, line))
		{
			const size_t eq = line.find('=');
			if (eq != std::string::npos)
				kv[line.substr(0, eq)] = line.substr(eq + 1);
		}
		BattleInfo info;
		info.sessionId = kv["session_id"];
		info.userId = kv["user_id"];
		info.battleCode = kv["battle_code"];
		if (info.sessionId.empty() || info.userId.empty() ||
			!ParseAddr(kv["battle_server"], info.serverIP, info.serverPort))
			return false;
		std::istringstream users(kv["users"]);
		std::string id;
		while (std::getline(users, id, ','))
			if (!id.empty())
				info.users.push_back(id);
		if (std::find(info.users.begin(), info.users.end(), info.userId) == info.users.end())
			return false;
		for (const std::string& u : info.users)
		{
			auto it = kv.find("p2p_" + u);
			if (u == info.userId || it == kv.end())
				continue;
			BattleInfo::Peer peer{u, {}};
			std::istringstream addrs(it->second);
			std::string a;
			while (std::getline(addrs, a, ','))
			{
				PeerAddr addr;
				if (ParsePeerAddr(a, addr))
					peer.addrs.push_back(addr);
			}
			if (!peer.addrs.empty())
				info.p2p.push_back(std::move(peer));
		}
		for (const std::string& u : info.users)
		{
			auto it = kv.find("ggpo_" + u);
			const int port = it == kv.end() ? 0 : std::atoi(it->second.c_str());
			if (u != info.userId && port > 0 && port <= 0xFFFF)
				info.ggpo[u] = static_cast<uint16_t>(port);
		}
		info.ggpoSession = static_cast<uint32_t>(std::strtoul(kv["ggpo_session"].c_str(), nullptr, 10));
		info.ggpoPingMs = std::atoi(kv["ggpo_ping_ms"].c_str());
		out = std::move(info);
		return true;
	}

	void SetBattleInfo(const BattleInfo& info)
	{
		std::function<void(const BattleInfo&)> listener;
		{
			std::lock_guard lock(g_mtx);
			g_info = info;
			listener = g_listener;
		}
		Log("battle info: user " + info.userId + ", " + std::to_string(info.users.size()) + " players, server " +
			AddrString(info.serverIP, info.serverPort) + ", " + std::to_string(info.p2p.size()) + " p2p peers, " +
			std::to_string(info.ggpo.size()) + " ggpo peers");
		if (listener)
			listener(info);
	}

	void SetBattleInfoListener(std::function<void(const BattleInfo&)> listener)
	{
		std::lock_guard lock(g_mtx);
		g_listener = std::move(listener);
	}

	bool GgpoPeers(const BattleInfo& info, const std::string& ownPublicIP, std::vector<std::vector<PeerAddr>>& byPosition)
	{
		byPosition.assign(info.users.size(), {});
		for (size_t i = 0; i < info.users.size(); i++)
		{
			const std::string& u = info.users[i];
			if (u == info.userId)
				continue;
			const auto port = info.ggpo.find(u);
			const auto peer = std::find_if(info.p2p.begin(), info.p2p.end(), [&u](const BattleInfo::Peer& p) { return p.userId == u; });
			if (port == info.ggpo.end() || peer == info.p2p.end())
				return false;
			std::vector<PeerAddr> v4, v6;
			for (const PeerAddr& a : peer->addrs)
				(a.V6() ? v6 : v4).push_back({a.ip, port->second});
			// same NAT (its public IPv4 is ours): its local IPv4 (the last one) first
			if (v4.size() > 1 && !ownPublicIP.empty() && v4.front().ip == ownPublicIP)
				std::rotate(v4.begin(), v4.end() - 1, v4.end());
			std::vector<PeerAddr>& c = byPosition[i];
			for (const std::vector<PeerAddr>* list : {&v4, &v6})
				for (const PeerAddr& a : *list)
					if (std::find(c.begin(), c.end(), a) == c.end())
						c.push_back(a);
		}
		return true;
	}

	size_t LobbyFilter::Take(uint8_t* dst, size_t max)
	{
		const size_t n = std::min(max, ready.size());
		std::memcpy(dst, ready.data(), n);
		ready.erase(ready.begin(), ready.begin() + n);
		return n;
	}

	void LobbyFilter::Feed(const uint8_t* data, size_t len)
	{
		constexpr size_t header = 12;
		buf.insert(buf.end(), data, data + len);
		std::vector<uint8_t>& out = ready;
		size_t pos = 0;
		while (buf.size() - pos >= header)
		{
			const uint8_t* f = &buf[pos];
			const size_t total = header + ((size_t{f[4]} << 8) | f[5]);
			if (buf.size() - pos < total)
				break;
			if (f[0] == 0x18 && f[1] == 0xFF && f[2] == 0x99 && f[3] == 0x51)
			{
				BattleInfo info;
				if (ParseBattleInfo(std::string(reinterpret_cast<const char*>(f + header), total - header), info))
					SetBattleInfo(info);
				else
					Log("bad battle info notice");
			}
			else
				out.insert(out.end(), f, f + total);
			pos += total;
		}
		buf.erase(buf.begin(), buf.begin() + pos);
	}

	// ---- protobuf (proto2) codec for zdxsv.proto: Ping / Pong only (the lobby's STUN) ----
	namespace Proto
	{
		namespace
		{
			void PutVarint(std::vector<uint8_t>& o, uint64_t v)
			{
				while (v >= 0x80)
				{
					o.push_back(static_cast<uint8_t>(v | 0x80));
					v >>= 7;
				}
				o.push_back(static_cast<uint8_t>(v));
			}

			void PutBytes(std::vector<uint8_t>& o, uint32_t field, const uint8_t* p, size_t n)
			{
				PutVarint(o, (field << 3) | 2);
				PutVarint(o, n);
				o.insert(o.end(), p, p + n);
			}

			void PutString(std::vector<uint8_t>& o, uint32_t field, const std::string& s)
			{
				PutBytes(o, field, reinterpret_cast<const uint8_t*>(s.data()), s.size());
			}

			void PutUint(std::vector<uint8_t>& o, uint32_t field, uint64_t v)
			{
				PutVarint(o, field << 3);
				PutVarint(o, v);
			}

			struct Reader
			{
				const uint8_t* p;
				const uint8_t* end;

				bool Varint(uint64_t& v)
				{
					v = 0;
					for (int shift = 0; shift < 64 && p < end; shift += 7)
					{
						const uint8_t b = *p++;
						v |= uint64_t{b & 0x7Fu} << shift;
						if (!(b & 0x80))
							return true;
					}
					return false;
				}

				// Calls f(field, wiretype, varint, bytes, len) per field; skips fixed32/64.
				template <typename F>
				bool Fields(F f)
				{
					while (p < end)
					{
						uint64_t tag, v = 0;
						if (!Varint(tag))
							return false;
						const uint32_t wt = tag & 7;
						const uint8_t* bytes = nullptr;
						size_t n = 0;
						if (wt == 0)
						{
							if (!Varint(v))
								return false;
						}
						else if (wt == 2)
						{
							if (!Varint(v) || v > static_cast<uint64_t>(end - p))
								return false;
							bytes = p;
							n = static_cast<size_t>(v);
							p += n;
						}
						else if (wt == 1 || wt == 5)
						{
							const size_t skip = wt == 1 ? 8 : 4;
							if (static_cast<size_t>(end - p) < skip)
								return false;
							p += skip;
							continue;
						}
						else
							return false;
						if (!f(static_cast<uint32_t>(tag >> 3), wt, v, bytes, n))
							return false;
					}
					return true;
				}
			};
		} // namespace

		std::vector<uint8_t> Encode(const Packet& pkt)
		{
			std::vector<uint8_t> o;
			PutUint(o, 1, pkt.type);
			if (pkt.type == Ping || pkt.type == Pong)
			{
				std::vector<uint8_t> m;
				PutUint(m, 1, static_cast<uint64_t>(pkt.timestamp));
				if (!pkt.pingUserId.empty())
					PutString(m, 2, pkt.pingUserId);
				if (pkt.type == Pong && !pkt.publicAddr.empty())
					PutString(m, 3, pkt.publicAddr);
				PutBytes(o, pkt.type == Ping ? 11 : 12, m.data(), m.size());
			}
			return o;
		}

		bool Decode(const uint8_t* data, size_t len, Packet& pkt)
		{
			pkt = Packet{};
			Reader r{data, data + len};
			return r.Fields([&](uint32_t field, uint32_t wt, uint64_t v, const uint8_t* bytes, size_t n) {
				if (wt == 0 && field == 1)
					pkt.type = static_cast<uint32_t>(v);
				else if (wt == 2 && (field == 11 || field == 12))
				{
					Reader m{bytes, bytes + n};
					return m.Fields([&](uint32_t f, uint32_t w, uint64_t mv, const uint8_t* mb, size_t mn) {
						if (w == 0 && f == 1)
							pkt.timestamp = static_cast<int64_t>(mv);
						else if (w == 2 && f == 2)
							pkt.pingUserId.assign(reinterpret_cast<const char*>(mb), mn);
						else if (w == 2 && f == 3)
							pkt.publicAddr.assign(reinterpret_cast<const char*>(mb), mn);
						return true;
					});
				}
				return true;
			});
		}
	} // namespace Proto

	// ---- socket helpers, STUN ----
	namespace
	{
		using Clock = std::chrono::steady_clock;

		bool WaitReadable(sock_t s, int ms)
		{
			fd_set rd;
			FD_ZERO(&rd);
			FD_SET(s, &rd);
			timeval tv{ms / 1000, (ms % 1000) * 1000};
			return select(static_cast<int>(s) + 1, &rd, nullptr, nullptr, &tv) > 0;
		}

		sockaddr_in MakeAddr(uint32_t ip, uint16_t port)
		{
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_addr.s_addr = ip;
			a.sin_port = htons(port);
			return a;
		}

		bool SameAddr(const sockaddr_in& a, const sockaddr_in& b)
		{
			return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
		}

		std::string AddrString(const sockaddr_in& a)
		{
			return AddrString(a.sin_addr.s_addr, ntohs(a.sin_port));
		}

		// numeric PeerAddr -> sockaddr_in / sockaddr_in6; false if the ip doesn't parse
		bool ToSockaddr(const PeerAddr& a, sockaddr_storage& sa, socklen_t& len)
		{
			sa = {};
			if (a.V6())
			{
				auto* s6 = reinterpret_cast<sockaddr_in6*>(&sa);
				s6->sin6_family = AF_INET6;
				s6->sin6_port = htons(a.port);
				len = sizeof(sockaddr_in6);
				return inet_pton(AF_INET6, a.ip.c_str(), &s6->sin6_addr) == 1;
			}
			auto* s4 = reinterpret_cast<sockaddr_in*>(&sa);
			s4->sin_family = AF_INET;
			s4->sin_port = htons(a.port);
			len = sizeof(sockaddr_in);
			return inet_pton(AF_INET, a.ip.c_str(), &s4->sin_addr) == 1;
		}

		// same family, ip and port (IPv6 scope / flow info ignored)
		bool SameAddr(const sockaddr_storage& a, const sockaddr_storage& b)
		{
			if (a.ss_family != b.ss_family)
				return false;
			if (a.ss_family == AF_INET)
				return SameAddr(reinterpret_cast<const sockaddr_in&>(a), reinterpret_cast<const sockaddr_in&>(b));
			const auto& a6 = reinterpret_cast<const sockaddr_in6&>(a);
			const auto& b6 = reinterpret_cast<const sockaddr_in6&>(b);
			return a.ss_family == AF_INET6 && a6.sin6_port == b6.sin6_port &&
				   std::memcmp(&a6.sin6_addr, &b6.sin6_addr, sizeof(a6.sin6_addr)) == 0;
		}

		// UDP socket of family af bound to port on every address; IPv6 only on AF_INET6 (as flycast's
		// UdpClient: one socket per family). INVALID_SOCKET if it can't be opened or bound.
		sock_t BindUdp(int af, uint16_t port)
		{
			sock_t s = socket(af, SOCK_DGRAM, IPPROTO_UDP);
			if (s == INVALID_SOCKET)
				return s;
			sockaddr_storage sa{};
			socklen_t len = 0;
			const int on = 1;
			if (af == AF_INET6)
			{
				setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&on), sizeof(on));
				ToSockaddr({"::", port}, sa, len);
			}
			else
				ToSockaddr({"0.0.0.0", port}, sa, len);
			if (bind(s, reinterpret_cast<const sockaddr*>(&sa), len) != 0)
			{
				closesocket(s);
				return INVALID_SOCKET;
			}
			return s;
		}

		// flycast is_loopback_addr / is_private_addr
		bool IsLoopback(const sockaddr_storage& a)
		{
			if (a.ss_family == AF_INET)
				return reinterpret_cast<const sockaddr_in&>(a).sin_addr.s_addr == htonl(INADDR_LOOPBACK);
			const auto& a6 = reinterpret_cast<const sockaddr_in6&>(a);
			return a.ss_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&a6.sin6_addr);
		}

		bool IsPrivate(const sockaddr_storage& a)
		{
			if (a.ss_family == AF_INET)
			{
				const auto* ip4 = reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in&>(a).sin_addr);
				return ip4[0] == 10 || (ip4[0] == 172 && (ip4[1] & 0xf0) == 16) || (ip4[0] == 192 && ip4[1] == 168);
			}
			const auto* ip6 = reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in6&>(a).sin6_addr);
			return a.ss_family == AF_INET6 && (ip6[0] & 0xfe) == 0xfc;
		}

		int64_t NowNanos()
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
		}

		// OpenUdp's platform info lines, kept for the emulator's life: a reconnect to the lobby
		// (after a battle) reports the same address. DEV9 thread only.
		bool g_udpOpened = false;
		std::string g_udpLines;
	} // namespace

	// ---- GGPO ping test ----
	namespace
	{
		// flycast core/gdxsv/gdxsv_network.h UdpPingPong::Packet, byte for byte
#pragma pack(push, 1)
		struct PingPacket
		{
			uint32_t magic;
			uint32_t sessionId;
			uint8_t type;
			uint8_t fromPeer;
			uint8_t toPeer;
			uint8_t candidate;
			uint64_t sendTimestamp;
			uint64_t pingTimestamp; // pong: the ping's sendTimestamp
			uint8_t rttMatrix[4][4];
		};
#pragma pack(pop)
		constexpr uint32_t PING_MAGIC = 2205246188u;
		constexpr uint8_t PING_TYPE = 1, PONG_TYPE = 2;

		struct PingTest
		{
			std::thread thread;
			std::atomic<bool> stop{false};
			std::vector<PingResult> result;
			~PingTest()
			{
				stop = true;
				if (thread.joinable())
					thread.join();
			}
		};
		std::unique_ptr<PingTest> g_ping;

		uint64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
		}

		// sockets of socks (INVALID_SOCKET skipped) that are readable within ms
		std::vector<sock_t> Readable(std::initializer_list<sock_t> socks, int ms)
		{
			fd_set rd;
			FD_ZERO(&rd);
			sock_t top = 0;
			for (const sock_t s : socks)
			{
				if (s == INVALID_SOCKET)
					continue;
				FD_SET(s, &rd);
				top = std::max(top, s);
			}
			timeval tv{ms / 1000, (ms % 1000) * 1000};
			std::vector<sock_t> out;
			if (select(static_cast<int>(top) + 1, &rd, nullptr, nullptr, &tv) > 0)
				for (const sock_t s : socks)
					if (s != INVALID_SOCKET && FD_ISSET(s, &rd))
						out.push_back(s);
			return out;
		}
	} // namespace

	void StartPingTest(uint32_t session, const std::vector<std::vector<PeerAddr>>& byPosition, uint16_t port, int durationMs)
	{
		FinishPingTest();
		const int n = static_cast<int>(byPosition.size());
		int me = -1;
		for (int p = 0; p < n; p++)
			if (byPosition[p].empty())
				me = p;
		if (me < 0 || n > 4)
			return;
		const sock_t s4 = BindUdp(AF_INET, port), s6 = BindUdp(AF_INET6, port);
		if (s4 == INVALID_SOCKET && s6 == INVALID_SOCKET)
		{
			Log("ping test: bind :" + std::to_string(port) + " failed");
			return;
		}
		if (s4 == INVALID_SOCKET || s6 == INVALID_SOCKET)
			Log(std::string("ping test: no IPv") + (s4 == INVALID_SOCKET ? "4" : "6") + " socket on :" + std::to_string(port) + ", its candidates skipped");
		g_ping = std::make_unique<PingTest>();
		g_ping->result.assign(n, {});
		PingTest* t = g_ping.get();
		t->thread = std::thread([t, s4, s6, session, byPosition, me, n, durationMs] {
			// candidate k of position p: byPosition[p][k], the index sent in its pings' candidate byte
			struct Cand
			{
				sockaddr_storage sa{};
				socklen_t len = 0;
				sock_t s = INVALID_SOCKET; // socket of its family, INVALID_SOCKET = not pinged
				int64_t sum = 0, pongs = 0;
			};
			std::vector<std::vector<Cand>> cand(n);
			for (int p = 0; p < n; p++)
				for (const PeerAddr& a : byPosition[p])
				{
					Cand c;
					if (ToSockaddr(a, c.sa, c.len))
						c.s = a.V6() ? s6 : s4;
					cand[p].push_back(c);
				}
			std::vector<int64_t> pongsIn(n, 0);
			int dropped = 0;
			const auto start = Clock::now();
			auto next = start;
			for (;;)
			{
				const auto now = Clock::now();
				const int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
				if (t->stop || elapsed >= durationMs)
					break;
				if (now >= next && elapsed + 500 < durationMs)
				{
					next = now + std::chrono::milliseconds(100);
					for (int p = 0; p < n; p++)
					{
						for (size_t k = 0; p != me && k < cand[p].size(); k++)
						{
							const Cand& c = cand[p][k];
							if (c.s == INVALID_SOCKET)
								continue;
							PingPacket pk{};
							pk.magic = PING_MAGIC;
							pk.sessionId = session;
							pk.type = PING_TYPE;
							pk.fromPeer = static_cast<uint8_t>(me);
							pk.toPeer = static_cast<uint8_t>(p);
							pk.candidate = static_cast<uint8_t>(k);
							pk.sendTimestamp = NowMs();
							sendto(c.s, reinterpret_cast<const char*>(&pk), sizeof(pk), 0, reinterpret_cast<const sockaddr*>(&c.sa), c.len);
						}
					}
				}
				for (const sock_t s : Readable({s4, s6}, 10))
				{
					PingPacket pk;
					sockaddr_storage from{};
					socklen_t len = sizeof(from);
					// Windows: an ICMP port unreachable (peer not bound yet) fails this recv, nothing to read
					if (recvfrom(s, reinterpret_cast<char*>(&pk), sizeof(pk), 0, reinterpret_cast<sockaddr*>(&from), &len) != sizeof(pk))
						continue;
					const bool head = pk.magic == PING_MAGIC && pk.sessionId == session && pk.toPeer == me && pk.fromPeer < n && pk.fromPeer != me;
					const std::vector<Cand>* fc = head ? &cand[pk.fromPeer] : nullptr;
					if (head && pk.type == PING_TYPE &&
						std::any_of(fc->begin(), fc->end(), [&from](const Cand& c) { return SameAddr(from, c.sa); }))
					{
						PingPacket pong{};
						pong.magic = PING_MAGIC;
						pong.sessionId = session;
						pong.type = PONG_TYPE;
						pong.fromPeer = static_cast<uint8_t>(me);
						pong.toPeer = pk.fromPeer;
						pong.candidate = pk.candidate;
						pong.sendTimestamp = NowMs();
						pong.pingTimestamp = pk.sendTimestamp;
						sendto(s, reinterpret_cast<const char*>(&pong), sizeof(pong), 0, reinterpret_cast<const sockaddr*>(&from), len);
						pongsIn[pk.fromPeer]++;
					}
					else if (head && pk.type == PONG_TYPE && pk.candidate < fc->size() && SameAddr(from, (*fc)[pk.candidate].sa))
					{
						Cand& c = cand[pk.fromPeer][pk.candidate];
						c.sum += std::max<int64_t>(1, static_cast<int64_t>(NowMs() - pk.pingTimestamp));
						c.pongs++;
					}
					else
						dropped++;
				}
			}
			if (s4 != INVALID_SOCKET)
				closesocket(s4);
			if (s6 != INVALID_SOCKET)
				closesocket(s6);
			std::string line = "ping test: session " + std::to_string(session) + ", position " + std::to_string(me);
			for (int p = 0; p < n; p++)
			{
				if (p == me)
					continue;
				// flycast UdpPingPong::GetAvailableAddress: highest 10000 - rtt, +100 loopback, +50 private, +20 IPv6
				float best = 0;
				line += "; peer " + std::to_string(p);
				for (size_t k = 0; k < cand[p].size(); k++)
				{
					const Cand& c = cand[p][k];
					const int rtt = c.pongs ? static_cast<int>((c.sum + c.pongs - 1) / c.pongs) : -1;
					line += " " + byPosition[p][k].String() + " rtt " + std::to_string(rtt) + " ms (" + std::to_string(c.pongs) + " pongs)";
					if (rtt <= 0)
						continue;
					const float score = 10000.f - rtt + (IsLoopback(c.sa) ? 100.f : 0.f) + (IsPrivate(c.sa) ? 50.f : 0.f) +
										(c.sa.ss_family == AF_INET6 ? 20.f : 0.f);
					if (score > best)
						best = score, t->result[p] = {rtt, byPosition[p][k]};
				}
				line += ", " + std::to_string(pongsIn[p]) + " pings answered, picked " +
						(t->result[p].rtt > 0 ? t->result[p].addr.String() : std::string("none"));
			}
			Log(line + ", " + std::to_string(dropped) + " dropped");
		});
	}

	std::vector<PingResult> FinishPingTest()
	{
		if (!g_ping)
			return {};
		if (g_ping->thread.joinable())
			g_ping->thread.join();
		std::vector<PingResult> result = std::move(g_ping->result);
		g_ping.reset();
		return result;
	}

	std::string PublicIP()
	{
		const std::string& lines = g_udpLines;
		const size_t at = lines.find("udp_addr=");
		PeerAddr a;
		if (at == std::string::npos || !ParsePeerAddr(lines.substr(at + 9, lines.find('\n', at) - at - 9), a))
			return "";
		return a.ip;
	}

	std::string OpenUdp(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort)
	{
		// The socket is only for the STUN question: peers take the public IP, GGPO has its own port.
		if (g_udpOpened)
			return g_udpLines;
		g_udpOpened = true;
		sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(bindPort);
		socklen_t len = sizeof(addr);
		if (s == INVALID_SOCKET || bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
			getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
		{
			if (s != INVALID_SOCKET)
				closesocket(s);
			Log("udp: socket failed, no STUN");
			return "";
		}
		const uint16_t port = ntohs(addr.sin_port);
		const sockaddr_in stun = MakeAddr(stunIP, stunPort);
		// Local address: the interface that routes to the lobby.
		std::string local;
		sock_t probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in self{};
		len = sizeof(self);
		if (probe != INVALID_SOCKET && connect(probe, reinterpret_cast<const sockaddr*>(&stun), sizeof(stun)) == 0 &&
			getsockname(probe, reinterpret_cast<sockaddr*>(&self), &len) == 0)
			local = AddrString(self.sin_addr.s_addr, port);
		if (probe != INVALID_SOCKET)
			closesocket(probe);
		// IPv6 has no NAT: our public address is the source of a route to a global one (2001:db8::1,
		// documentation prefix; connect() on UDP sends nothing), if it is global unicast (2000::/3).
		std::string pub6;
		sock_t probe6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_storage dst6{}, self6{};
		socklen_t len6 = 0;
		if (probe6 != INVALID_SOCKET && ToSockaddr({"2001:db8::1", 53}, dst6, len6) &&
			connect(probe6, reinterpret_cast<const sockaddr*>(&dst6), len6) == 0 &&
			getsockname(probe6, reinterpret_cast<sockaddr*>(&self6), &len6) == 0 && self6.ss_family == AF_INET6)
		{
			const in6_addr& a6 = reinterpret_cast<const sockaddr_in6&>(self6).sin6_addr;
			char buf[INET6_ADDRSTRLEN] = {};
			if ((reinterpret_cast<const uint8_t*>(&a6)[0] & 0xe0) == 0x20 && inet_ntop(AF_INET6, &a6, buf, sizeof(buf)))
				pub6 = PeerAddr{buf, port}.String();
		}
		if (probe6 != INVALID_SOCKET)
			closesocket(probe6);
		// zproxy's STUN: Ping -> Pong{public_addr}. 3 tries, 200 ms each.
		std::string pub;
		Proto::Packet ping;
		ping.type = Proto::Ping;
		for (int i = 0; i < 3 && pub.empty(); i++)
		{
			ping.timestamp = NowNanos();
			const std::vector<uint8_t> data = Proto::Encode(ping);
			sendto(s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
				reinterpret_cast<const sockaddr*>(&stun), sizeof(stun));
			const auto until = Clock::now() + std::chrono::milliseconds(200);
			while (pub.empty() && Clock::now() < until && WaitReadable(s, 50))
			{
				uint8_t buf[512];
				sockaddr_in from{};
				socklen_t fromLen = sizeof(from);
				const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
				Proto::Packet pong;
				if (n > 0 && SameAddr(from, stun) && Proto::Decode(buf, n, pong) && pong.type == Proto::Pong)
					pub = pong.publicAddr;
			}
		}
		closesocket(s);
		if (!pub.empty())
			g_udpLines += "udp_addr=" + pub + "\n";
		if (!local.empty())
			g_udpLines += "udp_local=" + local + "\n";
		if (!pub6.empty())
			g_udpLines += "udp_addr6=" + pub6 + "\n";
		Log("udp: port " + std::to_string(port) + ", public " + (pub.empty() ? "unknown (no STUN answer from " + AddrString(stun) + ")" : pub) +
			", local " + local + ", IPv6 " + (pub6.empty() ? "none" : pub6));
		return g_udpLines;
	}

	static std::atomic<bool> g_stateLoaded{false};

	void OnStateLoaded()
	{
		g_stateLoaded = true;
	}

	bool LobbyStateEnabled()
	{
		static const bool enabled = [] {
			const char* env = std::getenv("ZDXSV_LOBBY_STATE");
			return env && std::string(env) == "1";
		}();
		return enabled;
	}

	bool AdoptConnections()
	{
		return g_stateLoaded && LobbyStateEnabled();
	}

	bool IsBattleServer(uint32_t ip, uint16_t port)
	{
		std::lock_guard lock(g_mtx);
		return ip == g_info.serverIP && port == g_info.serverPort;
	}
} // namespace Zdxsv

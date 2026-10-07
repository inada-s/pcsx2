// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/Lobby.h"
#include "Zdxsv/Proto.h"

#include <array>
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

	void LobbySetLogger(std::function<void(const std::string&)> log)
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
		info.ggpoPingMs = std::clamp(std::atoi(kv["ggpo_ping_ms"].c_str()), 0, MAX_PING_MS);
		info.liveUplink = kv["live_uplink"] == "1";
		for (const std::string& u : info.users)
			if (auto it = kv.find("name_" + u); it != kv.end())
				info.names[u] = it->second;
		// relay_0.. in order; a malformed one ends the list, so the indexes stay the lobby's
		for (int k = 0; k < 4; k++)
		{
			auto it = kv.find("relay_" + std::to_string(k));
			if (it == kv.end())
				break;
			std::istringstream fields(it->second);
			std::string token, a4, a6;
			std::getline(fields, token, ',');
			std::getline(fields, a4, ',');
			std::getline(fields, a6, ',');
			BattleInfo::Relay r;
			char* end = nullptr;
			r.token = std::strtoull(token.c_str(), &end, 16);
			if (token.empty() || *end || !ParsePeerAddr(a4, r.addr) || (!a6.empty() && !ParsePeerAddr(a6, r.addr6)))
				break;
			info.relays.push_back(r);
		}
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
			std::to_string(info.ggpo.size()) + " ggpo peers, " + std::to_string(info.relays.size()) + " relays");
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

	void LobbyFilterDeleter::operator()(LobbyFilter* filter) const
	{
		delete filter;
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

	// ---- protobuf codec for zdxsv.proto (Zdxsv/Proto.h): Ping / Pong only (the lobby's STUN) ----
	using namespace Pb;

	std::vector<uint8_t> ProtoEncode(const ProtoPacket& pkt)
	{
		std::vector<uint8_t> o;
		PutUint(o, 1, pkt.type);
		if (pkt.type == ProtoPing || pkt.type == ProtoPong)
		{
			std::vector<uint8_t> m;
			PutUint(m, 1, static_cast<uint64_t>(pkt.timestamp));
			if (!pkt.pingUserId.empty())
				PutString(m, 2, pkt.pingUserId);
			if (pkt.type == ProtoPong && !pkt.publicAddr.empty())
				PutString(m, 3, pkt.publicAddr);
			PutBytes(o, pkt.type == ProtoPing ? 11 : 12, m.data(), m.size());
		}
		return o;
	}

	bool ProtoDecode(const uint8_t* data, size_t len, ProtoPacket& pkt)
	{
		pkt = ProtoPacket{};
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
		std::string g_udpAddr; // the STUN address OpenUdp asked (LobbyUdpAddr)
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
			uint8_t rttMatrix[4][4]; // [i][j]: i's rtt to j in ms (255 max), 0 = unknown; each player fills its own row
		};
		// sent instead of PingPacket only when the battle has relays (flycast PacketWithRelays)
		struct PingPacketRelays : PingPacket
		{
			uint8_t relayRttMatrix[4][4]; // [i][k]: i's rtt to relay k
		};
		// gdxsv relay.go ping, echoed as a pong (flycast UdpPingPong::RelayPacket)
		struct RelayPacket
		{
			uint32_t magic;
			uint8_t type;
			uint8_t peer;
			uint8_t relayIdx;
			uint8_t reserved;
			uint32_t sessionId;
			uint64_t token;
			uint64_t timestamp;
		};
#pragma pack(pop)
		static_assert(sizeof(PingPacket) == 44 && sizeof(PingPacketRelays) == 60 && sizeof(RelayPacket) == 28);
		constexpr uint32_t PING_MAGIC = 2205246188u;
		constexpr uint8_t PING_TYPE = 1, PONG_TYPE = 2;
		constexpr uint32_t RELAY_MAGIC = 0x594c4552;
		constexpr uint8_t RELAY_PING = 1, RELAY_PONG = 2;

		struct PingTest
		{
			std::thread thread;
			std::atomic<bool> stop{false};
			std::vector<PingResult> result;
			std::vector<RelayServerAddr> servers;
			~PingTest()
			{
				stop = true;
				if (thread.joinable())
					thread.join();
			}
		};
		std::unique_ptr<PingTest> g_ping;
		std::string g_pingError; // why the last StartPingTest ran no test

		// StartUdpTest's thread
		struct UdpTestRun
		{
			std::thread thread;
			std::atomic<bool> stop{false};
			~UdpTestRun()
			{
				stop = true;
				if (thread.joinable())
					thread.join();
			}
		};
		std::mutex g_udpTestMtx;
		std::atomic<bool> g_udpTestRunning{false};
		std::unique_ptr<UdpTestRun> g_udpTest;

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

		// One ping test on its thread (flycast UdpPingPong): pings each candidate address of every other position
		// and each relay path, answers the peers' pings, then picks a path per position.
		class PingRun
		{
			// candidate k of position p: byPosition[p][k], the index sent in its pings' candidate byte
			struct Cand
			{
				sockaddr_storage sa{};
				socklen_t len = 0;
				sock_t s = INVALID_SOCKET; // socket of its family, INVALID_SOCKET = not pinged
				int64_t sum = 0, pongs = 0;
				int Rtt() const { return pongs ? static_cast<int>((sum + pongs - 1) / pongs) : -1; }
			};

		public:
			PingRun(sock_t s4, sock_t s6, uint32_t session, const std::vector<std::vector<PeerAddr>>& byPosition, int me,
				const std::vector<BattleInfo::Relay>& relays)
				: m_s4(s4)
				, m_s6(s6)
				, m_session(session)
				, m_byPosition(byPosition)
				, m_me(me)
				, m_n(static_cast<int>(byPosition.size()))
				, m_relays(relays)
				, m_nr(static_cast<int>(std::min<size_t>(relays.size(), 4)))
				// older peers read only a PingPacket: the relay rtts go out only in battles with relays (as flycast)
				, m_pkSize(m_nr ? sizeof(PingPacketRelays) : sizeof(PingPacket))
				, m_cand(m_n)
				, m_rpath(m_nr)
				, m_pongsIn(m_n, 0)
			{
				for (int p = 0; p < m_n; p++)
					for (const PeerAddr& a : m_byPosition[p])
					{
						Cand c;
						if (ToSockaddr(a, c.sa, c.len))
							c.s = a.V6() ? m_s6 : m_s4;
						m_cand[p].push_back(c);
					}
				// relay k's IPv4 and IPv6 path, pinged like a candidate
				for (int k = 0; k < m_nr; k++)
					for (const PeerAddr* a : {&m_relays[k].addr, &m_relays[k].addr6})
					{
						Cand& c = m_rpath[k][a->V6() ? 1 : 0];
						if (!a->ip.empty() && ToSockaddr(*a, c.sa, c.len))
							c.s = a->V6() ? m_s6 : m_s4;
					}
			}

			// Pings every 100 ms (none in the last 500 ms) and answers until stop or durationMs, then closes the sockets.
			void Run(const std::atomic<bool>& stop, int durationMs)
			{
				const auto start = Clock::now();
				auto next = start;
				for (;;)
				{
					const auto now = Clock::now();
					const int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
					if (stop || elapsed >= durationMs)
						break;
					if (now >= next && elapsed + 500 < durationMs)
					{
						next = now + std::chrono::milliseconds(100);
						SendPings();
					}
					for (const sock_t s : Readable({m_s4, m_s6}, 10))
						Receive(s);
				}
				if (m_s4 != INVALID_SOCKET)
					closesocket(m_s4);
				if (m_s6 != INVALID_SOCKET)
					closesocket(m_s6);
			}

			// Fills t.result (path per position) and t.servers (GGPO's relay server addresses), logs the test.
			void Finish(PingTest& t)
			{
				OwnRow();
				std::string line = "ping test: session " + std::to_string(m_session) + ", position " + std::to_string(m_me);
				const std::vector<PingResult> direct = PickDirect(line);
				AddRelayServers(line, t.servers);
				for (int p = 0; p < m_n; p++)
				{
					if (p == m_me)
						continue;
					const PingResult r = PickPath(p, direct, t.servers);
					t.result[p] = r;
					line += "; path " + std::to_string(p) + " " +
							(r.via == 2 ? "relay " + std::to_string(r.relay) : r.via == 1 ? "peer " + std::to_string(r.relay) : std::string("direct")) +
							" rtt " + std::to_string(r.rtt);
				}
				Log(line + MatrixText() + ", " + std::to_string(m_dropped) + " dropped");
			}

		private:
			// the faster family (0 IPv4, 1 IPv6) of relay k that answered, -1 = none
			int RelayBest(int k) const
			{
				int f = -1;
				for (int i = 0; i < 2; i++)
					if (m_rpath[k][i].Rtt() > 0 && (f < 0 || m_rpath[k][i].Rtt() < m_rpath[k][f].Rtt()))
						f = i;
				return f;
			}

			// our rows of the matrices sent in every ping and pong
			void OwnRow()
			{
				for (int p = 0; p < m_n; p++)
				{
					int best = 0;
					for (const Cand& c : m_cand[p])
						if (c.Rtt() > 0 && (!best || c.Rtt() < best))
							best = c.Rtt();
					m_rtt[m_me][p] = static_cast<uint8_t>(std::min(255, best));
				}
				for (int k = 0; k < m_nr; k++)
				{
					const int f = RelayBest(k);
					m_relayRtt[m_me][k] = static_cast<uint8_t>(f < 0 ? 0 : std::min(255, m_rpath[k][f].Rtt()));
				}
			}

			void Send(PingPacketRelays& pk, sock_t s, const sockaddr_storage& sa, socklen_t len)
			{
				std::memcpy(pk.rttMatrix, m_rtt, sizeof(m_rtt));
				std::memcpy(pk.relayRttMatrix, m_relayRtt, sizeof(m_relayRtt));
				sendto(s, reinterpret_cast<const char*>(&pk), m_pkSize, 0, reinterpret_cast<const sockaddr*>(&sa), len);
			}

			void SendPings()
			{
				OwnRow();
				for (int p = 0; p < m_n; p++)
				{
					for (size_t k = 0; p != m_me && k < m_cand[p].size(); k++)
					{
						const Cand& c = m_cand[p][k];
						if (c.s == INVALID_SOCKET)
							continue;
						PingPacketRelays pk{};
						pk.magic = PING_MAGIC;
						pk.sessionId = m_session;
						pk.type = PING_TYPE;
						pk.fromPeer = static_cast<uint8_t>(m_me);
						pk.toPeer = static_cast<uint8_t>(p);
						pk.candidate = static_cast<uint8_t>(k);
						pk.sendTimestamp = NowMs();
						Send(pk, c.s, c.sa, c.len);
					}
				}
				for (int k = 0; k < m_nr; k++)
					for (const Cand& c : m_rpath[k])
					{
						if (c.s == INVALID_SOCKET)
							continue;
						RelayPacket rp{};
						rp.magic = RELAY_MAGIC;
						rp.type = RELAY_PING;
						rp.peer = static_cast<uint8_t>(m_me);
						rp.relayIdx = static_cast<uint8_t>(k);
						rp.sessionId = m_session;
						rp.token = m_relays[k].token;
						rp.timestamp = NowMs();
						sendto(c.s, reinterpret_cast<const char*>(&rp), sizeof(rp), 0, reinterpret_cast<const sockaddr*>(&c.sa), c.len);
					}
			}

			// One datagram from s: a relay pong, a peer's ping (answered) or pong, else dropped.
			void Receive(sock_t s)
			{
				union
				{
					PingPacketRelays pk;
					RelayPacket rp;
					char raw[128];
				} buf{};
				const PingPacketRelays& pk = buf.pk;
				sockaddr_storage from{};
				socklen_t len = sizeof(from);
				// Windows: an ICMP port unreachable (peer not bound yet) fails this recv, nothing to read
				const int got = recvfrom(s, buf.raw, sizeof(buf.raw), 0, reinterpret_cast<sockaddr*>(&from), &len);
				if (got == static_cast<int>(sizeof(RelayPacket)) && buf.rp.magic == RELAY_MAGIC)
				{
					const RelayPacket& rp = buf.rp;
					Cand* c = rp.relayIdx < m_nr ? &m_rpath[rp.relayIdx][s == m_s6 ? 1 : 0] : nullptr;
					if (c && rp.type == RELAY_PONG && rp.sessionId == m_session && rp.peer == m_me && rp.token == m_relays[rp.relayIdx].token &&
						SameAddr(from, c->sa))
					{
						c->sum += std::max<int64_t>(1, static_cast<int64_t>(NowMs() - rp.timestamp));
						c->pongs++;
					}
					else
						m_dropped++;
					return;
				}
				if (got < static_cast<int>(sizeof(PingPacket)))
					return;
				const bool head = pk.magic == PING_MAGIC && pk.sessionId == m_session && pk.toPeer == m_me && pk.fromPeer < m_n && pk.fromPeer != m_me;
				const std::vector<Cand>* fc = head ? &m_cand[pk.fromPeer] : nullptr;
				bool ok = false;
				if (head && pk.type == PING_TYPE &&
					std::any_of(fc->begin(), fc->end(), [&from](const Cand& c) { return SameAddr(from, c.sa); }))
				{
					OwnRow();
					PingPacketRelays pong{};
					pong.magic = PING_MAGIC;
					pong.sessionId = m_session;
					pong.type = PONG_TYPE;
					pong.fromPeer = static_cast<uint8_t>(m_me);
					pong.toPeer = pk.fromPeer;
					pong.candidate = pk.candidate;
					pong.sendTimestamp = NowMs();
					pong.pingTimestamp = pk.sendTimestamp;
					Send(pong, s, from, len);
					m_pongsIn[pk.fromPeer]++;
					ok = true;
				}
				else if (head && pk.type == PONG_TYPE && pk.candidate < fc->size() && SameAddr(from, (*fc)[pk.candidate].sa))
				{
					Cand& c = m_cand[pk.fromPeer][pk.candidate];
					c.sum += std::max<int64_t>(1, static_cast<int64_t>(NowMs() - pk.pingTimestamp));
					c.pongs++;
					ok = true;
				}
				else
					m_dropped++;
				if (ok) // the sender's own rows of the matrices
				{
					std::memcpy(m_rtt[pk.fromPeer], pk.rttMatrix[pk.fromPeer], sizeof(m_rtt[0]));
					if (got >= static_cast<int>(sizeof(PingPacketRelays)))
						std::memcpy(m_relayRtt[pk.fromPeer], pk.relayRttMatrix[pk.fromPeer], sizeof(m_relayRtt[0]));
				}
			}

			// the best direct candidate per position, rtt 0 = none answered
			std::vector<PingResult> PickDirect(std::string& line) const
			{
				std::vector<PingResult> direct(m_n);
				for (int p = 0; p < m_n; p++)
				{
					if (p == m_me)
						continue;
					// flycast UdpPingPong::GetAvailableAddress: highest 10000 - rtt, +100 loopback, +50 private, +20 IPv6
					float best = 0;
					line += "; peer " + std::to_string(p);
					for (size_t k = 0; k < m_cand[p].size(); k++)
					{
						const Cand& c = m_cand[p][k];
						const int r = c.Rtt();
						line += " " + m_byPosition[p][k].String() + " rtt " + std::to_string(r) + " ms (" + std::to_string(c.pongs) + " pongs)";
						if (r <= 0)
							continue;
						const float score = 10000.f - r + (IsLoopback(c.sa) ? 100.f : 0.f) + (IsPrivate(c.sa) ? 50.f : 0.f) +
											(c.sa.ss_family == AF_INET6 ? 20.f : 0.f);
						if (score > best)
							best = score, direct[p] = {r, m_byPosition[p][k]};
					}
					line += ", " + std::to_string(m_pongsIn[p]) + " pings answered, picked " +
							(direct[p].rtt > 0 ? direct[p].addr.String() : std::string("none"));
				}
				return direct;
			}

			void AddRelayServers(std::string& line, std::vector<RelayServerAddr>& servers) const
			{
				for (int k = 0; k < m_nr; k++)
				{
					const BattleInfo::Relay& rl = m_relays[k];
					line += "; relay " + std::to_string(k);
					for (int i = 0; i < 2; i++)
						if (m_rpath[k][i].s != INVALID_SOCKET)
							line += " " + (rl.addr.V6() == (i == 1) ? rl.addr : rl.addr6).String() + " rtt " + std::to_string(m_rpath[k][i].Rtt()) +
									" ms (" + std::to_string(m_rpath[k][i].pongs) + " pongs)";
					// GGPO's address of the relay: the faster family that answered, else addr; alt = the other one
					const int f = RelayBest(k);
					const bool useAddr6 = f >= 0 && !rl.addr6.ip.empty() && (f == 1) != rl.addr.V6();
					servers.push_back(useAddr6 ? RelayServerAddr{rl.addr6, rl.addr} : RelayServerAddr{rl.addr, rl.addr6});
				}
			}

			// path choice, flycast's rollback backend: a relaying peer must beat direct by 32 ms (it spends its
			// own bandwidth), a relay server by 16 ms; of the two the lower rtt wins
			PingResult PickPath(int p, const std::vector<PingResult>& direct, const std::vector<RelayServerAddr>& servers) const
			{
				PingResult r = direct[p];
				const bool directOk = r.rtt > 0;
				int peerJ = -1, peerSum = INT_MAX;
				for (int j = 0; j < m_n; j++)
					if (j != p && j != m_me && m_rtt[m_me][j] && 0 < m_rtt[j][p] && m_rtt[j][p] < 255 && m_rtt[m_me][j] + m_rtt[j][p] < peerSum)
						peerSum = m_rtt[m_me][j] + m_rtt[j][p], peerJ = j;
				bool peerOk = false;
				int peerRtt = 0;
				if (peerJ >= 0 && (!directOk || peerSum + 32 < r.rtt) && direct[peerJ].rtt > 0)
					peerOk = true, peerRtt = direct[peerJ].rtt + m_rtt[peerJ][p];
				int server = -1, serverSum = INT_MAX;
				for (int k = 0; k < m_nr; k++)
				{
					const int mine = m_relayRtt[m_me][k], theirs = m_relayRtt[p][k];
					if (0 < mine && mine < 255 && 0 < theirs && theirs < 255 && mine + theirs < serverSum)
						serverSum = mine + theirs, server = k;
				}
				const bool serverOk = server >= 0 && (!directOk || serverSum + 16 < r.rtt) && RelayBest(server) >= 0;
				if (serverOk && (!peerOk || serverSum <= peerRtt))
					r = {serverSum, servers[server].addr, 2, server};
				else if (peerOk)
					r = {peerRtt, direct[peerJ].addr, 1, peerJ};
				return r;
			}

			std::string MatrixText() const
			{
				std::string line = "; rtt";
				for (int i = 0; i < m_n; i++)
				{
					line += i ? " |" : "";
					for (int j = 0; j < m_n; j++)
						line += " " + std::to_string(m_rtt[i][j]);
				}
				for (int k = 0; k < m_nr; k++)
				{
					line += "; relay " + std::to_string(k) + " rtt";
					for (int i = 0; i < m_n; i++)
						line += " " + std::to_string(m_relayRtt[i][k]);
				}
				return line;
			}

			const sock_t m_s4, m_s6;
			const uint32_t m_session;
			const std::vector<std::vector<PeerAddr>> m_byPosition;
			const int m_me, m_n;
			const std::vector<BattleInfo::Relay> m_relays;
			const int m_nr;
			const int m_pkSize;
			std::vector<std::vector<Cand>> m_cand;
			std::vector<std::array<Cand, 2>> m_rpath;
			std::vector<int64_t> m_pongsIn;
			uint8_t m_rtt[4][4] = {}, m_relayRtt[4][4] = {};
			int m_dropped = 0;
		};
	} // namespace

	void StartPingTest(uint32_t session, const std::vector<std::vector<PeerAddr>>& byPosition, uint16_t port, int durationMs,
		const std::vector<BattleInfo::Relay>& relays)
	{
		if (g_ping)
			g_ping->stop = true;
		FinishPingTest();
		StopUdpTest();
		g_pingError.clear();
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
			g_pingError = "bind :" + std::to_string(port) + " failed (IPv4 and IPv6)";
			Log("ping test: " + g_pingError);
			return;
		}
		if (s4 == INVALID_SOCKET || s6 == INVALID_SOCKET)
			Log(std::string("ping test: no IPv") + (s4 == INVALID_SOCKET ? "4" : "6") + " socket on :" + std::to_string(port) + ", its candidates skipped");
		g_ping = std::make_unique<PingTest>();
		g_ping->result.assign(n, {});
		PingTest* t = g_ping.get();
		t->thread = std::thread([t, s4, s6, session, byPosition, me, durationMs, relays] {
			PingRun run(s4, s6, session, byPosition, me, relays);
			run.Run(t->stop, durationMs);
			run.Finish(*t);
		});
	}

	std::vector<PingResult> FinishPingTest(std::vector<RelayServerAddr>* servers, std::string* error)
	{
		if (servers)
			servers->clear();
		if (error)
			*error = g_pingError;
		g_pingError.clear();
		if (!g_ping)
			return {};
		if (g_ping->thread.joinable())
			g_ping->thread.join();
		if (servers)
			*servers = std::move(g_ping->servers);
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
		g_udpLines.clear();
		g_udpAddr = AddrString(stunIP, stunPort);
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
		int rtt = -1;
		ProtoPacket ping;
		ping.type = ProtoPing;
		for (int i = 0; i < 3 && pub.empty(); i++)
		{
			ping.timestamp = NowNanos();
			const std::vector<uint8_t> data = ProtoEncode(ping);
			sendto(s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
				reinterpret_cast<const sockaddr*>(&stun), sizeof(stun));
			const auto until = Clock::now() + std::chrono::milliseconds(200);
			while (pub.empty() && Clock::now() < until)
			{
				// a 50 ms silence is not the end of the try (it ended the whole try once)
				if (!WaitReadable(s, 50))
					continue;
				uint8_t buf[512];
				sockaddr_in from{};
				socklen_t fromLen = sizeof(from);
				const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
				ProtoPacket pong;
				if (n > 0 && SameAddr(from, stun) && ProtoDecode(buf, n, pong) && pong.type == ProtoPong)
				{
					pub = pong.publicAddr;
					rtt = static_cast<int>((NowNanos() - ping.timestamp) / 1000000);
				}
			}
		}
		closesocket(s);
		g_udpOpened = !pub.empty(); // no answer: the next lobby connection asks again
		if (!pub.empty())
			g_udpLines += "udp_addr=" + pub + "\nudp_rtt=" + std::to_string(rtt) + "\n"; // the lobby picks the live uplink by it
		if (!local.empty())
			g_udpLines += "udp_local=" + local + "\n";
		if (!pub6.empty())
			g_udpLines += "udp_addr6=" + pub6 + "\n";
		Log("udp: port " + std::to_string(port) + ", public " + (pub.empty() ? "unknown (no STUN answer from " + AddrString(stun) + ")" : pub) +
			", local " + local + ", IPv6 " + (pub6.empty() ? "none" : pub6));
		return g_udpLines;
	}

	std::string UdpTest(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort, std::string& summary, const std::atomic<bool>* stop)
	{
		const auto stopped = [stop] { return stop && stop->load(); };
		// zdxsv's STUN test socket is at stunPort + 1. On bindPort (the GGPO port, as flycast tests
		// GdxLocalPort): a "udptest" Ping makes the server answer from both sockets; the test socket's
		// Pong arrives only if the port takes packets from a source it never sent to (open). Then a
		// Ping to the test socket: the same mapped port as the main one = cone NAT, another = symmetric.
		summary = "unknown";
		sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in addr = MakeAddr(0, bindPort);
		if (s == INVALID_SOCKET || bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
		{
			if (s != INVALID_SOCKET)
				closesocket(s);
			summary = "unknown (port " + std::to_string(bindPort) + " busy)";
			Log("udp test: " + summary);
			return "nat=unknown\n";
		}
		const sockaddr_in stun = MakeAddr(stunIP, stunPort);
		const sockaddr_in test = MakeAddr(stunIP, static_cast<uint16_t>(stunPort + 1));
		std::string mapped, mappedTest;
		bool open = false;
		// sends ping to `to` (3 tries, 300 ms each) until `done`; Pongs from stun/test fill mapped/mappedTest/open
		const auto ask = [&](const sockaddr_in& to, const std::string& userId, const std::function<bool()>& done) {
			ProtoPacket ping;
			ping.type = ProtoPing;
			ping.pingUserId = userId;
			for (int i = 0; i < 3 && !done() && !stopped(); i++)
			{
				ping.timestamp = NowNanos();
				const std::vector<uint8_t> data = ProtoEncode(ping);
				sendto(s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
					reinterpret_cast<const sockaddr*>(&to), sizeof(to));
				const auto until = Clock::now() + std::chrono::milliseconds(300);
				while (!done() && !stopped() && Clock::now() < until)
				{
					if (!WaitReadable(s, 50))
						continue;
					uint8_t buf[512];
					sockaddr_in from{};
					socklen_t fromLen = sizeof(from);
					const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
					ProtoPacket pong;
					if (n <= 0 || !ProtoDecode(buf, n, pong) || pong.type != ProtoPong)
						continue;
					if (SameAddr(from, stun))
						mapped = pong.publicAddr;
					else if (SameAddr(from, test))
					{
						mappedTest = pong.publicAddr;
						if (userId == "udptest")
							open = true;
					}
				}
			}
		};
		// the test socket's Pong may come a little after the main one
		const auto start = Clock::now();
		ask(stun, "udptest", [&] { return open || (!mapped.empty() && Clock::now() - start > std::chrono::milliseconds(500)); });
		if (!mapped.empty() && !open)
			ask(test, "", [&] { return !mappedTest.empty(); });
		closesocket(s);

		uint32_t ip1 = 0, ip2 = 0;
		uint16_t port1 = 0, port2 = 0;
		std::string nat;
		if (stopped())
			nat = "unknown", summary = "unknown (test cancelled: port " + std::to_string(bindPort) + " taken by the ping test)";
		else if (mapped.empty())
			nat = "unknown", summary = "unknown (no STUN answer from " + AddrString(stun) + ")";
		else if (open)
			nat = "open", summary = "open (port " + std::to_string(bindPort) + " reachable from any source, public " + mapped + ")";
		else if (mappedTest.empty() || !ParseAddr(mapped, ip1, port1) || !ParseAddr(mappedTest, ip2, port2))
			nat = "unknown", summary = "unknown (no answer from the STUN test socket " + AddrString(test) + ")";
		else if (port1 == port2 && ip1 == ip2)
			nat = "cone", summary = "cone NAT (port " + std::to_string(bindPort) + " closed, same public " + mapped + " for both servers)";
		else
			nat = "symmetric", summary = "symmetric NAT (port " + std::to_string(bindPort) + " closed, public " + mapped + " / " + mappedTest + ")";
		Log("udp test: nat=" + nat + ": " + summary);
		return "nat=" + nat + "\n";
	}

	bool StartUdpTest(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort,
		std::function<void(const std::string& natLine, const std::string& summary)> done)
	{
		std::lock_guard lock(g_udpTestMtx);
		if (g_udpTestRunning)
			return false;
		g_udpTest.reset(); // a finished run: its thread ends at once
		g_udpTestRunning = true;
		g_udpTest = std::make_unique<UdpTestRun>();
		UdpTestRun* t = g_udpTest.get();
		t->thread = std::thread([t, stunIP, stunPort, bindPort, done = std::move(done)] {
			std::string summary;
			const std::string line = UdpTest(stunIP, stunPort, bindPort, summary, &t->stop);
			if (done && !t->stop)
				done(line, summary);
			g_udpTestRunning = false;
		});
		return true;
	}

	void StopUdpTest()
	{
		std::lock_guard lock(g_udpTestMtx);
		g_udpTest.reset();
	}

	static std::atomic<bool> g_stateLoaded{false};

	void LobbyOnStateLoaded()
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

	// Live spectating (Lobby.h LiveUp / LiveDown; the lobby side is inada-s/zdxsv pkg/lobby/spectator.go).
	namespace
	{
		// Packet.type and its message field (gdxsv's numbers)
		constexpr uint32_t LIVE_PUSH = 20, LIVE_ACK = 21, LIVE_SUBSCRIBE = 24, LIVE_CHALLENGE = 25;
		constexpr size_t LIVE_CHUNK = 1000;
		constexpr size_t LIVE_WINDOW = 32; // datagrams past the ack
		constexpr auto LIVE_RESEND = std::chrono::milliseconds(200);
		constexpr size_t LIVE_MAX_FRAMES_PER_PUSH = 128;
		constexpr auto LIVE_KEEPALIVE = std::chrono::seconds(2);
		constexpr auto LIVE_CLOSE_GIVEUP = std::chrono::seconds(30);
		constexpr size_t LIVE_COOKIE = 16;

		std::vector<uint8_t> LivePacket(uint32_t type, const std::vector<uint8_t>& msg)
		{
			std::vector<uint8_t> o;
			Pb::PutUint(o, 1, type);
			Pb::PutBytes(o, type, msg.data(), msg.size());
			return o;
		}

		// A spectator packet's type (0 = none) and message.
		uint32_t LiveParse(const uint8_t* p, size_t n, std::string_view& msg)
		{
			uint64_t type = 0;
			std::map<uint32_t, std::string_view> msgs;
			Pb::Reader rd{p, p + n};
			if (!rd.Fields([&](uint32_t field, uint32_t wt, uint64_t v, const uint8_t* b, size_t len) {
					if (field == 1 && wt == 0)
						type = v;
					else if (wt == 2)
						msgs[field] = std::string_view(reinterpret_cast<const char*>(b), len);
					return true;
				}))
				return 0;
			const auto it = msgs.find(static_cast<uint32_t>(type));
			if (it == msgs.end())
				return 0;
			msg = it->second;
			return static_cast<uint32_t>(type);
		}

		bool LiveFields(std::string_view msg, std::map<uint32_t, uint64_t>& nums, std::map<uint32_t, std::string_view>& bytes)
		{
			Pb::Reader rd{reinterpret_cast<const uint8_t*>(msg.data()), reinterpret_cast<const uint8_t*>(msg.data() + msg.size())};
			return rd.Fields([&](uint32_t field, uint32_t wt, uint64_t v, const uint8_t* b, size_t len) {
				if (wt == 0)
					nums[field] = v;
				else if (wt == 2)
					bytes[field] = std::string_view(reinterpret_cast<const char*>(b), len);
				return true;
			});
		}

		// The sender's go-back-N position in one stream (bytes or frames).
		struct LiveWindow
		{
			size_t acked = 0, next = 0;
			Clock::time_point progress{};
			void Ack(size_t n, Clock::time_point now)
			{
				if (acked < n)
				{
					acked = n;
					progress = now;
				}
				next = std::max(next, acked);
			}
			// Chunks of up to per units from next while next < have and within LIVE_WINDOW chunks of acked;
			// back to acked after LIVE_RESEND without progress.
			template <typename F>
			void Send(Clock::time_point now, size_t have, size_t per, F send)
			{
				if (next > acked && now - progress >= LIVE_RESEND)
				{
					next = acked;
					progress = now;
				}
				while (next < have && next < acked + LIVE_WINDOW * per)
				{
					const size_t n = std::min(per, have - next);
					if (next == acked)
						progress = now;
					send(next, n);
					next += n;
				}
			}
		};

		// "host:port" -> an IPv4 address.
		bool LiveResolve(const std::string& hostPort, sockaddr_in& out)
		{
#ifdef _WIN32
			static const bool wsa = [] {
				WSADATA d;
				return WSAStartup(MAKEWORD(2, 2), &d) == 0;
			}();
			if (!wsa)
				return false;
#endif
			const size_t colon = hostPort.rfind(':');
			if (colon == std::string::npos)
				return false;
			addrinfo hints{};
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_DGRAM;
			addrinfo* res = nullptr;
			if (getaddrinfo(hostPort.substr(0, colon).c_str(), hostPort.substr(colon + 1).c_str(), &hints, &res) != 0 || !res)
				return false;
			out = *reinterpret_cast<const sockaddr_in*>(res->ai_addr);
			freeaddrinfo(res);
			return true;
		}

		sock_t LiveSocket()
		{
			sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			sockaddr_in any{};
			any.sin_family = AF_INET;
			if (s != INVALID_SOCKET && bind(s, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) != 0)
			{
				closesocket(s);
				return INVALID_SOCKET;
			}
			return s;
		}

		void LiveSend(sock_t s, const sockaddr_in& to, uint32_t type, const std::vector<uint8_t>& msg)
		{
			const std::vector<uint8_t> data = LivePacket(type, msg);
			sendto(s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
		}

		// Receives one packet from `from` within ms; returns its spectator type (0 = none).
		uint32_t LiveRecv(sock_t s, const sockaddr_in& from, int ms, std::vector<uint8_t>& buf, std::string_view& msg)
		{
			if (!WaitReadable(s, ms))
				return 0;
			buf.resize(64 * 1024);
			sockaddr_in src{};
			socklen_t len = sizeof(src);
			const int n = recvfrom(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr*>(&src), &len);
			if (n <= 0 || !SameAddr(src, from))
				return 0;
			return LiveParse(buf.data(), static_cast<size_t>(n), msg);
		}
	} // namespace

	std::string LobbyUdpAddr()
	{
		return g_udpAddr;
	}

	struct LiveUp::Impl
	{
		sock_t s = INVALID_SOCKET;
		sockaddr_in to{};
		std::string code;
		uint32_t session = 0;
		int frameBytes = 0;
		std::mutex mtx;
		std::vector<uint8_t> header, state, inputs;
		bool stateSet = false, closeSet = false, headerAck = false, closeAck = false;
		std::string close;
		LiveWindow st, in;
		Clock::time_point headerSent{}, closeSent{}, closeAt{};
		std::atomic<bool> quit{false}, done{false};
		std::thread t;

		std::vector<uint8_t> Push() const
		{
			std::vector<uint8_t> m;
			Pb::PutString(m, 1, code);
			Pb::PutInt(m, 2, static_cast<int32_t>(session));
			return m;
		}

		void Run()
		{
			std::vector<uint8_t> buf;
			size_t sent = 0;
			while (!quit)
			{
				std::string_view msg;
				const uint32_t type = LiveRecv(s, to, 10, buf, msg);
				std::lock_guard lock(mtx);
				const Clock::time_point now = Clock::now();
				std::map<uint32_t, uint64_t> nums;
				std::map<uint32_t, std::string_view> bytes;
				if (type == LIVE_ACK && LiveFields(msg, nums, bytes) && bytes[1] == code)
				{
					headerAck = headerAck || nums[40] != 0;
					st.Ack(nums[41], now);
					in.Ack(nums[2], now);
					closeAck = closeAck || nums[42] != 0;
				}
				if (closeAck || (closeSet && now - closeAt > LIVE_CLOSE_GIVEUP))
				{
					Log("live: uplink " + code + (closeAck ? " closed: " : " gave up on the close: ") + std::to_string(in.acked) + " frames, state " +
						std::to_string(st.acked) + " bytes, " + std::to_string(sent) + " datagrams");
					break;
				}
				if (!headerAck && now - headerSent >= LIVE_RESEND)
				{
					std::vector<uint8_t> m = Push();
					Pb::PutBytes(m, 10, header.data(), header.size());
					LiveSend(s, to, LIVE_PUSH, m);
					headerSent = now;
					sent++;
				}
				if (stateSet)
					st.Send(now, state.size(), LIVE_CHUNK, [&](size_t off, size_t n) {
						std::vector<uint8_t> m = Push();
						Pb::PutBytes(m, 42, state.data() + off, n);
						Pb::PutInt(m, 43, static_cast<int64_t>(off));
						Pb::PutInt(m, 44, static_cast<int64_t>(state.size()));
						LiveSend(s, to, LIVE_PUSH, m);
						sent++;
					});
				const size_t fb = static_cast<size_t>(frameBytes);
				const size_t frames = inputs.size() / fb;
				in.Send(now, frames, std::min(LIVE_MAX_FRAMES_PER_PUSH, LIVE_CHUNK / fb), [&](size_t f, size_t n) {
					std::vector<uint8_t> m = Push();
					Pb::PutInt(m, 3, static_cast<int64_t>(f));
					Pb::PutBytes(m, 40, inputs.data() + f * fb, n * fb);
					Pb::PutInt(m, 41, frameBytes);
					LiveSend(s, to, LIVE_PUSH, m);
					sent++;
				});
				if (closeSet && headerAck && stateSet && st.acked == state.size() && in.acked == frames && now - closeSent >= LIVE_RESEND)
				{
					std::vector<uint8_t> m = Push();
					Pb::PutInt(m, 3, static_cast<int64_t>(frames));
					Pb::PutString(m, 8, close);
					Pb::PutInt(m, 41, frameBytes);
					LiveSend(s, to, LIVE_PUSH, m);
					closeSent = now;
					sent++;
				}
			}
			done = true;
		}
	};

	LiveUp::LiveUp(const std::string& to, std::string code, uint32_t session, std::vector<uint8_t> header, int frameBytes)
		: m(std::make_unique<Impl>())
	{
		m->code = std::move(code);
		m->session = session;
		m->header = std::move(header);
		m->frameBytes = std::max(frameBytes, 1);
		if (!LiveResolve(to, m->to) || (m->s = LiveSocket()) == INVALID_SOCKET)
		{
			Log("live: uplink to " + to + " failed: no socket");
			m->done = true;
			return;
		}
		Log("live: uplink " + m->code + " to " + to);
		m->t = std::thread([this] { m->Run(); });
	}

	LiveUp::~LiveUp()
	{
		m->quit = true;
		if (m->t.joinable())
			m->t.join();
		if (m->s != INVALID_SOCKET)
			closesocket(m->s);
	}

	void LiveUp::SetState(std::vector<uint8_t> state)
	{
		std::lock_guard lock(m->mtx);
		m->state = std::move(state);
		m->stateSet = true;
	}

	void LiveUp::AddFrames(const void* data, size_t frames)
	{
		std::lock_guard lock(m->mtx);
		const uint8_t* p = static_cast<const uint8_t*>(data);
		m->inputs.insert(m->inputs.end(), p, p + frames * m->frameBytes);
	}

	size_t LiveUp::Frames()
	{
		std::lock_guard lock(m->mtx);
		return m->inputs.size() / m->frameBytes;
	}

	void LiveUp::Close(const std::string& reason)
	{
		std::lock_guard lock(m->mtx);
		if (std::exchange(m->closeSet, true))
			return;
		m->close = reason.empty() ? "end" : reason;
		m->closeAt = Clock::now();
	}

	bool LiveUp::Done()
	{
		return m->done;
	}

	struct LiveDown::Impl
	{
		sock_t s = INVALID_SOCKET;
		sockaddr_in to{};
		std::mutex mtx;
		std::string code;
		std::vector<uint8_t> cookie = std::vector<uint8_t>(LIVE_COOKIE);
		LiveStreams got; // header + state until the first Take, then the frames not taken yet
		bool headerTaken = false;
		size_t frames = 0; // received in a row
		Clock::time_point rx = Clock::now(), subscribed{};
		std::atomic<bool> quit{false};
		std::thread t;

		void Subscribe(Clock::time_point now)
		{
			std::vector<uint8_t> m;
			Pb::PutString(m, 1, code);
			Pb::PutInt(m, 3, static_cast<int64_t>(frames));
			Pb::PutBytes(m, 4, cookie.data(), cookie.size());
			LiveSend(s, to, LIVE_SUBSCRIBE, m);
			subscribed = now;
		}

		void Run()
		{
			std::vector<uint8_t> buf;
			while (!quit)
			{
				std::string_view msg;
				const uint32_t type = LiveRecv(s, to, 10, buf, msg);
				std::lock_guard lock(mtx);
				const Clock::time_point now = Clock::now();
				std::map<uint32_t, uint64_t> nums;
				std::map<uint32_t, std::string_view> bytes;
				if (type == LIVE_CHALLENGE && LiveFields(msg, nums, bytes) && bytes[2].size() == LIVE_COOKIE && (code.empty() || bytes[1] == code))
				{
					code = bytes[1];
					cookie.assign(bytes[2].begin(), bytes[2].end());
					Subscribe(now);
				}
				else if (type == LIVE_PUSH && LiveFields(msg, nums, bytes) && !code.empty() && bytes[1] == code)
				{
					rx = now;
					LiveStreams& g = got;
					if (const auto h = bytes.find(10); h != bytes.end() && !headerTaken && g.header.empty())
						g.header.assign(h->second.begin(), h->second.end());
					if (const size_t total = nums[44]; total > 0 && (g.stateTotal == 0 || g.stateTotal == total))
					{
						g.stateTotal = total;
						const std::string_view chunk = bytes[42];
						if (nums[43] == g.state.size() && g.state.size() + chunk.size() <= total)
							g.state.insert(g.state.end(), chunk.begin(), chunk.end());
					}
					const size_t fb = nums[41];
					if (fb > 0 && (g.frameBytes == 0 || static_cast<size_t>(g.frameBytes) == fb))
					{
						g.frameBytes = static_cast<int>(fb);
						const std::string_view in = bytes[40];
						if (in.size() % fb == 0 && nums[3] == frames)
						{
							g.inputs.insert(g.inputs.end(), in.begin(), in.end());
							frames += in.size() / fb;
						}
						if (bytes.count(8) && nums[3] == frames && !g.closed)
						{
							g.closed = true;
							g.close = bytes[8];
						}
					}
					std::vector<uint8_t> ack;
					Pb::PutString(ack, 1, code);
					Pb::PutInt(ack, 2, static_cast<int64_t>(frames));
					Pb::PutInt(ack, 40, headerTaken || !g.header.empty() ? 1 : 0);
					Pb::PutInt(ack, 41, static_cast<int64_t>(headerTaken ? g.stateTotal : g.state.size()));
					Pb::PutInt(ack, 42, g.closed ? 1 : 0);
					LiveSend(s, to, LIVE_ACK, ack);
				}
				if (!got.closed && now - subscribed >= LIVE_KEEPALIVE)
					Subscribe(now);
			}
		}
	};

	LiveDown::LiveDown()
		: m(std::make_unique<Impl>())
	{
	}

	LiveDown::~LiveDown()
	{
		m->quit = true;
		if (m->t.joinable())
			m->t.join();
		if (m->s != INVALID_SOCKET)
			closesocket(m->s);
	}

	std::unique_ptr<LiveDown> LiveDown::Open(const std::string& url, int timeoutMs, std::string& error)
	{
		std::unique_ptr<LiveDown> d(new LiveDown());
		const size_t slash = url.find('/');
		const std::string hostPort = url.substr(0, slash);
		d->m->code = slash == std::string::npos ? std::string() : url.substr(slash + 1);
		if (!LiveResolve(hostPort, d->m->to) || (d->m->s = LiveSocket()) == INVALID_SOCKET)
		{
			error = "cannot resolve " + hostPort + " or open a socket";
			return nullptr;
		}
		d->m->Subscribe(Clock::now());
		d->m->t = std::thread([m = d->m.get()] { m->Run(); });
		const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
		while (Clock::now() < until)
		{
			{
				std::lock_guard lock(d->m->mtx);
				const LiveStreams& g = d->m->got;
				if (!g.header.empty() && g.stateTotal > 0 && g.state.size() == g.stateTotal && (d->m->frames > 0 || g.closed))
					return d;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		std::lock_guard lock(d->m->mtx);
		const LiveStreams& g = d->m->got;
		error = d->m->code.empty() ? "no live battle there (no challenge)" :
									 "battle " + d->m->code + ": header " + std::to_string(g.header.size()) + " bytes, state " +
										 std::to_string(g.state.size()) + "/" + std::to_string(g.stateTotal) + ", " +
										 std::to_string(d->m->frames) + " frames after " + std::to_string(timeoutMs) + " ms";
		return nullptr;
	}

	std::string LiveDown::Code()
	{
		std::lock_guard lock(m->mtx);
		return m->code;
	}

	bool LiveDown::Take(LiveStreams& s, int stallMs)
	{
		std::lock_guard lock(m->mtx);
		LiveStreams& g = m->got;
		if (!m->headerTaken)
		{
			m->headerTaken = true;
			s.header = std::move(g.header);
			s.state = std::move(g.state);
			s.stateTotal = g.stateTotal;
		}
		s.inputs.insert(s.inputs.end(), g.inputs.begin(), g.inputs.end());
		g.inputs.clear();
		s.frameBytes = g.frameBytes;
		s.closed = g.closed;
		s.close = g.close;
		return g.closed || Clock::now() - m->rx < std::chrono::milliseconds(stallMs);
	}
} // namespace Zdxsv

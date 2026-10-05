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
				uint32_t ip;
				uint16_t port;
				if (ParseAddr(a, ip, port))
					peer.addrs.emplace_back(ip, port);
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

	bool GgpoPeers(const BattleInfo& info, uint32_t ownPublicIP, std::vector<std::pair<uint32_t, uint16_t>>& byPosition)
	{
		byPosition.assign(info.users.size(), {0, 0});
		for (size_t i = 0; i < info.users.size(); i++)
		{
			const std::string& u = info.users[i];
			if (u == info.userId)
				continue;
			const auto port = info.ggpo.find(u);
			const auto peer = std::find_if(info.p2p.begin(), info.p2p.end(), [&u](const BattleInfo::Peer& p) { return p.userId == u; });
			if (port == info.ggpo.end() || peer == info.p2p.end())
				return false;
			const uint32_t ip = peer->addrs.front().first == ownPublicIP ? peer->addrs.back().first : peer->addrs.front().first;
			byPosition[i] = {ip, port->second};
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
			std::vector<int> rtt;
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
	} // namespace

	void StartPingTest(uint32_t session, const std::vector<std::pair<uint32_t, uint16_t>>& byPosition, uint16_t port, int durationMs)
	{
		FinishPingTest();
		const int n = static_cast<int>(byPosition.size());
		int me = -1;
		for (int p = 0; p < n; p++)
			if (byPosition[p].second == 0)
				me = p;
		if (me < 0 || n > 4)
			return;
		sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		const sockaddr_in any = MakeAddr(0, port);
		if (s == INVALID_SOCKET || bind(s, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) != 0)
		{
			if (s != INVALID_SOCKET)
				closesocket(s);
			Log("ping test: bind :" + std::to_string(port) + " failed");
			return;
		}
		g_ping = std::make_unique<PingTest>();
		g_ping->rtt.assign(n, -1);
		PingTest* t = g_ping.get();
		t->thread = std::thread([t, s, session, byPosition, me, n, durationMs] {
			std::vector<sockaddr_in> peer(n);
			for (int p = 0; p < n; p++)
				peer[p] = MakeAddr(byPosition[p].first, byPosition[p].second);
			std::vector<int64_t> sum(n, 0), pongs(n, 0), pongsIn(n, 0);
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
						if (p == me)
							continue;
						PingPacket pk{};
						pk.magic = PING_MAGIC;
						pk.sessionId = session;
						pk.type = PING_TYPE;
						pk.fromPeer = static_cast<uint8_t>(me);
						pk.toPeer = static_cast<uint8_t>(p);
						pk.sendTimestamp = NowMs();
						sendto(s, reinterpret_cast<const char*>(&pk), sizeof(pk), 0, reinterpret_cast<const sockaddr*>(&peer[p]), sizeof(peer[p]));
					}
				}
				if (!WaitReadable(s, 10))
					continue;
				PingPacket pk;
				sockaddr_in from{};
				socklen_t len = sizeof(from);
				// Windows: an ICMP port unreachable (peer not bound yet) fails this recv, nothing to read
				if (recvfrom(s, reinterpret_cast<char*>(&pk), sizeof(pk), 0, reinterpret_cast<sockaddr*>(&from), &len) != sizeof(pk))
					continue;
				if (pk.magic != PING_MAGIC || pk.sessionId != session || pk.toPeer != me || pk.fromPeer >= n ||
					pk.fromPeer == me || !SameAddr(from, peer[pk.fromPeer]))
				{
					dropped++;
					continue;
				}
				if (pk.type == PING_TYPE)
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
					sendto(s, reinterpret_cast<const char*>(&pong), sizeof(pong), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
					pongsIn[pk.fromPeer]++;
				}
				else if (pk.type == PONG_TYPE)
				{
					sum[pk.fromPeer] += std::max<int64_t>(1, static_cast<int64_t>(NowMs() - pk.pingTimestamp));
					pongs[pk.fromPeer]++;
				}
			}
			closesocket(s);
			std::string line = "ping test: session " + std::to_string(session) + ", position " + std::to_string(me);
			for (int p = 0; p < n; p++)
			{
				if (p == me)
					continue;
				t->rtt[p] = pongs[p] ? static_cast<int>((sum[p] + pongs[p] - 1) / pongs[p]) : -1;
				line += "; peer " + std::to_string(p) + " " + AddrString(peer[p]) + " rtt " + std::to_string(t->rtt[p]) + " ms (" +
						std::to_string(pongs[p]) + " pongs, " + std::to_string(pongsIn[p]) + " pings answered)";
			}
			Log(line + ", " + std::to_string(dropped) + " dropped");
		});
	}

	std::vector<int> FinishPingTest()
	{
		if (!g_ping)
			return {};
		if (g_ping->thread.joinable())
			g_ping->thread.join();
		std::vector<int> rtt = std::move(g_ping->rtt);
		g_ping.reset();
		return rtt;
	}

	uint32_t PublicIP()
	{
		const std::string& lines = g_udpLines;
		const size_t at = lines.find("udp_addr=");
		uint32_t ip = 0;
		uint16_t port = 0;
		if (at == std::string::npos || !ParseAddr(lines.substr(at + 9, lines.find('\n', at) - at - 9), ip, port))
			return 0;
		return ip;
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
		Log("udp: port " + std::to_string(port) + ", public " + (pub.empty() ? "unknown (no STUN answer from " + AddrString(stun) + ")" : pub) +
			", local " + local);
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

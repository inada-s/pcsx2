// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ZdxsvBridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
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
#include <netinet/tcp.h>
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
		bool g_armed = false;

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

	bool Enabled()
	{
		const char* env = std::getenv("ZDXSV_UDP");
		return !(env && std::string(env) == "0");
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
		out = std::move(info);
		return true;
	}

	void SetBattleInfo(const BattleInfo& info)
	{
		{
			std::lock_guard lock(g_mtx);
			g_info = info;
			g_armed = true;
		}
		Log("battle info: user " + info.userId + ", " + std::to_string(info.users.size()) + " players, server " +
			AddrString(info.serverIP, info.serverPort) + ", " + std::to_string(info.p2p.size()) + " p2p peers");
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

	// ---- protobuf (proto2) codec for zdxsv.proto ----
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
			if (pkt.type == Battle)
			{
				PutUint(o, 2, pkt.seq);
				PutUint(o, 3, pkt.ack);
			}
			if (pkt.type == HelloServer)
			{
				std::vector<uint8_t> h;
				PutString(h, 1, pkt.helloSessionId);
				PutBytes(o, 10, h.data(), h.size());
			}
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
			for (const BattleMessage& m : pkt.battle)
			{
				std::vector<uint8_t> b;
				PutString(b, 1, m.userId);
				PutUint(b, 2, m.seq);
				PutBytes(b, 3, m.body.data(), m.body.size());
				PutBytes(o, 13, b.data(), b.size());
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
				else if (wt == 0 && field == 2)
					pkt.seq = static_cast<uint32_t>(v);
				else if (wt == 0 && field == 3)
					pkt.ack = static_cast<uint32_t>(v);
				else if (wt == 2 && field == 10)
				{
					Reader h{bytes, bytes + n};
					return h.Fields([&](uint32_t f, uint32_t w, uint64_t hv, const uint8_t* hb, size_t hn) {
						if (w == 2 && f == 1)
							pkt.helloSessionId.assign(reinterpret_cast<const char*>(hb), hn);
						else if (w == 0 && f == 2)
							pkt.helloOk = hv != 0;
						return true;
					});
				}
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
				else if (wt == 2 && field == 13)
				{
					BattleMessage m;
					Reader b{bytes, bytes + n};
					if (!b.Fields([&](uint32_t f, uint32_t w, uint64_t bv, const uint8_t* bb, size_t bn) {
							if (w == 2 && f == 1)
								m.userId.assign(reinterpret_cast<const char*>(bb), bn);
							else if (w == 0 && f == 2)
								m.seq = static_cast<uint32_t>(bv);
							else if (w == 2 && f == 3)
								m.body.assign(bb, bb + bn);
							return true;
						}))
						return false;
					pkt.battle.push_back(std::move(m));
				}
				return true;
			});
		}
	} // namespace Proto

	// ---- the UDP socket and the bridge ----
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

		// The bridge's UDP socket, opened at lobby connect so its address can go
		// to peers in the battle info. Keeps the NAT mapping alive like zproxy,
		// which pings the STUN every 5 s; the pongs are dropped by the next bridge.
		class UdpSocket
		{
		public:
			const sock_t s;
			const sockaddr_in stun;
			const std::string lines; // platform info lines
			UdpSocket(sock_t sock, sockaddr_in stunAddr, std::string infoLines)
				: s(sock)
				, stun(stunAddr)
				, lines(std::move(infoLines))
			{
				keeper = std::thread([this] {
					std::unique_lock lock(m);
					while (!cv.wait_for(lock, std::chrono::seconds(10), [this] { return quit; }))
					{
						Proto::Packet ping;
						ping.type = Proto::Ping;
						ping.timestamp = NowNanos();
						const std::vector<uint8_t> data = Proto::Encode(ping);
						sendto(s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
							reinterpret_cast<const sockaddr*>(&stun), sizeof(stun));
					}
				});
			}
			~UdpSocket()
			{
				{
					std::lock_guard lock(m);
					quit = true;
				}
				cv.notify_all();
				keeper.join();
				closesocket(s);
			}

		private:
			std::thread keeper;
			std::mutex m;
			std::condition_variable cv;
			bool quit = false;
		};
		std::unique_ptr<UdpSocket> g_udp; // DEV9 thread only

		class Bridge
		{
		public:
			// udp: the socket from OpenUdp (not owned), or INVALID_SOCKET: own one.
			Bridge(sock_t listener, BattleInfo info, sock_t udp)
				: listener(listener)
				, info(std::move(info))
				, udp(udp)
			{
				thread = std::thread([this] { Run(); });
			}
			~Bridge()
			{
				stop = true;
				if (thread.joinable())
					thread.join();
			}

		private:
			// zdxsv/pkg/proto BattleBuffer: unacked messages to one destination,
			// resent until acked. links[0] is the battle server (always up); the
			// rest are peers, up once one of their candidates answers a ping.
			struct Link
			{
				std::string userId; // "" = battle server
				sockaddr_in addr{};
				std::vector<sockaddr_in> candidates;
				bool up = false;
				std::deque<Proto::BattleMessage> pending;
				uint32_t begin = 1; // seq of pending.front()
				uint32_t end = 1; // next seq
				uint32_t recvAck = 0; // last seq received on this link
				uint64_t sentPkts = 0, recvPkts = 0;
				uint64_t firstMsgs = 0; // messages this link delivered before any other
				int64_t rttMs = -1;
			};
			sock_t listener;
			BattleInfo info;
			sock_t udp;
			bool ownUdp = false;
			std::atomic<bool> stop{false};
			std::thread thread;
			sock_t tcp = INVALID_SOCKET;
			std::vector<Link> links;
			bool greeted = false, refused = false;
			// MessageFilter: per-sender next-in-order check, first message any seq
			// (as zdxsv proto.MessageFilter): the server can drop a sender's seq 1
			// (bridge_test e2e s572: seq 1 never relayed, first seen = 2). A peer
			// link resends from its seq 1, so P2P only loses seq 1 if the relayed
			// seq 2 wins the race, as on the relay alone.
			std::map<std::string, uint32_t> recvSeq;
			uint32_t msgSeq = 1;
			uint64_t sentMsgs = 0, recvMsgs = 0;
			// ZDXSV_UDP_TEST_DROP=N: drop every Nth UDP packet, both ways (tests the resend path).
			int dropEvery = 0;
			uint64_t udpCount = 0, dropped = 0;
			// ZDXSV_UDP_TEST_P2P_ONLY=1: ignore battle data from the server (tests P2P alone).
			bool p2pOnly = false;
			// ZDXSV_UDP_TEST_P2P_BLOCK=1: drop all peer packets, both ways (a NAT
			// that lets nothing through: every message must go via the server).
			bool p2pBlock = false;
			uint64_t blocked = 0;
			// ZDXSV_UDP_TEST_P2P_DELAY=ms: hold packets to peers ms before sending
			// (a slow direct path; ~16 ms granularity, the Serve tick).
			int p2pDelayMs = 0;
			struct Delayed
			{
				Clock::time_point due;
				sockaddr_in to;
				std::vector<uint8_t> data;
			};
			std::deque<Delayed> delayed;

			bool IsServer(const sockaddr_in& a) { return SameAddr(a, links[0].addr); }

			void SendRaw(const std::vector<uint8_t>& data, const sockaddr_in& to)
			{
				sendto(udp, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
					reinterpret_cast<const sockaddr*>(&to), sizeof(to));
			}

			void SendDelayed()
			{
				const auto now = Clock::now();
				while (!delayed.empty() && delayed.front().due <= now)
				{
					SendRaw(delayed.front().data, delayed.front().to);
					delayed.pop_front();
				}
			}

			bool WriteTCP(const uint8_t* p, size_t n)
			{
				while (n > 0)
				{
					const int ret = send(tcp, reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
					if (ret <= 0)
						return false;
					p += ret;
					n -= ret;
				}
				return true;
			}
			bool Drop()
			{
				if (dropEvery <= 0 || ++udpCount % dropEvery != 0)
					return false;
				dropped++;
				return true;
			}
			void SendUDP(const Proto::Packet& pkt, const sockaddr_in& to)
			{
				if (Drop())
					return;
				if (p2pBlock && !IsServer(to))
				{
					blocked++;
					return;
				}
				if (p2pDelayMs > 0 && !IsServer(to))
				{
					delayed.push_back({Clock::now() + std::chrono::milliseconds(p2pDelayMs), to, Proto::Encode(pkt)});
					return;
				}
				SendRaw(Proto::Encode(pkt), to);
			}
			Link* FindLink(const sockaddr_in& from)
			{
				for (Link& l : links)
					if (l.up && SameAddr(l.addr, from))
						return &l;
				return nullptr;
			}
			// Reads and handles one UDP packet if one comes within ms.
			void Poll(int ms)
			{
				SendDelayed();
				if (!WaitReadable(udp, ms))
					return;
				uint8_t buf[4096];
				sockaddr_in from{};
				socklen_t fromLen = sizeof(from);
				const int n = recvfrom(udp, reinterpret_cast<char*>(buf), sizeof(buf), 0,
					reinterpret_cast<sockaddr*>(&from), &fromLen);
				Proto::Packet pkt;
				if (n <= 0 || Drop() || !Proto::Decode(buf, n, pkt))
					return;
				if (p2pBlock && !IsServer(from))
				{
					blocked++;
					return;
				}
				if (pkt.type == Proto::HelloServer && SameAddr(from, links[0].addr))
				{
					greeted = pkt.helloOk;
					refused = !pkt.helloOk;
				}
				else if (pkt.type == Proto::Battle)
				{
					if (Link* l = FindLink(from); l && greeted && !(p2pOnly && l == &links[0]))
						OnBattlePacket(*l, pkt);
				}
				else if (pkt.type == Proto::Ping && !pkt.pingUserId.empty())
				{
					Proto::Packet pong;
					pong.type = Proto::Pong;
					pong.timestamp = pkt.timestamp;
					pong.pingUserId = info.userId;
					SendUDP(pong, from);
				}
				else if (pkt.type == Proto::Pong)
				{
					for (Link& l : links)
					{
						if (l.up || l.userId.empty() || l.userId != pkt.pingUserId)
							continue;
						for (const sockaddr_in& c : l.candidates)
							if (SameAddr(c, from))
							{
								l.up = true;
								l.addr = from;
								l.rttMs = (NowNanos() - pkt.timestamp) / 1000000;
								Log("p2p " + l.userId + " up at " + AddrString(from) + ", rtt " + std::to_string(l.rttMs) + " ms");
							}
					}
				}
			}
			void PingPeers()
			{
				Proto::Packet ping;
				ping.type = Proto::Ping;
				ping.timestamp = NowNanos();
				ping.pingUserId = info.userId;
				for (const Link& l : links)
					if (!l.up && !l.userId.empty())
						for (const sockaddr_in& c : l.candidates)
							SendUDP(ping, c);
			}
			bool Greet()
			{
				Proto::Packet hello;
				hello.type = Proto::HelloServer;
				hello.helloSessionId = info.sessionId;
				// zproxy GreetBattleServer: 10 tries, 100 ms apart
				for (int i = 0; i < 10 && !stop; i++)
				{
					SendUDP(hello, links[0].addr);
					PingPeers();
					const auto until = Clock::now() + std::chrono::milliseconds(100);
					while (Clock::now() < until && !greeted && !refused)
						Poll(std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count())));
					if (refused)
						Log("battle server refused session");
					if (greeted || refused)
						return greeted;
				}
				return false;
			}
			void Flush(Link& l)
			{
				Proto::Packet pkt;
				pkt.type = Proto::Battle;
				const size_t n = std::min<size_t>(l.pending.size(), 50);
				pkt.battle.assign(l.pending.begin(), l.pending.begin() + n);
				pkt.seq = l.begin + static_cast<uint32_t>(n) - 1;
				pkt.ack = l.recvAck;
				SendUDP(pkt, l.addr);
				l.sentPkts++;
			}
			void OnBattlePacket(Link& l, const Proto::Packet& pkt)
			{
				l.recvPkts++;
				l.recvAck = std::max(l.recvAck, pkt.seq);
				while (!l.pending.empty() && l.begin <= pkt.ack)
				{
					l.pending.pop_front();
					l.begin++;
				}
				for (const Proto::BattleMessage& m : pkt.battle)
				{
					auto it = recvSeq.find(m.userId);
					if (it == recvSeq.end() || !(it->second == 0 || m.seq == it->second + 1))
						continue;
					it->second = m.seq;
					recvMsgs++;
					l.firstMsgs++;
					if (!WriteTCP(m.body.data(), m.body.size()))
						stop = true;
				}
			}
			void Run()
			{
				Log("bridge listening");
				if (const char* env = std::getenv("ZDXSV_UDP_TEST_DROP"))
					dropEvery = std::atoi(env);
				if (const char* env = std::getenv("ZDXSV_UDP_TEST_P2P_ONLY"))
					p2pOnly = std::string(env) == "1";
				if (const char* env = std::getenv("ZDXSV_UDP_TEST_P2P_BLOCK"))
					p2pBlock = std::string(env) == "1";
				if (const char* env = std::getenv("ZDXSV_UDP_TEST_P2P_DELAY"))
					p2pDelayMs = std::atoi(env);
				if (p2pBlock || p2pDelayMs > 0)
					Log("bridge test: p2p block " + std::to_string(p2pBlock) + ", p2p delay " + std::to_string(p2pDelayMs) + " ms");
				const auto acceptUntil = Clock::now() + std::chrono::seconds(10);
				while (!stop && Clock::now() < acceptUntil && tcp == INVALID_SOCKET)
					if (WaitReadable(listener, 100))
						tcp = accept(listener, nullptr, nullptr);
				closesocket(listener);
				if (tcp == INVALID_SOCKET)
				{
					Log("bridge: game did not connect");
					return;
				}
				const int one = 1;
				setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
				if (udp == INVALID_SOCKET)
				{
					ownUdp = true;
					udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
					sockaddr_in any{};
					any.sin_family = AF_INET;
					bind(udp, reinterpret_cast<const sockaddr*>(&any), sizeof(any));
				}
				else
				{
					// stale packets: STUN keepalive pongs, a previous battle, early peer pings
					uint8_t buf[4096];
					while (WaitReadable(udp, 0) && recv(udp, reinterpret_cast<char*>(buf), sizeof(buf), 0) >= 0)
						;
				}
				links.emplace_back();
				links[0].addr = MakeAddr(info.serverIP, info.serverPort);
				links[0].up = true;
				for (const BattleInfo::Peer& p : info.p2p)
				{
					Link l;
					l.userId = p.userId;
					for (const auto& [ip, port] : p.addrs)
						l.candidates.push_back(MakeAddr(ip, port));
					links.push_back(std::move(l));
				}
				for (const std::string& id : info.users)
					if (id != info.userId)
						recvSeq[id] = 0;
				if (!Greet())
					Log("bridge: no HelloServer ok from " + AddrString(links[0].addr));
				else
				{
					Log("bridge: battle server ok");
					Serve();
				}
				std::string peers;
				for (const Link& l : links)
				{
					if (l.userId.empty())
						continue;
					peers += ", p2p " + l.userId + (l.up ? " up rtt " + std::to_string(l.rttMs) + " ms" : " down") +
							 " sent " + std::to_string(l.sentPkts) + " / recv " + std::to_string(l.recvPkts) +
							 " pkts, first " + std::to_string(l.firstMsgs) + " msgs";
				}
				Log("bridge end: sent " + std::to_string(sentMsgs) + " msgs / " + std::to_string(links[0].sentPkts) +
					" pkts, received " + std::to_string(recvMsgs) + " msgs / " + std::to_string(links[0].recvPkts) + " pkts" +
					", server first " + std::to_string(links[0].firstMsgs) + " msgs" + peers +
					(dropEvery > 0 ? ", test-dropped " + std::to_string(dropped) + " pkts" : "") +
					(p2pBlock ? ", test-blocked " + std::to_string(blocked) + " pkts" : ""));
				if (ownUdp)
					closesocket(udp);
				closesocket(tcp);
			}
			void Serve()
			{
				// What the battle server's TCP side sends first (zproxy firstData).
				static const uint8_t firstData[] = {0x28, 0x01, 0x10, 0x31, 0x00, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff, 0xff};
				if (!WriteTCP(firstData, sizeof(firstData)))
					return;
				const auto start = Clock::now();
				auto lastSend = start, lastPing = start, lastStatus = start;
				auto lastGame = start;
				bool fin = false;
				while (!stop && !fin)
				{
					fd_set rd;
					FD_ZERO(&rd);
					FD_SET(tcp, &rd);
					FD_SET(udp, &rd);
					timeval tv{0, 16000};
					const int maxfd = static_cast<int>(std::max(tcp, udp));
					if (select(maxfd + 1, &rd, nullptr, nullptr, &tv) < 0)
						return;
					bool flush = false;
					if (FD_ISSET(tcp, &rd))
					{
						uint8_t buf[1024];
						const int n = recv(tcp, reinterpret_cast<char*>(buf), sizeof(buf), 0);
						if (n <= 0)
							return;
						lastGame = Clock::now();
						Proto::BattleMessage m;
						m.userId = info.userId;
						m.seq = msgSeq++;
						m.body.assign(buf, buf + n);
						fin = n == 4 && buf[0] == 0x04 && buf[1] == 0xF0 && buf[2] == 0x00 && buf[3] == 0x00;
						// Every link gets every message (a peer that comes up late
						// catches up from its queue); peers that never answer are
						// dropped after 10 s. Server only: seq 1 (the 22-byte session
						// handshake, the server joins the room on it) and fin; the
						// server relays neither (udp_peer.go).
						const bool serverOnly = m.seq == 1 || fin;
						for (size_t i = 0; i < (serverOnly ? 1 : links.size()); i++)
						{
							links[i].pending.push_back(m);
							links[i].end++;
						}
						sentMsgs++;
						flush = true;
					}
					if (FD_ISSET(udp, &rd))
						Poll(0);
					SendDelayed();
					const auto now = Clock::now();
					if (now - lastPing >= std::chrono::milliseconds(100))
					{
						lastPing = now;
						if (now - start < std::chrono::seconds(10))
							PingPeers();
						else
							for (size_t i = 1; i < links.size();)
								if (!links[i].up)
								{
									Log("p2p " + links[i].userId + " never answered");
									links.erase(links.begin() + i);
								}
								else
									i++;
					}
					if (flush || now - lastSend >= std::chrono::milliseconds(30))
					{
						for (Link& l : links)
							if (l.up)
								Flush(l);
						lastSend = now;
					}
					if (now - lastStatus >= std::chrono::seconds(10))
					{
						lastStatus = now;
						std::string s = "bridge status: sent " + std::to_string(sentMsgs) + " / received " + std::to_string(recvMsgs) + " msgs";
						for (const Link& l : links)
							s += ", " + (l.userId.empty() ? std::string("server") : l.userId) + " pkts " + std::to_string(l.sentPkts) + "/" +
								 std::to_string(l.recvPkts) + " pending " + std::to_string(l.pending.size()) + " ack " + std::to_string(l.recvAck);
						for (const auto& [id, seq] : recvSeq)
							s += ", seq " + id + " " + std::to_string(seq);
						Log(s);
					}
					if (now - lastGame > std::chrono::seconds(10))
					{
						Log("bridge: game idle 10 s");
						return;
					}
				}
			}
		};

		std::unique_ptr<Bridge> g_bridge;
	} // namespace

	bool RedirectConnect(uint32_t ip, uint16_t port, uint16_t& bridgePort)
	{
		BattleInfo info;
		{
			std::lock_guard lock(g_mtx);
			if (!g_armed || ip != g_info.serverIP || port != g_info.serverPort)
				return false;
			g_armed = false;
			info = g_info;
		}
		sock_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t len = sizeof(addr);
		if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
			listen(listener, 1) != 0 || getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
		{
			if (listener != INVALID_SOCKET)
				closesocket(listener);
			Log("bridge: listen failed, game uses TCP");
			return false;
		}
		bridgePort = ntohs(addr.sin_port);
		g_bridge.reset(); // a previous battle's bridge, if still running
		g_bridge = std::make_unique<Bridge>(listener, std::move(info), g_udp ? g_udp->s : INVALID_SOCKET);
		return true;
	}

	std::string OpenUdp(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort)
	{
		// One socket for the emulator's life: a reconnect to the lobby (after a
		// battle) reports the same address.
		if (g_udp)
			return g_udp->lines;
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
			Log("udp: socket failed, relay only");
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
		std::string lines;
		if (!pub.empty())
			lines += "udp_addr=" + pub + "\n";
		if (!local.empty())
			lines += "udp_local=" + local + "\n";
		g_udp = std::make_unique<UdpSocket>(s, stun, std::move(lines));
		Log("udp: port " + std::to_string(port) + ", public " + (pub.empty() ? "unknown (no STUN answer from " + AddrString(stun) + ")" : pub) +
			", local " + local);
		return g_udp->lines;
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

	void Shutdown()
	{
		g_bridge.reset();
		g_udp.reset();
		std::lock_guard lock(g_mtx);
		g_armed = false;
	}
} // namespace Zdxsv

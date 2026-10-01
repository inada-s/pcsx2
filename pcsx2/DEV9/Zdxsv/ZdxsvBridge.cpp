// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ZdxsvBridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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
		const std::string server = kv["battle_server"];
		const size_t colon = server.rfind(':');
		if (info.sessionId.empty() || info.userId.empty() || colon == std::string::npos)
			return false;
		in_addr addr{};
		if (inet_pton(AF_INET, server.substr(0, colon).c_str(), &addr) != 1)
			return false;
		const int port = std::atoi(server.c_str() + colon + 1);
		if (port <= 0 || port > 0xFFFF)
			return false;
		std::memcpy(&info.serverIP, &addr, 4);
		info.serverPort = static_cast<uint16_t>(port);
		std::istringstream users(kv["users"]);
		std::string id;
		while (std::getline(users, id, ','))
			if (!id.empty())
				info.users.push_back(id);
		if (std::find(info.users.begin(), info.users.end(), info.userId) == info.users.end())
			return false;
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
			AddrString(info.serverIP, info.serverPort));
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

	// ---- the bridge: one battle, one thread ----
	namespace
	{
		using Clock = std::chrono::steady_clock;

		class Bridge
		{
		public:
			Bridge(sock_t listener, BattleInfo info)
				: listener(listener)
				, info(std::move(info))
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
			sock_t listener;
			BattleInfo info;
			std::atomic<bool> stop{false};
			std::thread thread;

			sock_t tcp = INVALID_SOCKET;
			sock_t udp = INVALID_SOCKET;
			sockaddr_in server{};

			// zdxsv/pkg/proto BattleBuffer: unacked messages, resent until acked.
			std::deque<Proto::BattleMessage> pending;
			uint32_t begin = 1; // seq of pending.front()
			uint32_t end = 1; // next seq
			uint32_t recvAck = 0; // last seq received from the server
			// MessageFilter: per-sender next-in-order check.
			std::map<std::string, uint32_t> recvSeq;
			uint32_t msgSeq = 1;
			uint64_t sentMsgs = 0, recvMsgs = 0, sentPkts = 0, recvPkts = 0;

			static bool WaitReadable(sock_t s, int ms)
			{
				fd_set rd;
				FD_ZERO(&rd);
				FD_SET(s, &rd);
				timeval tv{ms / 1000, (ms % 1000) * 1000};
				return select(static_cast<int>(s) + 1, &rd, nullptr, nullptr, &tv) > 0;
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

			// ZDXSV_UDP_TEST_DROP=N: drop every Nth UDP packet, both ways (tests the resend path).
			int dropEvery = 0;
			uint64_t udpCount = 0, dropped = 0;
			bool Drop()
			{
				if (dropEvery <= 0 || ++udpCount % dropEvery != 0)
					return false;
				dropped++;
				return true;
			}

			void SendUDP(const Proto::Packet& pkt)
			{
				if (Drop())
					return;
				const std::vector<uint8_t> data = Proto::Encode(pkt);
				sendto(udp, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
					reinterpret_cast<const sockaddr*>(&server), sizeof(server));
			}

			bool FromServer(const sockaddr_in& from) const
			{
				return from.sin_addr.s_addr == server.sin_addr.s_addr && from.sin_port == server.sin_port;
			}

			int RecvUDP(Proto::Packet& pkt, int ms)
			{
				if (!WaitReadable(udp, ms))
					return 0;
				uint8_t buf[4096];
				sockaddr_in from{};
				socklen_t fromLen = sizeof(from);
				const int n = recvfrom(udp, reinterpret_cast<char*>(buf), sizeof(buf), 0,
					reinterpret_cast<sockaddr*>(&from), &fromLen);
				if (n <= 0 || !FromServer(from) || Drop() || !Proto::Decode(buf, n, pkt))
					return -1;
				return 1;
			}

			bool Greet()
			{
				Proto::Packet hello;
				hello.type = Proto::HelloServer;
				hello.helloSessionId = info.sessionId;
				// zproxy GreetBattleServer: 10 tries, 100 ms apart
				for (int i = 0; i < 10 && !stop; i++)
				{
					SendUDP(hello);
					const auto until = Clock::now() + std::chrono::milliseconds(100);
					while (Clock::now() < until)
					{
						Proto::Packet pkt;
						const int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count());
						if (RecvUDP(pkt, std::max(ms, 1)) == 1 && pkt.type == Proto::HelloServer)
						{
							if (pkt.helloOk)
								return true;
							Log("battle server refused session");
							return false;
						}
					}
				}
				return false;
			}

			void Flush()
			{
				Proto::Packet pkt;
				pkt.type = Proto::Battle;
				const size_t n = std::min<size_t>(pending.size(), 50);
				pkt.battle.assign(pending.begin(), pending.begin() + n);
				pkt.seq = begin + static_cast<uint32_t>(n) - 1;
				pkt.ack = recvAck;
				SendUDP(pkt);
				sentPkts++;
			}

			void OnBattlePacket(const Proto::Packet& pkt)
			{
				recvPkts++;
				recvAck = std::max(recvAck, pkt.seq);
				while (!pending.empty() && begin <= pkt.ack)
				{
					pending.pop_front();
					begin++;
				}
				for (const Proto::BattleMessage& m : pkt.battle)
				{
					auto it = recvSeq.find(m.userId);
					if (it == recvSeq.end() || !(it->second == 0 || m.seq == it->second + 1))
						continue;
					it->second = m.seq;
					recvMsgs++;
					if (!WriteTCP(m.body.data(), m.body.size()))
						stop = true;
				}
			}

			void Run()
			{
				Log("bridge listening");
				if (const char* env = std::getenv("ZDXSV_UDP_TEST_DROP"))
					dropEvery = std::atoi(env);
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

				udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
				sockaddr_in any{};
				any.sin_family = AF_INET;
				bind(udp, reinterpret_cast<const sockaddr*>(&any), sizeof(any));
				server.sin_family = AF_INET;
				server.sin_addr.s_addr = info.serverIP;
				server.sin_port = htons(info.serverPort);
				for (const std::string& id : info.users)
					if (id != info.userId)
						recvSeq[id] = 0;

				if (!Greet())
					Log("bridge: no HelloServer ok from " + AddrString(info.serverIP, info.serverPort));
				else
				{
					Log("bridge: battle server ok");
					Serve();
				}
				Log("bridge end: sent " + std::to_string(sentMsgs) + " msgs / " + std::to_string(sentPkts) +
					" pkts, received " + std::to_string(recvMsgs) + " msgs / " + std::to_string(recvPkts) + " pkts" +
					(dropEvery > 0 ? ", test-dropped " + std::to_string(dropped) + " pkts" : ""));
				closesocket(udp);
				closesocket(tcp);
			}

			void Serve()
			{
				// What the battle server's TCP side sends first (zproxy firstData).
				static const uint8_t firstData[] = {0x28, 0x01, 0x10, 0x31, 0x00, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff, 0xff};
				if (!WriteTCP(firstData, sizeof(firstData)))
					return;
				auto lastSend = Clock::now();
				auto lastGame = Clock::now();
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
						pending.push_back(std::move(m));
						end++;
						sentMsgs++;
						flush = true;
						fin = n == 4 && buf[0] == 0x04 && buf[1] == 0xF0 && buf[2] == 0x00 && buf[3] == 0x00;
					}
					if (FD_ISSET(udp, &rd))
					{
						Proto::Packet pkt;
						if (RecvUDP(pkt, 0) == 1 && pkt.type == Proto::Battle)
							OnBattlePacket(pkt);
					}
					if (flush || Clock::now() - lastSend >= std::chrono::milliseconds(30))
					{
						Flush();
						lastSend = Clock::now();
					}
					if (Clock::now() - lastGame > std::chrono::seconds(10))
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
		g_bridge = std::make_unique<Bridge>(listener, std::move(info));
		return true;
	}

	void Shutdown()
	{
		g_bridge.reset();
		std::lock_guard lock(g_mtx);
		g_armed = false;
	}
} // namespace Zdxsv

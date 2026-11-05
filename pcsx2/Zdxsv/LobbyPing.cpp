// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// GGPO ping test between the battle's peers (Lobby.h StartPingTest) and the cut connection's refusal.

#include "Zdxsv/Lobby.h"
#include "Zdxsv/LobbyNet.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Zdxsv
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// same family, ip and port (IPv6 scope / flow info ignored)
		bool SameAddr(const sockaddr_storage& a, const sockaddr_storage& b)
		{
			if (a.ss_family != b.ss_family)
				return false;
			if (a.ss_family == AF_INET)
				return LobbySameAddr(reinterpret_cast<const sockaddr_in&>(a), reinterpret_cast<const sockaddr_in&>(b));
			const auto& a6 = reinterpret_cast<const sockaddr_in6&>(a);
			const auto& b6 = reinterpret_cast<const sockaddr_in6&>(b);
			return a.ss_family == AF_INET6 && a6.sin6_port == b6.sin6_port &&
				   std::memcmp(&a6.sin6_addr, &b6.sin6_addr, sizeof(a6.sin6_addr)) == 0;
		}

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
	} // namespace

	void MergeRelayRtt(uint8_t dst[4][4], const uint8_t src[4][4], int self, int from)
	{
		for (int p = 0; p < 4; p++)
		{
			if (p == self)
				continue;
			for (int k = 0; k < 4; k++)
				if (p == from || src[p][k] != 0)
					dst[p][k] = src[p][k];
		}
	}

	namespace
	{
		// ping test packet; its layout is a wire format shared with other clients
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
		// sent instead of PingPacket only when the battle has relays
		struct PingPacketRelays : PingPacket
		{
			uint8_t relayRttMatrix[4][4]; // [i][k]: i's rtt to relay k
		};
		// relay server ping, echoed as a pong
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

		// the running test's matrix for PingTestView
		struct PingView
		{
			std::mutex mtx;
			bool active = false;
			Clock::time_point start{};
			int durationMs = 0;
			int n = 0;
			uint8_t rtt[4][4] = {};
			int teams[4] = {};
		};
		PingView g_pingView;

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

		// One ping test on its thread: pings each candidate address of every other position
		// and each relay path, answers the peers' pings, then picks a path per position.
		class PingRun
		{
			// candidate k of position p: byPosition[p][k], then the sources added from its packets; k is the
			// index sent in its pings' candidate byte, and a pong counts for candidate k whatever its source
			struct Cand
			{
				sockaddr_storage sa{};
				socklen_t len = 0;
				sock_t s = INVALID_SOCKET; // socket of its family, INVALID_SOCKET = not pinged
				PeerAddr addr;
				std::vector<int> samples;
				int rtt = -1; // median of the samples, so a few late pongs do not raise the delay
				bool sends = false; // the peer's packets come from sa
				std::vector<sockaddr_storage> answered; // relay path: pong sources other than sa, at most 2
				int Rtt() const { return rtt; }
				void Pong(int64_t ms)
				{
					if (samples.size() >= 256)
						return;
					samples.push_back(static_cast<int>(std::clamp<int64_t>(ms, 1, 60000)));
					std::vector<int> s = samples;
					auto mid = s.begin() + (s.size() - 1) / 2;
					std::nth_element(s.begin(), mid, s.end());
					rtt = *mid;
				}
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
				// older peers read only a PingPacket: the relay rtts go out only in battles with relays
				, m_pkSize(m_nr ? sizeof(PingPacketRelays) : sizeof(PingPacket))
				, m_cand(m_n)
				, m_rpath(m_nr)
				, m_pongsIn(m_n, 0)
			{
				for (int p = 0; p < m_n; p++)
					for (const PeerAddr& a : m_byPosition[p])
					{
						Cand c;
						c.addr = a;
						if (LobbyToSockaddr(a, c.sa, c.len))
							c.s = a.V6() ? m_s6 : m_s4;
						m_cand[p].push_back(c);
					}
				// relay k's IPv4 and IPv6 path, pinged like a candidate
				for (int k = 0; k < m_nr; k++)
					for (const PeerAddr* a : {&m_relays[k].addr, &m_relays[k].addr6})
					{
						Cand& c = m_rpath[k][a->V6() ? 1 : 0];
						if (!a->ip.empty() && LobbyToSockaddr(*a, c.sa, c.len))
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
					std::lock_guard lock(g_pingView.mtx);
					std::memcpy(g_pingView.rtt, m_rtt, sizeof(m_rtt));
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
				LobbyLog(line + MatrixText() + ", " + std::to_string(m_dropped) + " dropped");
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
					// the token proves the relay, not the source: a multi-homed or NATed relay answers from another
					// address, which GGPO then accepts for it too (ggpo_add_relay_server_source)
					const bool other = c && !SameAddr(from, c->sa);
					if (c && c->s != INVALID_SOCKET && rp.type == RELAY_PONG && rp.sessionId == m_session && rp.peer == m_me &&
						rp.token == m_relays[rp.relayIdx].token && (!other || c->answered.size() < 2 ||
							std::any_of(c->answered.begin(), c->answered.end(), [&from](const sockaddr_storage& a) { return SameAddr(from, a); })))
					{
						c->Pong(static_cast<int64_t>(NowMs() - rp.timestamp));
						if (other && std::none_of(c->answered.begin(), c->answered.end(), [&from](const sockaddr_storage& a) { return SameAddr(from, a); }))
							c->answered.push_back(from);
					}
					else
						m_dropped++;
					return;
				}
				if (got < static_cast<int>(sizeof(PingPacket)))
					return;
				const bool head = pk.magic == PING_MAGIC && pk.sessionId == m_session && pk.toPeer == m_me && pk.fromPeer < m_n && pk.fromPeer != m_me;
				std::vector<Cand>* fc = head ? &m_cand[pk.fromPeer] : nullptr;
				// an unlisted source of a peer's ping or pong (its NAT mapping) becomes its candidate, at most 4 per peer
				if (head && (pk.type == PING_TYPE || pk.type == PONG_TYPE))
				{
					auto it = std::find_if(fc->begin(), fc->end(), [&from](const Cand& c) { return SameAddr(from, c.sa); });
					if (it == fc->end() && fc->size() < m_byPosition[pk.fromPeer].size() + 4)
					{
						Cand c;
						c.sa = from;
						c.len = len;
						c.s = s;
						c.addr = LobbyFromSockaddr(from);
						fc->push_back(c);
						it = fc->end() - 1;
					}
					if (it != fc->end())
						it->sends = true;
				}
				bool ok = false;
				if (head && pk.type == PING_TYPE)
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
				else if (head && pk.type == PONG_TYPE && pk.candidate < fc->size())
				{
					Cand& c = (*fc)[pk.candidate];
					c.Pong(static_cast<int64_t>(NowMs() - pk.pingTimestamp));
					ok = true;
				}
				else
					m_dropped++;
				if (ok) // the sender's own row of the rtt matrix, the relay rows it knows
				{
					std::memcpy(m_rtt[pk.fromPeer], pk.rttMatrix[pk.fromPeer], sizeof(m_rtt[0]));
					if (got >= static_cast<int>(sizeof(PingPacketRelays)))
						MergeRelayRtt(m_relayRtt, pk.relayRttMatrix, m_me, pk.fromPeer);
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
					// highest 10000 - rtt, +100 loopback, +50 private, +20 IPv6, +1 the peer sends from it
					float best = 0;
					line += "; peer " + std::to_string(p);
					for (size_t k = 0; k < m_cand[p].size(); k++)
					{
						const Cand& c = m_cand[p][k];
						const int r = c.Rtt();
						line += " " + c.addr.Masked() + " rtt " + std::to_string(r) + " ms (" + std::to_string(c.samples.size()) + " pongs)";
						if (r <= 0)
							continue;
						const float score = 10000.f - r + (IsLoopback(c.sa) ? 100.f : 0.f) + (IsPrivate(c.sa) ? 50.f : 0.f) +
											(c.sa.ss_family == AF_INET6 ? 20.f : 0.f) + (c.sends ? 1.f : 0.f);
						if (score > best)
							best = score, direct[p] = {r, c.addr};
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
									" ms (" + std::to_string(m_rpath[k][i].samples.size()) + " pongs)";
					// GGPO's address of the relay: the faster family that answered, else addr; alt = the other one
					const int f = RelayBest(k);
					const bool useAddr6 = f >= 0 && !rl.addr6.ip.empty() && (f == 1) != rl.addr.V6();
					servers.push_back(useAddr6 ? RelayServerAddr{rl.addr6, rl.addr} : RelayServerAddr{rl.addr, rl.addr6});
					for (int i = 0; i < 2; i++)
						for (const sockaddr_storage& a : m_rpath[k][i].answered)
						{
							servers.back().sources.push_back(LobbyFromSockaddr(a));
							line += " answers from " + servers.back().sources.back().String();
						}
				}
			}

			// a relaying peer must beat direct by 32 ms (it spends its
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

	void StartPingTest(uint32_t session, int me, const std::vector<std::vector<PeerAddr>>& byPosition, uint16_t port, int durationMs,
		const std::vector<BattleInfo::Relay>& relays, const std::vector<int>& teams)
	{
		if (g_ping)
			g_ping->stop = true;
		FinishPingTest();
		StopUdpTest();
		StopCutRefusal();
		g_pingError.clear();
		const int n = static_cast<int>(byPosition.size());
		if (me < 0 || me >= n || n > 4)
			return;
		const sock_t s4 = LobbyBindUdp(AF_INET, port), s6 = LobbyBindUdp(AF_INET6, port);
		if (s4 == INVALID_SOCKET && s6 == INVALID_SOCKET)
		{
			g_pingError = "bind :" + std::to_string(port) + " failed (IPv4 and IPv6)";
			LobbyLog("ping test: " + g_pingError);
			return;
		}
		if (s4 == INVALID_SOCKET || s6 == INVALID_SOCKET)
			LobbyLog(std::string("ping test: no IPv") + (s4 == INVALID_SOCKET ? "4" : "6") + " socket on :" + std::to_string(port) + ", its candidates skipped");
		g_ping = std::make_unique<PingTest>();
		g_ping->result.assign(n, {});
		{
			std::lock_guard lock(g_pingView.mtx);
			g_pingView.active = true;
			g_pingView.start = Clock::now();
			g_pingView.durationMs = durationMs;
			g_pingView.n = n;
			std::memset(g_pingView.rtt, 0, sizeof(g_pingView.rtt));
			for (int p = 0; p < 4; p++)
				g_pingView.teams[p] = p < static_cast<int>(teams.size()) ? teams[p] : 0;
		}
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
		{
			std::lock_guard lock(g_pingView.mtx);
			g_pingView.active = false;
		}
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

	namespace
	{
		struct CutRefusal
		{
			std::thread thread;
			std::atomic<bool> stop{false};
			~CutRefusal()
			{
				stop = true;
				if (thread.joinable())
					thread.join();
			}
		};
		std::unique_ptr<CutRefusal> g_refusal;

		// GGPO's packed UdpMsg: a 12-byte header (const_magic, magic, sequence_number, remote_endpoint,
		// type, relay_magic, relay_to_endpoint, org_type), then sync_request {random_request, ...} or
		// sync_reply {random_reply, verification_failure}
		constexpr uint16_t GGPO_CONST_MAGIC = 34046;
		constexpr uint8_t GGPO_SYNC_REQUEST = 1, GGPO_SYNC_REPLY = 2;
		constexpr int GGPO_HDR = 12;
	} // namespace

	void StopCutRefusal()
	{
		g_refusal.reset();
	}

	void StartCutRefusal(int me, uint16_t port, int durationMs)
	{
		g_refusal.reset();
		if (g_ping)
			g_ping->stop = true;
		FinishPingTest();
		if (me < 0 || me >= 4)
			return;
		const sock_t s4 = LobbyBindUdp(AF_INET, port), s6 = LobbyBindUdp(AF_INET6, port);
		if (s4 == INVALID_SOCKET && s6 == INVALID_SOCKET)
		{
			LobbyLog("cut refusal: bind :" + std::to_string(port) + " failed");
			return;
		}
		g_refusal = std::make_unique<CutRefusal>();
		CutRefusal* r = g_refusal.get();
		r->thread = std::thread([r, s4, s6, me, durationMs] {
			const auto end = Clock::now() + std::chrono::milliseconds(durationMs);
			const uint16_t magic = static_cast<uint16_t>(1 + std::rand() % 0xfffe);
			uint16_t seq = 0;
			int replies = 0;
			std::vector<PeerAddr> refused;
			while (!r->stop && Clock::now() < end)
			{
				fd_set rd;
				FD_ZERO(&rd);
				sock_t top = 0;
				for (sock_t s : {s4, s6})
					if (s != INVALID_SOCKET)
					{
						FD_SET(s, &rd);
						top = std::max(top, s);
					}
				timeval tv{0, 50000};
				if (select(static_cast<int>(top) + 1, &rd, nullptr, nullptr, &tv) <= 0)
					continue;
				for (sock_t s : {s4, s6})
				{
					if (s == INVALID_SOCKET || !FD_ISSET(s, &rd))
						continue;
					uint8_t buf[512];
					sockaddr_storage from{};
					socklen_t fromLen = sizeof(from);
					const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
					uint16_t cm;
					if (n < GGPO_HDR + 6 || (std::memcpy(&cm, buf, 2), cm) != GGPO_CONST_MAGIC || buf[7] != GGPO_SYNC_REQUEST)
						continue;
					uint8_t reply[GGPO_HDR + 5] = {};
					std::memcpy(reply, &GGPO_CONST_MAGIC, 2);
					std::memcpy(reply + 2, &magic, 2);
					std::memcpy(reply + 4, &seq, 2);
					seq++;
					reply[6] = static_cast<uint8_t>(me);
					reply[7] = GGPO_SYNC_REPLY;
					std::memcpy(reply + GGPO_HDR, buf + GGPO_HDR, 4);
					reply[GGPO_HDR + 4] = 1;
					sendto(s, reinterpret_cast<const char*>(reply), sizeof(reply), 0, reinterpret_cast<const sockaddr*>(&from), fromLen);
					replies++;
					const PeerAddr peer = LobbyFromSockaddr(from);
					if (std::find_if(refused.begin(), refused.end(), [&](const PeerAddr& a) { return a.String() == peer.String(); }) == refused.end())
					{
						refused.push_back(peer);
						LobbyLog("cut refusal: refused the GGPO session of " + peer.String());
					}
				}
			}
			LobbyLog("cut refusal: ended, " + std::to_string(replies) + " replies to " + std::to_string(refused.size()) + " peers");
			for (sock_t s : {s4, s6})
				if (s != INVALID_SOCKET)
					closesocket(s);
		});
	}

	bool PingTestView(int& elapsedMs, int& durationMs, int& players, uint8_t rtt[4][4], int teams[4])
	{
		std::lock_guard lock(g_pingView.mtx);
		if (!g_pingView.active)
			return false;
		elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_pingView.start).count());
		durationMs = g_pingView.durationMs;
		players = g_pingView.n;
		std::memcpy(rtt, g_pingView.rtt, sizeof(g_pingView.rtt));
		std::memcpy(teams, g_pingView.teams, sizeof(g_pingView.teams));
		return true;
	}
} // namespace Zdxsv

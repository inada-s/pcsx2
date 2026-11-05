// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/Lobby.h"
#include "Zdxsv/Proto.h"
#include "Zdxsv/LobbyNet.h"
#include "Zdxsv/TestOptions.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace Zdxsv
{
	namespace
	{
		std::mutex g_mtx;
		std::function<void(const std::string&)> g_log;
		BattleInfo g_info;
		std::function<void(const BattleInfo&)> g_listener;
		std::vector<std::vector<uint8_t>> g_startAnswers; // LobbyNoteFrame, under g_mtx

		std::string AddrString(uint32_t ip, uint16_t port)
		{
			const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
			return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
				   std::to_string(b[3]) + ":" + std::to_string(port);
		}
	} // namespace

	void LobbyLog(const std::string& s)
	{
		std::function<void(const std::string&)> log;
		{
			std::lock_guard lock(g_mtx);
			log = g_log;
		}
		if (log)
			log("zdxsv: " + s);
	}

	void LobbySetLogger(std::function<void(const std::string&)> log)
	{
		std::lock_guard lock(g_mtx);
		g_log = std::move(log);
	}

	std::string MaskIpAddress(const std::string& addr)
	{
		const bool v6 = std::count(addr.begin(), addr.end(), ':') >= 2;
		// masked part: IPv4 from the first '.', IPv6 from the third ':', up to the port's ':'
		size_t from = std::string::npos;
		if (v6)
		{
			int colons = 0;
			for (size_t i = 0; i < addr.size() && from == std::string::npos; i++)
				if (addr[i] == ':' && ++colons == 3)
					from = i;
		}
		else
			from = addr.find('.');
		const size_t to = v6 ? addr.rfind(':') : addr.find(':');
		std::string out;
		for (size_t i = 0; i < addr.size(); i++)
		{
			const unsigned char c = static_cast<unsigned char>(addr[i]);
			const bool hide = from != std::string::npos && i > from && (to == std::string::npos || i < to) &&
							  (v6 ? std::isxdigit(c) : std::isdigit(c));
			// a run of hidden digits becomes one 'x': the digit count is not shown either
			if (!hide)
				out += addr[i];
			else if (out.empty() || out.back() != 'x')
				out += 'x';
		}
		return out;
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

	static bool PbString(uint32_t wt, const uint8_t* b, size_t n, std::string& s)
	{
		if (wt != 2 || n > 256)
			return false;
		s.assign(reinterpret_cast<const char*>(b), n);
		return true;
	}

	static bool PbInt(uint32_t wt, uint64_t v, int64_t& out)
	{
		out = static_cast<int64_t>(v);
		return wt == 0;
	}

	// "ip" (IPv6 without brackets) and a port -> PeerAddr
	static bool PbPeerAddr(const std::string& ip, int64_t port, PeerAddr& out)
	{
		if (port <= 0 || port > 0xFFFF)
			return false;
		const std::string host = ip.find(':') == std::string::npos ? ip : "[" + ip + "]";
		return ParsePeerAddr(host + ":" + std::to_string(port), out);
	}

	bool ParseBattleInfo(const std::string& body, BattleInfo& out)
	{
		constexpr size_t MAX_USERS = 8, MAX_CANDIDATES = 64, MAX_RELAYS = 4;
		struct User
		{
			std::string id, name, pilot, sync;
			int64_t team = 0;
			int64_t ggpo = 0;
		};
		struct Candidate
		{
			std::string userId, ip;
			int64_t port = 0;
		};
		struct Relay
		{
			std::string ip, ip6;
			int64_t port = 0, port6 = 0;
			uint64_t token = 0;
		};
		std::vector<User> users;
		std::vector<Candidate> candidates;
		std::vector<Relay> relays;
		std::string battleServer;
		int64_t peerId = -1, session = 0, pingMs = 0, uplink = 0;
		BattleInfo info;
		const uint8_t* p = reinterpret_cast<const uint8_t*>(body.data());
		Pb::Reader r{p, p + body.size()};
		const bool ok = r.Fields([&](uint32_t field, uint32_t wt, uint64_t v, const uint8_t* b, size_t n) {
			Pb::Reader sub{b, b + n};
			switch (field)
			{
				case 1: return PbString(wt, b, n, info.battleCode);
				case 2: return PbInt(wt, v, session) && v <= 0xFFFFFFFFu;
				case 4: return PbInt(wt, v, peerId);
				case 6: return PbInt(wt, v, pingMs);
				case 9:
					if (wt != 2 || users.size() >= MAX_USERS)
						return false;
					users.emplace_back();
					return sub.Fields([&u = users.back()](uint32_t f, uint32_t w, uint64_t x, const uint8_t* s, size_t len) {
						switch (f)
						{
							case 1: return PbString(w, s, len, u.id);
							case 2: return PbString(w, s, len, u.name);
							case 3: return PbString(w, s, len, u.pilot);
							case 9: return PbInt(w, x, u.team);
							case 40: return PbInt(w, x, u.ggpo);
							case 41: return PbString(w, s, len, u.sync);
							default: return true;
						}
					});
				case 10:
					if (wt != 2 || candidates.size() >= MAX_CANDIDATES)
						return false;
					candidates.emplace_back();
					return sub.Fields([&c = candidates.back()](uint32_t f, uint32_t w, uint64_t x, const uint8_t* s, size_t len) {
						switch (f)
						{
							case 1: return PbString(w, s, len, c.userId);
							case 3: return PbString(w, s, len, c.ip);
							case 4: return PbInt(w, x, c.port);
							default: return true;
						}
					});
				case 11:
					if (wt != 2 || relays.size() >= MAX_RELAYS)
						return false;
					relays.emplace_back();
					return sub.Fields([&rl = relays.back()](uint32_t f, uint32_t w, uint64_t x, const uint8_t* s, size_t len) {
						switch (f)
						{
							case 2: return PbString(w, s, len, rl.ip);
							case 3: return PbInt(w, x, rl.port);
							case 4: rl.token = x; return w == 0;
							case 5: return PbString(w, s, len, rl.ip6);
							case 40: return PbInt(w, x, rl.port6);
							default: return true;
						}
					});
				case 12: return PbInt(wt, v, uplink);
				case 40: return PbString(wt, b, n, info.sessionId);
				case 41: return PbString(wt, b, n, battleServer);
				default: return true;
			}
		});
		if (!ok || peerId < 0 || peerId >= static_cast<int64_t>(users.size()) || info.sessionId.empty() ||
			!ParseAddr(battleServer, info.serverIP, info.serverPort))
			return false;
		for (const User& u : users)
		{
			if (u.id.empty() || std::find(info.users.begin(), info.users.end(), u.id) != info.users.end())
				return false;
			info.users.push_back(u.id);
		}
		info.userId = users[peerId].id;
		static const char* drop_sync = TestEnv("ZDXSV_DROP_SYNC");
		for (const User& u : users)
		{
			if (!u.name.empty())
				info.names[u.id] = u.name;
			if (!u.pilot.empty())
				info.pilots[u.id] = u.pilot;
			if (u.sync.size() == 16 && u.sync.find_first_not_of("0123456789abcdef") == std::string::npos &&
				!(drop_sync && std::atoi(drop_sync) == &u - users.data()))
				info.syncs[u.id] = u.sync;
			if (u.team == 1 || u.team == 2)
				info.teams[u.id] = static_cast<int>(u.team);
			if (u.id != info.userId && u.ggpo > 0 && u.ggpo <= 0xFFFF)
				info.ggpo[u.id] = static_cast<uint16_t>(u.ggpo);
			BattleInfo::Peer peer{u.id, {}};
			for (const Candidate& c : candidates)
			{
				PeerAddr addr;
				if (c.userId == u.id && u.id != info.userId && PbPeerAddr(c.ip, c.port, addr))
					peer.addrs.push_back(addr);
			}
			if (!peer.addrs.empty())
				info.p2p.push_back(std::move(peer));
		}
		info.ggpoSession = static_cast<uint32_t>(session);
		info.ggpoPingMs = static_cast<int>(std::clamp<int64_t>(pingMs, 0, MAX_PING_MS));
		info.liveUplink = uplink != 0;
		// a malformed relay ends the list, so the indexes stay the lobby's
		for (const Relay& rl : relays)
		{
			BattleInfo::Relay br;
			br.token = rl.token;
			if (!rl.token || !PbPeerAddr(rl.ip, rl.port, br.addr) ||
				(!rl.ip6.empty() && !PbPeerAddr(rl.ip6, rl.port6 ? rl.port6 : rl.port, br.addr6)))
				break;
			info.relays.push_back(br);
		}
		out = std::move(info);
		return true;
	}

	std::vector<uint8_t> EncodeMatchingReport(const std::string& lines)
	{
		std::map<std::string, std::string> kv;
		std::map<int, std::map<std::string, std::string>> peers; // rtt_ / addr_ / path_ / ping_ / loss_<position>
		std::istringstream in(lines);
		std::string line;
		while (std::getline(in, line))
		{
			const size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
			const size_t us = k.find('_');
			const std::string prefix = us == std::string::npos ? "" : k.substr(0, us);
			if ((prefix == "rtt" || prefix == "addr" || prefix == "path" || prefix == "ping" || prefix == "loss") && k.find_first_not_of("0123456789", us + 1) == std::string::npos &&
				us + 1 < k.size())
				peers[std::atoi(k.c_str() + us + 1)][prefix] = v;
			else
				kv[k] = v;
		}
		static constexpr struct
		{
			const char* key;
			uint32_t field;
			bool string;
		} FIELDS[] = {{"battle_code", 1, true}, {"players", 3, false}, {"position", 4, false}, {"close", 5, true},
			{"frames", 6, false}, {"delay", 21, false}, {"user_id", 40, true}, {"result", 41, true}, {"reason", 42, true},
			{"sync", 43, true}, {"sync_differ", 44, true}, {"answered", 45, false}, {"relays", 46, false},
			{"ping_error", 47, true}, {"ping_wait_ms", 48, false}, {"min_delay", 49, false}, {"fixed_delay", 50, false},
			{"cut_sends", 51, false}, {"rollback_frames", 52, false}, {"mismatches", 53, false}, {"disconnected", 54, false},
			{"timesync", 58, false}, {"input_blocks", 59, false}};
		std::vector<uint8_t> o;
		for (const auto& f : FIELDS)
		{
			if (f.field == 40) // remote_peers (31) in field order
				for (const auto& [pos, m] : peers)
				{
					std::vector<uint8_t> peer;
					Pb::PutInt(peer, 1, pos);
					int ping[3];
					if (auto it = m.find("ping"); it != m.end() &&
						std::sscanf(it->second.c_str(), "%d/%d/%d", &ping[0], &ping[1], &ping[2]) == 3 && ping[0] >= 0)
						for (int i = 0; i < 3; i++)
							Pb::PutInt(peer, 4 + i, ping[i]); // ping_min_ms, ping_median_ms, ping_max_ms
					if (auto it = m.find("path"); it != m.end())
						Pb::PutString(peer, 7, it->second);
					if (auto it = m.find("rtt"); it != m.end())
						Pb::PutInt(peer, 40, std::atoll(it->second.c_str()));
					if (auto it = m.find("addr"); it != m.end())
						Pb::PutString(peer, 41, it->second);
					int loss[2];
					if (auto it = m.find("loss"); it != m.end() && std::sscanf(it->second.c_str(), "%d/%d", &loss[0], &loss[1]) == 2)
						for (int i = 0; i < 2; i++)
							Pb::PutInt(peer, 42 + i, loss[i]); // recv_loss, send_loss
					Pb::PutBytes(o, 31, peer.data(), peer.size());
				}
			const auto it = kv.find(f.key);
			if (it == kv.end())
				continue;
			if (f.string)
				Pb::PutString(o, f.field, it->second);
			else
				Pb::PutInt(o, f.field, std::atoll(it->second.c_str()));
		}
		if (auto it = kv.find("rounds"); it != kv.end())
		{
			std::istringstream rs(it->second);
			for (std::string r; std::getline(rs, r, ';');)
			{
				const size_t colon = r.find(':');
				if (colon == std::string::npos)
					continue;
				std::vector<int64_t> ms;
				std::istringstream ss(r.substr(colon + 1));
				for (std::string m; std::getline(ss, m, ',');)
					ms.push_back(std::atoll(m.c_str()));
				std::vector<uint8_t> round;
				Pb::PutInt(round, 1, std::atoll(r.c_str()));
				Pb::PutPackedInts(round, 2, ms);
				Pb::PutBytes(o, 20, round.data(), round.size()); // round_data
			}
		}
		if (auto it = kv.find("disconnected_positions"); it != kv.end() && !it->second.empty())
		{
			std::vector<int64_t> left;
			std::istringstream ps(it->second);
			for (std::string p; std::getline(ps, p, ',');)
				left.push_back(std::atoll(p.c_str()));
			Pb::PutPackedInts(o, 55, left); // disconnected_peer_ids
		}
		if (auto it = kv.find("rollback_budget"); it != kv.end())
			Pb::PutFloat(o, 56, std::strtof(it->second.c_str(), nullptr));
		if (auto it = kv.find("rollback_budget_used"); it != kv.end())
			Pb::PutFloat(o, 57, std::strtof(it->second.c_str(), nullptr));
		if (auto it = kv.find("fps_history"); it != kv.end() && !it->second.empty())
		{
			std::vector<int64_t> fps;
			std::istringstream fs(it->second);
			for (std::string f; std::getline(fs, f, ',');)
				fps.push_back(std::atoll(f.c_str()));
			Pb::PutPackedInts(o, 60, fps);
		}
		return o;
	}

	void SetBattleInfo(const BattleInfo& info)
	{
		std::function<void(const BattleInfo&)> listener;
		{
			std::lock_guard lock(g_mtx);
			g_info = info;
			listener = g_listener;
		}
		LobbyLog("battle info: user " + info.userId + ", " + std::to_string(info.users.size()) + " players, server " +
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

	int GgpoPeers(const BattleInfo& info, const std::string& ownPublicIP, std::vector<std::vector<PeerAddr>>& byPosition)
	{
		int me = -1;
		byPosition.assign(info.users.size(), {});
		for (size_t i = 0; i < info.users.size(); i++)
		{
			const std::string& u = info.users[i];
			if (u == info.userId)
			{
				me = static_cast<int>(i);
				continue;
			}
			const auto port = info.ggpo.find(u);
			const auto peer = std::find_if(info.p2p.begin(), info.p2p.end(), [&u](const BattleInfo::Peer& p) { return p.userId == u; });
			if (port == info.ggpo.end() || peer == info.p2p.end())
				continue;
			const bool announced = std::any_of(peer->addrs.begin(), peer->addrs.end(), [&port](const PeerAddr& a) { return a.port == port->second; });
			std::vector<PeerAddr> v4, v6;
			for (const PeerAddr& a : peer->addrs)
				(a.V6() ? v6 : v4).push_back({a.ip, announced ? a.port : port->second});
			// same NAT (its public IPv4 is ours): its local IPv4 (the last one) first
			if (v4.size() > 1 && !ownPublicIP.empty() && v4.front().ip == ownPublicIP)
				std::rotate(v4.begin(), v4.end() - 1, v4.end());
			std::vector<PeerAddr>& c = byPosition[i];
			for (const std::vector<PeerAddr>* list : {&v4, &v6})
				for (const PeerAddr& a : *list)
					if (std::find(c.begin(), c.end(), a) == c.end())
						c.push_back(a);
		}
		return me;
	}

	namespace
	{
		std::atomic<int> s_lobby_filters{0};
	} // namespace

	LobbyFilter::LobbyFilter()
	{
		s_lobby_filters++;
	}

	LobbyFilter::~LobbyFilter()
	{
		s_lobby_filters--;
	}

	bool LobbyConnectionOpen()
	{
		return s_lobby_filters.load() > 0;
	}

	void LobbyFilterDeleter::operator()(LobbyFilter* filter) const
	{
		delete filter;
	}

	void LobbyNoteFrame(const uint8_t* frame, size_t size)
	{
		if (size < 12 || frame[0] != 0x18 || frame[2] != 0x69 || frame[3] < 0x10 || frame[3] > 0x17)
			return;
		std::lock_guard lock(g_mtx);
		if (frame[3] == 0x10)
			g_startAnswers.clear();
		else
			g_startAnswers.emplace_back(frame, frame + size);
	}

	std::vector<std::vector<uint8_t>> LobbyStartAnswers()
	{
		std::lock_guard lock(g_mtx);
		return g_startAnswers;
	}

	size_t LobbyFilter::Take(uint8_t* dst, size_t max)
	{
		const size_t n = std::min(max, ready.size());
		std::memcpy(dst, ready.data(), n);
		ready.erase(ready.begin(), ready.begin() + n);
		return n;
	}

	void LobbyFilter::Feed(const uint8_t* data, size_t len, bool answerLineChecks)
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
					LobbyLog("bad battle info notice");
			}
			else if (answerLineChecks && f[0] == 0x18 && f[1] == 0x01 && f[2] == 0x60 && f[3] == 0x01)
			{
				const uint8_t answer[header] = {0x81, 0x02, 0x60, 0x01, 0x00, 0x00, f[6], f[7], 0x00, 0xFF, 0xFF, 0xFF};
				answers.insert(answers.end(), answer, answer + header);
			}
			else
			{
				constexpr std::string_view update = "PLEASE UPDATE PCSX2";
				if (f[0] == 0x18 && f[1] == 0x10 && f[2] == 0x60 && f[3] == 0x03 &&
					std::string_view(reinterpret_cast<const char*>(f + header), total - header).find(update) != std::string_view::npos)
					pleaseUpdate = true;
				LobbyNoteFrame(f, total);
				out.insert(out.end(), f, f + total);
			}
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

		sockaddr_in MakeAddr(uint32_t ip, uint16_t port)
		{
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_addr.s_addr = ip;
			a.sin_port = htons(port);
			return a;
		}

		std::string AddrString(const sockaddr_in& a)
		{
			return AddrString(a.sin_addr.s_addr, ntohs(a.sin_port));
		}

		// RFC 5389 STUN servers on other networks for the connectivity test; ZDXSV_NAT_STUN overrides
		// ("host:port,host:port"; none = no item with a port)
		std::vector<std::string> NatStunServers()
		{
			const char* env = TestEnv("ZDXSV_NAT_STUN");
			std::stringstream list(env ? env : "stun.l.google.com:19302,stun.cloudflare.com:3478");
			std::vector<std::string> out;
			for (std::string item; std::getline(list, item, ',');)
				if (!item.empty())
					out.push_back(item);
			return out;
		}

		// IPv4 of each "host:port" that resolves within 2 s. getaddrinfo cannot be cancelled, so each runs
		// on a detached thread that stop or the timeout leaves behind.
		std::vector<sockaddr_in> ResolveStunServers(const std::vector<std::string>& hostPorts, const std::function<bool()>& stopped)
		{
			struct Job
			{
				std::mutex mtx;
				bool done = false;
				bool ok = false;
				sockaddr_in addr{};
			};
			std::vector<std::shared_ptr<Job>> jobs;
			for (const std::string& hp : hostPorts)
			{
				const size_t colon = hp.rfind(':');
				if (colon == std::string::npos)
					continue;
				auto job = std::make_shared<Job>();
				jobs.push_back(job);
				std::thread([job, host = hp.substr(0, colon), port = hp.substr(colon + 1)] {
					addrinfo hints{};
					hints.ai_family = AF_INET;
					hints.ai_socktype = SOCK_DGRAM;
					addrinfo* res = nullptr;
					const bool ok = getaddrinfo(host.c_str(), port.c_str(), &hints, &res) == 0 && res;
					std::lock_guard lock(job->mtx);
					if (ok)
						job->addr = *reinterpret_cast<const sockaddr_in*>(res->ai_addr);
					job->ok = ok;
					job->done = true;
					if (res)
						freeaddrinfo(res);
				}).detach();
			}
			const auto allDone = [&] {
				return std::all_of(jobs.begin(), jobs.end(), [](const std::shared_ptr<Job>& j) {
					std::lock_guard lock(j->mtx);
					return j->done;
				});
			};
			const auto until = Clock::now() + std::chrono::seconds(2);
			while (!allDone() && !stopped() && Clock::now() < until)
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			std::vector<sockaddr_in> out;
			for (const auto& j : jobs)
			{
				std::lock_guard lock(j->mtx);
				if (j->done && j->ok)
					out.push_back(j->addr);
			}
			return out;
		}

		// RFC 5389 magic cookie: bytes 4-7 of every STUN message, the XOR key of XOR-MAPPED-ADDRESS
		constexpr uint8_t StunMagic[4] = {0x21, 0x12, 0xA4, 0x42};

		// "ip:port" of a Binding success response to tid (XOR-MAPPED-ADDRESS, else MAPPED-ADDRESS); "" if it is not one
		std::string StunMappedAddr(const uint8_t* p, int n, const uint8_t* tid)
		{
			if (n < 20 || p[0] != 0x01 || p[1] != 0x01 || std::memcmp(p + 4, StunMagic, 4) != 0 || std::memcmp(p + 8, tid, 12) != 0)
				return "";
			const int end = 20 + ((p[2] << 8) | p[3]);
			if (end > n)
				return "";
			std::string plain;
			for (int i = 20; i + 4 <= end;)
			{
				const int type = (p[i] << 8) | p[i + 1];
				const int len = (p[i + 2] << 8) | p[i + 3];
				const int v = i + 4;
				if (v + len > end)
					break;
				if ((type == 0x0020 || type == 0x0001) && len >= 8 && p[v + 1] == 0x01)
				{
					const bool x = type == 0x0020;
					const uint16_t port = static_cast<uint16_t>(((p[v + 2] ^ (x ? StunMagic[0] : 0)) << 8) | (p[v + 3] ^ (x ? StunMagic[1] : 0)));
					uint8_t ip[4];
					for (int k = 0; k < 4; k++)
						ip[k] = p[v + 4 + k] ^ (x ? StunMagic[k] : 0);
					uint32_t ipn;
					std::memcpy(&ipn, ip, 4);
					if (x)
						return AddrString(ipn, port);
					plain = AddrString(ipn, port);
				}
				i = v + ((len + 3) & ~3);
			}
			return plain;
		}

		// Binding requests from s to each server (3 tries, 300 ms each): the "ip:port" each one saw, "" for no answer
		std::vector<std::string> StunMapped(sock_t s, const std::vector<sockaddr_in>& servers, const std::function<bool()>& stopped)
		{
			std::vector<std::string> mapped(servers.size());
			std::vector<std::array<uint8_t, 12>> tids(servers.size());
			std::random_device rd;
			for (auto& tid : tids)
				for (uint8_t& b : tid)
					b = static_cast<uint8_t>(rd());
			const auto all = [&] { return std::none_of(mapped.begin(), mapped.end(), [](const std::string& m) { return m.empty(); }); };
			for (int i = 0; i < 3 && !all() && !stopped(); i++)
			{
				for (size_t k = 0; k < servers.size(); k++)
				{
					if (!mapped[k].empty())
						continue;
					uint8_t req[20] = {0x00, 0x01, 0x00, 0x00};
					std::memcpy(req + 4, StunMagic, 4);
					std::memcpy(req + 8, tids[k].data(), 12);
					sendto(s, reinterpret_cast<const char*>(req), sizeof(req), 0, reinterpret_cast<const sockaddr*>(&servers[k]), sizeof(servers[k]));
				}
				const auto until = Clock::now() + std::chrono::milliseconds(300);
				while (!all() && !stopped() && Clock::now() < until)
				{
					if (!LobbyWaitReadable(s, 50))
						continue;
					uint8_t buf[512];
					sockaddr_in from{};
					socklen_t fromLen = sizeof(from);
					const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
					for (size_t k = 0; k < servers.size(); k++)
						if (mapped[k].empty() && LobbySameAddr(from, servers[k]))
							mapped[k] = StunMappedAddr(buf, n, tids[k].data());
				}
			}
			return mapped;
		}

		int64_t NowNanos()
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
		}

		// The platform info lines of the last OpenUdp. DEV9 thread only.
		std::string g_udpLines;
		std::string g_udpAddr; // the STUN address OpenUdp asked (LobbyUdpAddr)

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
	} // namespace

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
		g_udpLines.clear();
		g_udpAddr = AddrString(stunIP, stunPort);
		sockaddr_in addr{};
		socklen_t len = sizeof(addr);
		const auto open = [&addr, &len](uint16_t p) {
			sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			addr = MakeAddr(0, p);
			len = sizeof(addr);
			if (s != INVALID_SOCKET && (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
										   getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0))
			{
				closesocket(s);
				s = INVALID_SOCKET;
			}
			return s;
		};
		sock_t s = open(bindPort);
		if (s == INVALID_SOCKET && bindPort != 0)
		{
			LobbyLog("udp: port " + std::to_string(bindPort) + " taken, STUN from another port");
			s = open(0);
		}
		if (s == INVALID_SOCKET)
		{
			LobbyLog("udp: socket failed, no STUN");
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
		if (probe6 != INVALID_SOCKET && LobbyToSockaddr({"2001:db8::1", 53}, dst6, len6) &&
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
				if (!LobbyWaitReadable(s, 50))
					continue;
				uint8_t buf[512];
				sockaddr_in from{};
				socklen_t fromLen = sizeof(from);
				const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
				ProtoPacket pong;
				if (n > 0 && LobbySameAddr(from, stun) && ProtoDecode(buf, n, pong) && pong.type == ProtoPong)
				{
					pub = pong.publicAddr;
					rtt = static_cast<int>((NowNanos() - ping.timestamp) / 1000000);
				}
			}
		}
		closesocket(s);
		if (!pub.empty())
			g_udpLines += "udp_addr=" + pub + "\nudp_rtt=" + std::to_string(rtt) + "\n"; // the lobby picks the live uplink by it
		if (!local.empty())
			g_udpLines += "udp_local=" + local + "\n";
		if (!pub6.empty())
			g_udpLines += "udp_addr6=" + pub6 + "\n";
		LobbyLog("udp: port " + std::to_string(port) + ", public " + (pub.empty() ? "unknown (no STUN answer from " + AddrString(stun) + ")" : MaskIpAddress(pub)) +
			", local " + MaskIpAddress(local) + ", IPv6 " + (pub6.empty() ? "none" : MaskIpAddress(pub6)));
		return g_udpLines;
	}

	std::string UdpTest(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort, std::string& summary, const std::atomic<bool>* stop,
		const UdpReachAsk& reachAsk)
	{
		const auto stopped = [stop] { return stop && stop->load(); };
		// A "udptest" Ping makes the server answer from both sockets: the test socket's Pong arrives only
		// if the port is open. Then a Ping to the test socket: the same mapped port = cone NAT, else symmetric.
		summary = "unknown";
		sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in addr = MakeAddr(0, bindPort);
		if (s == INVALID_SOCKET || bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
		{
			if (s != INVALID_SOCKET)
				closesocket(s);
			summary = "unknown (port busy)";
			LobbyLog("udp test: " + summary + ": port " + std::to_string(bindPort));
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
					if (!LobbyWaitReadable(s, 50))
						continue;
					uint8_t buf[512];
					sockaddr_in from{};
					socklen_t fromLen = sizeof(from);
					const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
					ProtoPacket pong;
					if (n <= 0 || !ProtoDecode(buf, n, pong) || pong.type != ProtoPong)
						continue;
					if (LobbySameAddr(from, stun))
						mapped = pong.publicAddr;
					else if (LobbySameAddr(from, test))
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
		// "" = not asked; the server's test socket shares its IP, so an address-restricted NAT passes it
		std::string reach;
		if (open && reachAsk && !stopped())
		{
			reach = "no answer";
			if (!reachAsk(mapped, stop))
				reach = "not asked (request failed)";
			const auto until = Clock::now() + std::chrono::seconds(3);
			while (reach == "no answer" && !stopped() && Clock::now() < until)
			{
				if (!LobbyWaitReadable(s, 50))
					continue;
				char buf[64];
				sockaddr_in from{};
				socklen_t fromLen = sizeof(from);
				const int n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
				if (n == 5 && std::memcmp(buf, "Hello", 5) == 0)
					reach = "Hello from " + MaskIpAddress(AddrString(from));
			}
		}
		// the server's two sockets share one IP: a NAT that maps per destination IP looks like a cone to them
		std::vector<std::string> outside;
		if (!mapped.empty() && !open && !stopped())
		{
			outside = StunMapped(s, ResolveStunServers(NatStunServers(), stopped), stopped);
			outside.erase(std::remove(outside.begin(), outside.end(), std::string()), outside.end());
		}
		closesocket(s);

		uint32_t ip1 = 0, ip2 = 0;
		uint16_t port1 = 0, port2 = 0;
		// summary is shown to the player (streams): words only; addresses go to the log line only
		std::string nat, detail;
		if (stopped())
			nat = "unknown", summary = "unknown (test cancelled)", detail = "port " + std::to_string(bindPort) + " taken by the ping test";
		else if (mapped.empty())
			nat = "unknown", summary = "unknown (no answer from the server)", detail = "no STUN answer from " + AddrString(stun);
		else if (open && reach == "no answer")
			nat = "cone", summary = "cone NAT (open to the server only)", detail = "port " + std::to_string(bindPort) + " open to the server's IP, no Hello from the udptest function, public " + MaskIpAddress(mapped);
		else if (open)
			nat = "open", summary = "open", detail = "port " + std::to_string(bindPort) + " reachable from any source, public " + MaskIpAddress(mapped) + ", udptest " + (reach.empty() ? "off" : reach);
		else
		{
			const bool serverPair = !mappedTest.empty() && ParseAddr(mapped, ip1, port1) && ParseAddr(mappedTest, ip2, port2);
			const bool outsideSplit = std::any_of(outside.begin(), outside.end(), [&](const std::string& a) { return a != outside.front(); });
			std::string seen = "port " + std::to_string(bindPort) + " closed, public " + MaskIpAddress(mapped) + " / " +
							   (mappedTest.empty() ? "none" : MaskIpAddress(mappedTest)) + " (server), ";
			for (size_t i = 0; i < outside.size(); i++)
				seen += (i ? " / " : "") + MaskIpAddress(outside[i]);
			seen += outside.empty() ? "none (outside STUN)" : " (outside STUN)";
			if (outsideSplit || (serverPair && (port1 != port2 || ip1 != ip2)))
				nat = "symmetric", summary = "symmetric NAT (port closed)", detail = seen;
			else if (serverPair || outside.size() >= 2)
				nat = "cone", summary = "cone NAT (port closed)", detail = seen;
			else
				nat = "unknown", summary = "unknown (no answer from the server's test socket)", detail = "no answer from the STUN test socket " + AddrString(test) + ", " + seen;
		}
		LobbyLog("udp test: nat=" + nat + ": " + summary + ": " + detail);
		return "nat=" + nat + "\n";
	}

	bool StartUdpTest(uint32_t stunIP, uint16_t stunPort, uint16_t bindPort, UdpReachAsk reachAsk,
		std::function<void(const std::string& natLine, const std::string& summary)> done,
		std::function<bool(std::string& lines)> openPort)
	{
		std::lock_guard lock(g_udpTestMtx);
		if (g_udpTestRunning)
			return false;
		g_udpTest.reset(); // a finished run: its thread ends at once
		g_udpTestRunning = true;
		g_udpTest = std::make_unique<UdpTestRun>();
		UdpTestRun* t = g_udpTest.get();
		t->thread = std::thread([t, stunIP, stunPort, bindPort, reachAsk = std::move(reachAsk), done = std::move(done), openPort = std::move(openPort)] {
			std::string summary;
			std::string line = UdpTest(stunIP, stunPort, bindPort, summary, &t->stop, reachAsk);
			std::string extra;
			if (openPort && !t->stop && line != "nat=open\n" && openPort(extra) && !t->stop)
			{
				std::string retest;
				const std::string again = UdpTest(stunIP, stunPort, bindPort, retest, &t->stop, reachAsk);
				if (again != "nat=unknown\n" || line == "nat=unknown\n")
					line = again, summary = retest;
			}
			if (done && !t->stop)
				done(line + extra, summary);
			g_udpTestRunning = false;
		});
		return true;
	}

	void StopUdpTest()
	{
		std::lock_guard lock(g_udpTestMtx);
		g_udpTest.reset();
	}

	bool IsBattleServer(uint32_t ip, uint16_t port)
	{
		std::lock_guard lock(g_mtx);
		return ip == g_info.serverIP && port == g_info.serverPort;
	}

	std::string LobbyUdpAddr()
	{
		return g_udpAddr;
	}
} // namespace Zdxsv

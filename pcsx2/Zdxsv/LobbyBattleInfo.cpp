// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Lobby message bodies: the battle info notice (ParseBattleInfo) and the match report (EncodeMatchingReport).

#include "Zdxsv/Lobby.h"
#include "Zdxsv/LobbyNet.h"
#include "Zdxsv/Proto.h"
#include "Zdxsv/TestOptions.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace Zdxsv
{
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
		return LobbyParsePeerAddr(host + ":" + std::to_string(port), out);
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
			!LobbyParseAddr(battleServer, info.serverIP, info.serverPort))
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
} // namespace Zdxsv

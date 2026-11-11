// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Lobby message bodies, as JSON: the battle info notice (ParseBattleInfo) and the match report (EncodeMatchingReport).

#include "Zdxsv/Lobby.h"
#include "Zdxsv/LobbyNet.h"
#include "Zdxsv/TestOptions.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

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
	namespace
	{
		using Json = rapidjson::Value;

		// nullptr for a missing or null key
		const Json* JsonField(const Json& o, const char* key)
		{
			const auto it = o.FindMember(key);
			return it == o.MemberEnd() || it->value.IsNull() ? nullptr : &it->value;
		}

		// A missing key leaves out unchanged; a value of another type fails.
		bool JsonStr(const Json& o, const char* key, std::string& out)
		{
			const Json* v = JsonField(o, key);
			if (!v)
				return true;
			if (!v->IsString() || v->GetStringLength() > 256)
				return false;
			out.assign(v->GetString(), v->GetStringLength());
			return true;
		}

		bool JsonInt(const Json& o, const char* key, int64_t& out)
		{
			const Json* v = JsonField(o, key);
			if (!v)
				return true;
			if (!v->IsInt64())
				return false;
			out = v->GetInt64();
			return true;
		}

		bool JsonUint(const Json& o, const char* key, uint64_t& out)
		{
			const Json* v = JsonField(o, key);
			if (!v)
				return true;
			if (!v->IsUint64())
				return false;
			out = v->GetUint64();
			return true;
		}

		bool JsonBool(const Json& o, const char* key, bool& out)
		{
			const Json* v = JsonField(o, key);
			if (!v)
				return true;
			if (!v->IsBool())
				return false;
			out = v->GetBool();
			return true;
		}

		// f(element) for each element of the array key; more than max elements or a non-object fails.
		template <typename F>
		bool JsonObjects(const Json& o, const char* key, size_t max, F f)
		{
			const Json* v = JsonField(o, key);
			if (!v)
				return true;
			if (!v->IsArray() || v->Size() > max)
				return false;
			for (const Json& e : v->GetArray())
				if (!e.IsObject() || !f(e))
					return false;
			return true;
		}

		// "ip" (IPv6 without brackets) and a port -> PeerAddr
		bool JsonPeerAddr(const std::string& ip, int64_t port, PeerAddr& out)
		{
			if (port <= 0 || port > 0xFFFF)
				return false;
			const std::string host = ip.find(':') == std::string::npos ? ip : "[" + ip + "]";
			return LobbyParsePeerAddr(host + ":" + std::to_string(port), out);
		}

		std::vector<int64_t> SplitInts(const std::string& s, char sep)
		{
			std::vector<int64_t> out;
			std::istringstream in(s);
			for (std::string x; std::getline(in, x, sep);)
				out.push_back(std::atoll(x.c_str()));
			return out;
		}
	} // namespace

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
		rapidjson::Document doc;
		doc.Parse(body.data(), body.size());
		if (doc.HasParseError() || !doc.IsObject())
			return false;
		std::vector<User> users;
		std::vector<Candidate> candidates;
		std::vector<Relay> relays;
		std::string battleServer;
		int64_t peerId = -1, pingMs = 0;
		uint64_t session = 0;
		bool uplink = false;
		BattleInfo info;
		const bool ok =
			JsonStr(doc, "battle_code", info.battleCode) && JsonUint(doc, "session_id", session) && session <= 0xFFFFFFFFu &&
			JsonInt(doc, "peer_id", peerId) && JsonInt(doc, "ping_test_duration", pingMs) &&
			JsonObjects(doc, "users", MAX_USERS,
				[&](const Json& e) {
					User& u = users.emplace_back();
					return JsonStr(e, "user_id", u.id) && JsonStr(e, "user_name", u.name) && JsonStr(e, "pilot_name", u.pilot) &&
						   JsonInt(e, "team", u.team) && JsonInt(e, "ggpo_port", u.ggpo) && JsonStr(e, "sync", u.sync);
				}) &&
			JsonObjects(doc, "candidates", MAX_CANDIDATES,
				[&](const Json& e) {
					Candidate& c = candidates.emplace_back();
					return JsonStr(e, "user_id", c.userId) && JsonStr(e, "ip", c.ip) && JsonInt(e, "port", c.port);
				}) &&
			JsonObjects(doc, "relays", MAX_RELAYS,
				[&](const Json& e) {
					Relay& rl = relays.emplace_back();
					return JsonStr(e, "ip", rl.ip) && JsonInt(e, "port", rl.port) && JsonUint(e, "token", rl.token) &&
						   JsonStr(e, "ip6", rl.ip6) && JsonInt(e, "port6", rl.port6);
				}) &&
			JsonBool(doc, "spectator_uplink", uplink) && JsonStr(doc, "lobby_session_id", info.sessionId) &&
			JsonStr(doc, "battle_server", battleServer);
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
				if (c.userId == u.id && u.id != info.userId && JsonPeerAddr(c.ip, c.port, addr))
					peer.addrs.push_back(addr);
			}
			if (!peer.addrs.empty())
				info.p2p.push_back(std::move(peer));
		}
		info.ggpoSession = static_cast<uint32_t>(session);
		info.ggpoPingMs = static_cast<int>(std::clamp<int64_t>(pingMs, 0, MAX_PING_MS));
		info.liveUplink = uplink;
		// a malformed relay ends the list, so the indexes stay the lobby's
		for (const Relay& rl : relays)
		{
			BattleInfo::Relay br;
			br.token = rl.token;
			if (!rl.token || !JsonPeerAddr(rl.ip, rl.port, br.addr) ||
				(!rl.ip6.empty() && !JsonPeerAddr(rl.ip6, rl.port6 ? rl.port6 : rl.port, br.addr6)))
				break;
			info.relays.push_back(br);
		}
		out = std::move(info);
		return true;
	}

	std::string EncodeMatchingReport(const std::string& lines)
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
		enum Kind
		{
			STR,
			INT,
			BOOL,
			NUM,
		};
		// report line key -> JSON key
		static constexpr struct
		{
			const char* line;
			const char* key;
			Kind kind;
		} FIELDS[] = {{"battle_code", "battle_code", STR}, {"players", "player_count", INT}, {"position", "peer_id", INT},
			{"close", "close_reason", STR}, {"frames", "frame_count", INT}, {"delay", "input_delay", INT},
			{"user_id", "user_id", STR}, {"result", "result", STR}, {"reason", "cut_reason", STR}, {"sync", "sync", STR},
			{"sync_differ", "sync_differ", STR}, {"answered", "answered", INT}, {"relays", "relay_count", INT},
			{"ping_error", "ping_error", STR}, {"ping_wait_ms", "ping_wait_ms", INT}, {"min_delay", "min_delay", INT},
			{"fixed_delay", "fixed_delay", BOOL}, {"cut_sends", "cut_sends", INT}, {"rollback_frames", "rollback_frames", INT},
			{"mismatches", "mismatches", INT}, {"disconnected", "disconnected", BOOL}, {"rollback_budget", "rollback_budget", NUM},
			{"rollback_budget_used", "rollback_budget_used", NUM}, {"timesync", "total_timesync", INT},
			{"input_blocks", "input_block_count", INT}};
		rapidjson::StringBuffer sb;
		rapidjson::Writer<rapidjson::StringBuffer> w(sb);
		const auto str = [&w](const char* key, const std::string& v) {
			w.Key(key);
			w.String(v.c_str(), static_cast<rapidjson::SizeType>(v.size()));
		};
		const auto ints = [&w](const char* key, const std::vector<int64_t>& vs) {
			w.Key(key);
			w.StartArray();
			for (const int64_t v : vs)
				w.Int64(v);
			w.EndArray();
		};
		w.StartObject();
		for (const auto& f : FIELDS)
		{
			const auto it = kv.find(f.line);
			if (it == kv.end())
				continue;
			const char* v = it->second.c_str();
			switch (f.kind)
			{
				case STR: str(f.key, it->second); break;
				case INT: w.Key(f.key), w.Int64(std::atoll(v)); break;
				case BOOL: w.Key(f.key), w.Bool(std::atoll(v) != 0); break;
				case NUM: w.Key(f.key), w.Double(std::strtod(v, nullptr)); break;
			}
		}
		if (!peers.empty())
		{
			w.Key("remote_peers");
			w.StartArray();
			for (const auto& [pos, m] : peers)
			{
				w.StartObject();
				w.Key("peer_id");
				w.Int(pos);
				int ping[3];
				if (auto it = m.find("ping"); it != m.end() &&
					std::sscanf(it->second.c_str(), "%d/%d/%d", &ping[0], &ping[1], &ping[2]) == 3 && ping[0] >= 0)
				{
					w.Key("ping_min_ms"), w.Int(ping[0]);
					w.Key("ping_median_ms"), w.Int(ping[1]);
					w.Key("ping_max_ms"), w.Int(ping[2]);
				}
				if (auto it = m.find("path"); it != m.end())
					str("route", it->second);
				if (auto it = m.find("rtt"); it != m.end())
					w.Key("rtt_ms"), w.Int64(std::atoll(it->second.c_str()));
				if (auto it = m.find("addr"); it != m.end())
					str("addr", it->second);
				int loss[2];
				if (auto it = m.find("loss"); it != m.end() && std::sscanf(it->second.c_str(), "%d/%d", &loss[0], &loss[1]) == 2)
				{
					w.Key("recv_loss"), w.Int(loss[0]);
					w.Key("send_loss"), w.Int(loss[1]);
				}
				w.EndObject();
			}
			w.EndArray();
		}
		if (auto it = kv.find("rounds"); it != kv.end())
		{
			w.Key("round_data");
			w.StartArray();
			std::istringstream rs(it->second);
			for (std::string r; std::getline(rs, r, ';');)
			{
				const size_t colon = r.find(':');
				if (colon == std::string::npos)
					continue;
				w.StartObject();
				w.Key("win_team"), w.Int64(std::atoll(r.c_str()));
				ints("used_ms", SplitInts(r.substr(colon + 1), ','));
				w.EndObject();
			}
			w.EndArray();
		}
		if (auto it = kv.find("disconnected_positions"); it != kv.end() && !it->second.empty())
			ints("disconnected_peer_ids", SplitInts(it->second, ','));
		if (auto it = kv.find("fps_history"); it != kv.end() && !it->second.empty())
			ints("fps_history", SplitInts(it->second, ','));
		w.EndObject();
		return std::string(sb.GetString(), sb.GetSize());
	}
} // namespace Zdxsv

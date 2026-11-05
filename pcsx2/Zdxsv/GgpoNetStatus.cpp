// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// GGPO network status: peer states, pings and fps history for the report, the OSD lines (GgpoOsd).

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	namespace
	{
		std::mutex s_osd_mtx;
		std::vector<GgpoOsdLine> s_osd_lines;
		GgpoOsdPanel s_osd_panel;
		Common::Timer::Value s_osd_hold_until = 0; // nonzero: the session ended on a disconnect, its last lines stay up until then

		struct PeerStats
		{
			int state[GGPO_MAX_PLAYERS] = {}; // per position: 0 connected, 1 interrupted, 2 disconnected (GGPO events, for the OSD)
			std::vector<int> pings[GGPO_MAX_PLAYERS]; // ms, sampled once a second for the report
			std::pair<int, int> loss[GGPO_MAX_PLAYERS] = {}; // GGPO packets lost: received, sent
			Common::Timer fps_timer;
			int fps_mark = 0;
			std::vector<int> fps_history; // frames run in each second, for the report
		};
		PeerStats s_peers;

		constexpr u32 OsdColor(u32 r, u32 g, u32 b) { return 0xff000000u | (b << 16) | (g << 8) | r; } // IM_COL32
		constexpr u32 OSD_TEXT = OsdColor(255, 255, 255);
		u32 OsdPingColor(int ms)
		{
			return ms <= 0 ? OsdColor(64, 64, 64) : ms <= 30 ? OsdColor(87, 213, 213) : ms <= 60 ? OsdColor(0, 255, 149) :
			       ms <= 90 ? OsdColor(255, 255, 0) : ms <= 120 ? OsdColor(255, 170, 0) : OsdColor(255, 0, 0);
		}
	} // namespace

	void NetStatusSetPeer(GGPOPlayerHandle h, int state)
	{
		for (int p = 0; p < GGPO_MAX_PLAYERS; p++)
			if (s_handles[p] == h)
				s_peers.state[p] = state;
	}

	void NetStatusSamplePings()
	{
		for (int p = 0; p < s_opt.players; p++)
		{
			GGPONetworkStats st{};
			if (p != s_battle.me && s_peers.state[p] == 0 && ggpo_get_network_stats(s_session, s_handles[p], &st) == GGPO_OK &&
				st.network.ping > 0)
				s_peers.pings[p].push_back(st.network.ping);
			// GGPO's counters are totals and stop once the peer is gone: the last sample is the battle's
			if (p != s_battle.me && s_peers.state[p] == 0 && ggpo_get_network_stats(s_session, s_handles[p], &st) == GGPO_OK)
				s_peers.loss[p] = {st.network.recv_packet_loss, st.network.send_packet_loss};
		}
	}

	void NetStatusSampleFps()
	{
		if (s_peers.fps_timer.GetTimeSeconds() < 1.0 || s_peers.fps_history.size() >= 3600)
			return;
		s_peers.fps_timer.Reset();
		s_peers.fps_history.push_back(s_session_frames - s_peers.fps_mark);
		s_peers.fps_mark = s_session_frames;
	}

	// report lines: disconnected_positions=0,2 (empty: none), ping_<position>=min/median/max ms (none: -1/-1/-1)
	std::string NetStatusReport()
	{
		std::string left, pings;
		for (int p = 0; p < s_opt.players; p++)
		{
			if (s_peers.state[p] == 2)
				left += (left.empty() ? "" : ",") + std::to_string(p);
			if (p == s_battle.me)
				continue;
			std::vector<int> v = s_peers.pings[p];
			std::sort(v.begin(), v.end());
			pings += v.empty() ? fmt::format("ping_{}=-1/-1/-1\n", p) : fmt::format("ping_{}={}/{}/{}\n", p, v.front(), v[v.size() / 2], v.back());
			pings += fmt::format("loss_{}={}/{}\n", p, s_peers.loss[p].first, s_peers.loss[p].second);
		}
		std::string fps;
		for (int f : s_peers.fps_history)
			fps += (fps.empty() ? "" : ",") + std::to_string(f);
		return "disconnected_positions=" + left + "\n" + pings + "timesync=" + std::to_string(s_run.sync_frames) +
		       "\ninput_blocks=" + std::to_string(s_run.waits) + "\nfps_history=" + fps + "\n";
	}

	// net, once per frame: the OSD lines (GgpoOsdLines). log: also to the log, one line.
	// With the OSD off, shown only while a peer is interrupted or disconnected.
	void NetStatusUpdate(bool log)
	{
		if (!s_opt.net || !s_session)
			return;
		if (!s_opt.osd && std::all_of(std::begin(s_peers.state), std::end(s_peers.state), [](int s) { return s == 0; }))
		{
			std::lock_guard lock(s_osd_mtx);
			s_osd_lines.clear();
			s_osd_panel = {};
			return;
		}
		std::vector<GgpoOsdLine> lines;
		lines.push_back({fmt::format("Delay {}fr", s_opt.delay), s_opt.delay >= 13 ? OsdColor(255, 38, 31) : s_opt.delay >= 10 ? OsdColor(255, 128, 0) :
		                                                      s_opt.delay >= 5   ? OsdColor(255, 217, 0) : OSD_TEXT});
		lines.push_back({fmt::format("Roll {}  Wait {}  Sync {}", s_stats.rollback_frames, s_run.waits, s_run.sync_frames), OSD_TEXT});
		for (int p = 0; p < s_opt.players; p++)
		{
			if (p == s_battle.me)
				continue;
			const LobbyPlayer who = p < static_cast<int>(s_net_players.size()) ? s_net_players[p] : LobbyPlayer{};
			lines.push_back({fmt::format("{}P {}", p + 1, who.id), OSD_TEXT});
			if (!who.name.empty())
				lines.push_back({" " + who.name, OSD_TEXT});
			if (!who.pilot.empty())
				lines.push_back({" " + who.pilot, OSD_TEXT});
			GGPONetworkStats st{};
			if (s_peers.state[p] == 2)
				lines.push_back({" Disconnected", OsdPingColor(999)});
			else if (ggpo_get_network_stats(s_session, s_handles[p], &st) == GGPO_OK)
			{
				// Interrupted in place of the ping, so the line fits the OSD's fixed width
				if (s_peers.state[p] == 1)
					lines.push_back({fmt::format(" Interrupted  P {}", st.sync.predicted_frames), OsdPingColor(999)});
				else
					lines.push_back({fmt::format(" Ping {}ms{}  P {}", st.network.ping,
					                     p < static_cast<int>(s_net_via.size()) && s_net_via[p] ? " (R)" : "", st.sync.predicted_frames),
						OsdPingColor(st.network.ping)});
			}
		}
		if (log)
		{
			std::string all;
			for (const GgpoOsdLine& l : lines)
				all += (all.empty() ? "" : " |") + l.text;
			Console.WriteLn("ZdxsvGgpo: osd frame %d: %s", s_session_frames, all.c_str());
		}
		GgpoOsdPanel panel;
		panel.delay = s_opt.delay;
		panel.rollback_frames = s_stats.rollback_frames;
		panel.sync_frames = s_run.sync_frames;
		GGPONetworkStats own{};
		if (ggpo_get_network_stats(s_session, s_handles[s_battle.me], &own) == GGPO_OK)
			panel.predicted_frames = own.sync.predicted_frames;
		for (int p = 0; p < s_opt.players; p++)
		{
			if (p == s_battle.me)
				continue;
			GgpoOsdPeer peer;
			peer.position = p;
			if (p < static_cast<int>(s_net_players.size()))
			{
				peer.id = s_net_players[p].id;
				peer.name = s_net_players[p].name;
				peer.pilot = s_net_players[p].pilot;
			}
			peer.connected = s_peers.state[p] != 2;
			GGPONetworkStats st{};
			if (peer.connected && ggpo_get_network_stats(s_session, s_handles[p], &st) == GGPO_OK)
			{
				peer.relay = st.network.is_relay != 0 || (p < static_cast<int>(s_net_via.size()) && s_net_via[p] != 0);
				peer.ping = st.network.ping;
				peer.recv_loss = st.network.recv_packet_loss;
				peer.send_loss = st.network.send_packet_loss;
				peer.send_queue = st.network.send_queue_len;
				peer.frames_behind = st.timesync.local_frames_behind;
			}
			panel.peers.push_back(std::move(peer));
		}
		std::lock_guard lock(s_osd_mtx);
		s_osd_lines = std::move(lines);
		s_osd_panel = std::move(panel);
	}

	// GgpoStop: after a disconnect the lines stay up 10 s, so the player sees who left.
	void NetStatusHold(bool disconnected)
	{
		std::lock_guard lock(s_osd_mtx);
		if (disconnected && !s_osd_lines.empty())
		{
			s_osd_hold_until = Common::Timer::GetCurrentValue() + Common::Timer::ConvertSecondsToValue(10.0);
			std::string all;
			for (const GgpoOsdLine& l : s_osd_lines)
				all += (all.empty() ? "" : " |") + l.text;
			Console.WriteLn("ZdxsvGgpo: osd held 10 s: %s", all.c_str());
		}
		else
			s_osd_lines.clear();
		s_osd_panel = {};
	}

	void NetStatusPeersConnected()
	{
		std::fill(std::begin(s_peers.state), std::end(s_peers.state), 0);
	}

	void NetStatusReset()
	{
		s_peers = {};
		std::lock_guard lock(s_osd_mtx);
		s_osd_hold_until = 0;
		s_osd_lines.clear();
		s_osd_panel = {};
	}

	bool GgpoOsd(GgpoOsdPanel& panel)
	{
		std::lock_guard lock(s_osd_mtx);
		if (s_osd_hold_until && Common::Timer::GetCurrentValue() >= s_osd_hold_until)
		{
			s_osd_hold_until = 0;
			s_osd_lines.clear();
			s_osd_panel = {};
			Console.WriteLn("ZdxsvGgpo: osd hold over");
		}
		if (s_osd_lines.empty())
			return false;
		panel = s_osd_panel;
		return true;
	}
} // namespace Zdxsv

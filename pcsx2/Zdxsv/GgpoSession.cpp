// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// The GGPO session lifecycle: the GGPO callbacks, start and stop, the net session's per-frame calls
// (local input, prediction limit wait, synchronized inputs, end of battle).

#include "Zdxsv/GgpoShared.h"
#include "Zdxsv/SyncSettings.h"

namespace Zdxsv
{
	namespace
	{
		// The game builds a frame's draw list in its render callbacks, VU1 draws it in the next frame, and
		// the picture is shown one vblank later: the first present after a rollback shows what VU1 drew in
		// the last rerun frame, from the list of the one before. So only the last two rerun frames run the
		// render callbacks (g_rerun_draw_skip) and only the last starts VU1 microprograms (g_rerun_vu1_skip).
		// With g_single_draw_list no rerun frame draws: GS memory is not rolled back, so the first present
		// after a rollback shows the frame drawn before the rollback (the predicted one), not an older one.
		// ZDXSV_RERUN_TAIL=n: the last n rerun frames run the callbacks and the last n - 1 VU1, or n with
		// g_single_draw_list (default 2; 0 with it). Without g_single_draw_list one less breaks the picture
		// after each rollback (tests/zdxsv/rerunpic.py).
		const int s_rerun_tail = [] {
			const char* e = TestEnv("ZDXSV_RERUN_TAIL");
			return e ? std::max(0, std::atoi(e)) : -1;
		}();
		// ZDXSV_RERUN_VU1=1: VU1 in every rerun frame.
		const bool s_rerun_vu1 = [] {
			const char* e = TestEnv("ZDXSV_RERUN_VU1");
			return e && e[0] == '1';
		}();
		// ZDXSV_RERUN_EE_DRAW=1|0: render callbacks in every rerun frame or not; unset: per game.
		const int s_rerun_ee_draw_env = [] {
			const char* e = TestEnv("ZDXSV_RERUN_EE_DRAW");
			return e ? (e[0] == '1' ? 1 : 0) : -1;
		}();
		// SLPS-25305: the CPU players' logic reads what the render callbacks change, so skipping them desyncs
		// the CPU players after a rollback.
		bool RerunEeDraw() { return s_rerun_ee_draw_env >= 0 ? s_rerun_ee_draw_env == 1 : g_aeug_game; }

		void NetLoaded(int frame)
		{
			s_battle.frame = frame;
			const int end_frame = s_rb.end_frame;
			s_rb = s_net_at[frame & 127];
			if (end_frame >= 0 && s_rb.end_frame < 0)
				Console.WriteLn("ZdxsvGgpo: net end msg of frame %d rolled back to frame %d", end_frame, frame);
		}

		bool __cdecl BeginGame(const char*) { return true; }
		bool __cdecl OnEvent(GGPOEvent* ev)
		{
			switch (ev->code)
			{
				case GGPO_EVENTCODE_RUNNING:
					s_run.running = true;
					Console.WriteLn("ZdxsvGgpo: running");
					break;
				case GGPO_EVENTCODE_TIMESYNC:
					s_run.frames_ahead = ev->u.timesync.frames_ahead;
					break;
				case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
					if (s_battle.end >= 0)
						break;
					s_run.disconnected = true;
					NetStatusSetPeer(ev->u.disconnected.player, 2);
					Console.Warning("ZdxsvGgpo: peer disconnected");
					break;
				case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
					NetStatusSetPeer(ev->u.connection_interrupted.player, 1);
					Console.Warning("ZdxsvGgpo: connection interrupted");
					break;
				case GGPO_EVENTCODE_CONNECTION_RESUMED:
					NetStatusSetPeer(ev->u.connection_resumed.player, 0);
					break;
				default:
					break;
			}
			return true;
		}

		bool __cdecl SaveGameState(unsigned char** buffer, int* len, int* checksum, int frame)
		{
			Common::Timer timer;
			int confirmed = -1;
			// A confirmed frame (every input received) is never a rollback target.
			if (!s_opt.sync && ggpo_get_last_confirmed_frame(s_session, &confirmed) == GGPO_OK && frame <= confirmed)
				s_run.save_skipped++;
			else if (!Zdxsv::DeltaStateSave(frame))
				return false;
			if (s_opt.net)
				NetSaved(frame);
			s_stats.save_ms.Add(timer.GetTimeMilliseconds());
			timer.Reset();
			*checksum = 0;
			if (s_opt.sync)
				HashSave(frame, checksum);
			s_stats.hash_ms.Add(timer.GetTimeMilliseconds());
			if (!s_rerun)
				s_run.top_frame = frame;
			int* saved = new int(frame);
			*buffer = reinterpret_cast<unsigned char*>(saved);
			*len = sizeof(int);
			// GGPO never goes back further than its check distance / prediction window.
			Zdxsv::DeltaStateDiscardBefore(frame - std::max(s_opt.check, 8) - 4);
			return true;
		}

		bool __cdecl LoadGameState(unsigned char* buffer, int len)
		{
			if (len != sizeof(int))
				return false;
			Common::Timer timer;
			RerunKeepBeforeLoad(!RerunEeDraw());
			const bool ok = Zdxsv::DeltaStateLoad(*reinterpret_cast<int*>(buffer));
			RerunKeepAfterLoad();
			s_run.reruns_left = s_run.top_frame - *reinterpret_cast<int*>(buffer);
			if (s_opt.net)
				NetLoaded(*reinterpret_cast<int*>(buffer));
			s_stats.load_ms.Add(timer.GetTimeMilliseconds());
			s_stats.loads++;
			return ok;
		}

		bool __cdecl LogGameState(char* filename, unsigned char* buffer, int len)
		{
			// Called by the synctest on a checksum mismatch, for the original and the rerun.
			if (std::strstr(filename, "original"))
			{
				if (s_stats.mismatches++ < 20)
					Console.WriteLn("ZdxsvGgpo: MISMATCH %s (session frame %d)", filename, s_session_frames);
			}
			return true;
		}

		void __cdecl FreeBuffer(void* buffer)
		{
			delete static_cast<int*>(buffer);
		}

		bool __cdecl AdvanceFrame(int)
		{
			if (!GgpoSyncAndApply(true))
				return false;
			g_ggpo_in_rollback = true;
			Common::Timer timer;
			EeProfileOnRerun();
			const u64 cycle0 = cpuRegs.cycle;
			--s_run.reruns_left;
			const int tail = s_rerun_tail >= 0 ? s_rerun_tail : (g_single_draw_list ? 0 : 2);
			g_rerun_vu1_skip = !s_rerun_vu1 && s_run.reruns_left >= tail - (g_single_draw_list ? 0 : 1);
			g_rerun_draw_skip = !RerunEeDraw() && s_run.reruns_left >= tail;
			const bool ok = GgpoRunFrame();
			if (g_rerun_draw_skip)
				RerunKeepAfterRerunFrame(s_run.reruns_left == 0);
			g_rerun_vu1_skip = g_rerun_draw_skip = false;
			s_stats.rerun_mcycles.Add((cpuRegs.cycle - cycle0) / 1e6);
			s_stats.rerun_ms.Add(timer.GetTimeMilliseconds());
			g_ggpo_in_rollback = false;
			s_rerun = true;
			ggpo_advance_frame(s_session);
			s_rerun = false;
			s_stats.rollback_frames++;
			return ok;
		}

		void OnLog(int level, const char* msg)
		{
			if (level > GGPO_LOG_WARNING)
				return;
			if (s_stats.ggpo_warnings++ < 20)
				Console.Warning("ZdxsvGgpo: ggpo: %s", msg);
		}


		const char* NetStart(GGPOSessionCallbacks& cb)
		{
			if (s_battle.me < 0 || s_battle.me >= s_opt.players)
			{
				Console.Error("ZdxsvGgpo: net position %d not below players=%d", s_battle.me, s_opt.players);
				return "start failed";
			}
			const int local_port = s_opt.lobby ? s_opt.port : s_opt.port + s_battle.me;
			const std::string code = IdValue(ReplayIds(), "battle_code");
			if (s_net_trace) // battle start marker (pwcheck.py --battle): BATTLES=N lobby battles in one process
				std::fprintf(s_net_trace, "%u B %s\n", g_FrameCount, code.empty() ? "-" : code.c_str());
			// GGPO's sync handshake compares this: a peer of another session (an old battle, a stray
			// packet) is refused. ZDXSV_NET_SESSION=s replaces it (test: a peer of another session).
			const char* session_env = Zdxsv::TestEnv("ZDXSV_NET_SESSION");
			const std::string session = (session_env ? std::string(session_env) : code).substr(0, 256); // GGPO's max
			if (ggpo_start_session(&s_session, &cb, "zdxsv", s_opt.players, sizeof(NetInput), static_cast<unsigned short>(local_port),
					session.empty() ? nullptr : session.data(), static_cast<int>(session.size())) != GGPO_OK)
			{
				s_session = nullptr;
				return "start failed";
			}
			// ZDXSV_NET_DISCONNECT_MS=ms (default 10000): longer lets a peer stall (ZDXSV_RAM_DUMP) without a disconnect.
			const char* dms = Zdxsv::TestEnv("ZDXSV_NET_DISCONNECT_MS");
			ggpo_set_disconnect_timeout(s_session, dms ? std::atoi(dms) : 10000);
			ggpo_set_disconnect_notify_start(s_session, 1000);
			// A frame is confirmed once this peer has its inputs; otherwise, with 3+ players, also once every peer reported them: one more one-way trip of prediction.
			ggpo_set_disconnect_without_rollback(s_session, true);
			g_limiter_no_catch_up = true;
			GgpoPriority(true);
			// relay servers before the players, in the battle info's order on every peer (ggpo_add_relay_server)
			if (s_opt.lobby)
				for (const Zdxsv::RelayServerAddr& r : s_net_servers)
				{
					if (ggpo_add_relay_server(s_session, r.addr.ip.c_str(), r.addr.port, r.alt.ip.empty() ? nullptr : r.alt.ip.c_str()) != GGPO_OK)
						return "start failed";
					Console.WriteLn("ZdxsvGgpo: relay server %s%s%s", r.addr.String().c_str(), r.alt.ip.empty() ? "" : " alt ",
						r.alt.ip.empty() ? "" : r.alt.String().c_str());
					for (const Zdxsv::PeerAddr& a : r.sources)
						if (ggpo_add_relay_server_source(s_session, a.ip.c_str(), a.port) != GGPO_OK)
							return "start failed";
				}
			for (int p = 0; p < s_opt.players; p++)
			{
				GGPOPlayer player{};
				player.size = sizeof(GGPOPlayer);
				player.player_num = p + 1;
				player.type = (p == s_battle.me) ? GGPO_PLAYERTYPE_LOCAL : GGPO_PLAYERTYPE_REMOTE;
				if (player.type == GGPO_PLAYERTYPE_REMOTE && s_opt.lobby)
				{
					const Zdxsv::PeerAddr& a = s_net_peers[p];
					StringUtil::Strlcpy(player.u.remote.ip_address, a.ip.c_str(), sizeof(player.u.remote.ip_address));
					player.u.remote.port = a.port;
					player.u.remote.relay = p < static_cast<int>(s_net_via.size()) && s_net_via[p] != 0;
					// a relay is a zdxsv server, not a player: its address stays whole
					Console.WriteLn("ZdxsvGgpo: lobby peer position %d at %s%s", p,
						(player.u.remote.relay ? a.String() : a.Masked()).c_str(), player.u.remote.relay ? " (relay)" : "");
				}
				else if (player.type == GGPO_PLAYERTYPE_REMOTE)
				{
					StringUtil::Strlcpy(player.u.remote.ip_address, s_opt.peer_host.c_str(), sizeof(player.u.remote.ip_address));
					player.u.remote.port = static_cast<unsigned short>(s_opt.relay ? s_opt.relay + 8 * s_battle.me + p : s_opt.port + p);
				}
				if (ggpo_add_player(s_session, &player, &s_handles[p]) != GGPO_OK)
					return "start failed";
				if (player.type == GGPO_PLAYERTYPE_LOCAL)
					ggpo_set_frame_delay(s_session, s_handles[p], s_opt.delay + 1); // + 1: the input is added before the advance
			}
			// Every peer arms at its own first key msg: block until all are synchronized.
			constexpr double START_TIMEOUT_S = 10;
			Common::Timer wait;
			while (!s_run.running && wait.GetTimeSeconds() < START_TIMEOUT_S)
			{
				if (ggpo_idle(s_session, 0) == GGPO_ERRORCODE_VERIFICATION_ERROR)
				{
					Console.Error("ZdxsvGgpo: a peer is in another session (battle code mismatch)");
					// Refuse the peer's sync requests for 1 s, so it can close as a mismatch too. Best effort: GGPO resends
					// a request only after 500 ms without any send, and its replies to ours count as sends.
					for (Common::Timer linger; linger.GetTimeSeconds() < 1;)
					{
						ggpo_idle(s_session, 0);
						Threading::Sleep(1);
					}
					return "session mismatch";
				}
				Threading::Sleep(1);
			}
			if (!s_run.running)
			{
				Console.Error("ZdxsvGgpo: net peers not synchronized in %.0f s", START_TIMEOUT_S);
				return "start timeout";
			}
			Console.WriteLn("ZdxsvGgpo: net player %d of %d port %d delay %d, %zu msgs sent before the start, waited %.1f s",
				s_battle.me + 1, s_opt.players, local_port, s_opt.delay, s_battle.sent.size(), wait.GetTimeSeconds());
			return nullptr;
		}
	} // namespace

	// s_ggpo_mtx guards every GGPO call: the frame loop holds it through Returned.
	std::recursive_mutex s_ggpo_mtx;

	void GgpoStop(const char* what)
	{
		std::lock_guard ggpo_lock(s_ggpo_mtx);
		if (s_run.disconnected)
			NetStatusUpdate(false); // the frame's lines predate the disconnect event
		if (s_session)
			ggpo_close_session(s_session);
		s_session = nullptr;
		ggpo_set_log_function(nullptr);
		g_ggpo_active = false;
		if (!s_vm_closing)
			Host::RunOnCPUThread([] { SyncSettingsOnBattle(false); });
		Zdxsv::DeltaStateClear();
		GgpoReport(what);
		GgpoBudgetSessionEnd(s_opt.net);
		NetStatusHold(s_run.disconnected);
		const std::string peers = s_opt.net ? NetStatusReport() : std::string();
		if (s_opt.net)
		{
			std::string line = peers;
			std::replace(line.begin(), line.end(), '\n', ' ');
			Console.WriteLn("ZdxsvGgpo: peer report: %s", line.c_str());
		}
		if (s_opt.lobby)
		{
			std::lock_guard lock(s_lobby_mtx);
			if (const std::string rounds = ReplayRoundsReport(); !s_report.empty() && !rounds.empty())
				s_report += "rounds=" + rounds + "\n";
			if (!s_report.empty())
				s_report += std::string("close=") + what + "\nframes=" + std::to_string(s_session_frames) +
				            "\nrollback_frames=" + std::to_string(s_stats.rollback_frames) + "\nmismatches=" + std::to_string(s_stats.mismatches) +
				            "\ndisconnected=" + (s_run.disconnected ? "1" : "0") + "\n" + peers;
		}
		if (s_opt.net)
		{
			s_battle.over = true;
			s_battle.closed = std::strcmp(what, "net battle end") == 0 || std::strcmp(what, "disconnected") == 0;
			ReplayWrite(what);
			NetReport();
		}
		if (s_rbk && !s_play_env && !s_vm_closing)
		{
			Console.WriteLn("ZdxsvGgpo: rbk exit (%s) at vsync %u", what, g_FrameCount);
			Host::RunOnCPUThread([] { Host::RequestVMShutdown(false, false, false); });
		}
	}

	// nullptr, or the close reason
	const char* GgpoStart()
	{
		ggpo_set_log_function(OnLog);
		GGPOSessionCallbacks cb{};
		cb.begin_game = BeginGame;
		cb.save_game_state = SaveGameState;
		cb.load_game_state = LoadGameState;
		cb.log_game_state = LogGameState;
		cb.free_buffer = FreeBuffer;
		cb.advance_frame = AdvanceFrame;
		cb.on_event = OnEvent;
		if (s_opt.net)
			return NetStart(cb);
		if (ggpo_start_synctest(&s_session, &cb, "zdxsv", GGPO_PLAYERS, sizeof(Input), s_opt.check) != GGPO_OK)
		{
			s_session = nullptr;
			return "start failed";
		}
		ggpo_idle(s_session, 0);
		for (int p = 0; p < GGPO_PLAYERS; p++)
		{
			GGPOPlayer player{sizeof(GGPOPlayer), GGPO_PLAYERTYPE_LOCAL, p + 1};
			if (ggpo_add_player(s_session, &player, &s_handles[p]) != GGPO_OK)
				return "start failed";
		}
		s_ggpo_rng.seed(s_opt.seed);
		Console.WriteLn("ZdxsvGgpo: synctest start=%d frames=%d check=%d seed=%u input=%s mask=%04x control=%d hash=%s",
			s_opt.start, s_opt.frames, s_opt.check, s_opt.seed, s_opt.host_input ? "host" : s_opt.no_input ? "none" : "random", s_opt.mask, s_opt.control_input,
			s_opt.hash == Hash::Full ? "full" : s_opt.hash == Hash::Pos ? "pos" : "pw");
		return nullptr;
	}

	// net: the battle ended once every input of the frame that sent the end msg is here (a mispredicted
	// timeline can send one that the rerun does not). The session then leaves the peers, so it never waits
	// for a peer that already stopped; s_net_tail frames still run for the game. Inputs after the confirmed
	// frame are no longer the peers': the replay and the pw hashes stop there.
	void NetEndCheck()
	{
		if (!s_opt.net || s_battle.end >= 0 || s_rb.end_frame < 0)
			return;
		int confirmed = INT_MAX;
		ggpo_get_last_confirmed_frame(s_session, &confirmed);
		if (confirmed < s_rb.end_frame)
			return;
		s_battle.end = 0;
		// A peer that left first had confirmed this end msg: it ended the battle, it did not drop out. Its
		// inputs after the end msg frame may be missing, and our own inputs then count as confirmed.
		s_battle.last_real = s_run.disconnected ? s_rb.end_frame : confirmed;
		if (s_run.disconnected)
		{
			s_run.disconnected = false;
			NetStatusPeersConnected();
		}
		Console.WriteLn("ZdxsvGgpo: net end msg of frame %d confirmed at frame %d, leaving the peers", s_rb.end_frame, s_battle.frame);
		ggpo_disconnect_player(s_session, s_handles[s_battle.me]);
	}

	bool NetAddLocalInput(int frame)
	{
		NetInput in = NetPack(s_rand_env ? RandInput() : GgpoHostInput(), frame);
		GGPOErrorCode rc = ggpo_add_local_input(s_session, s_handles[s_battle.me], &in, sizeof(in));
		if (rc == GGPO_ERRORCODE_PREDICTION_THRESHOLD)
		{
			s_run.waits++;
			Common::Timer wait;
			while (rc == GGPO_ERRORCODE_PREDICTION_THRESHOLD && !s_run.disconnected && wait.GetTimeSeconds() < 10)
			{
				ggpo_idle(s_session, 0);
				Threading::Sleep(1);
				NetEndCheck();
				rc = ggpo_add_local_input(s_session, s_handles[s_battle.me], &in, sizeof(in));
			}
			s_stats.wait_ms.Add(wait.GetTimeMilliseconds());
		}
		if (rc != GGPO_OK)
		{
			Console.Error("ZdxsvGgpo: net add_local_input %d", rc);
			return false;
		}
		return true;
	}

	// The input was added before the advance, so add_local_input no longer stops the next frame at GGPO's
	// prediction limit: the same check here, after the rollback.
	bool NetWaitPredictionLimit()
	{
		constexpr int LIMIT = 6; // MAX_PREDICTION_FRAMES
		const auto at_limit = [] {
			int frame = 0, confirmed = 0;
			ggpo_get_current_frame(s_session, &frame);
			ggpo_get_last_confirmed_frame(s_session, &confirmed);
			return frame >= LIMIT && frame - confirmed >= LIMIT;
		};
		if (!at_limit())
			return true;
		s_run.waits++;
		Common::Timer wait;
		while (at_limit() && !s_run.disconnected && wait.GetTimeSeconds() < 10)
		{
			ggpo_idle(s_session, 0);
			Threading::Sleep(1);
			NetEndCheck();
		}
		s_stats.wait_ms.Add(wait.GetTimeMilliseconds());
		return s_run.disconnected || !at_limit();
	}

	// Inputs of frame s_battle.frame: own host pad to pad 0, every position's (A, B) to s_rings.zd_hist, kind-3
	// msgs with a new seq to the barrier (the n-th of each remote to recv once all peers' n-th arrived).
	bool NetSyncAndApply()
	{
		NetInput in[GGPO_MAX_PLAYERS] = {};
		int disconnect_flags = 0;
		const GGPOErrorCode rc = ggpo_synchronize_input(s_session, in, sizeof(NetInput) * s_opt.players, &disconnect_flags);
		if (rc != GGPO_OK)
		{
			Console.Error("ZdxsvGgpo: net synchronize_input %d", rc);
			return false;
		}
		NetApply(in);
		return true;
	}
} // namespace Zdxsv

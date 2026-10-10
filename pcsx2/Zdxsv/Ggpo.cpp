// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// The GGPO session: options, frame loop, GGPO callbacks, start and stop, VM hooks, network status OSD.
// ZDXSV_GGPO="key=value,..." (docs/zdxsv/options.md) replaces the ZdxsvGgpo setting's DEFAULT_OPTIONS.
// Without net=1 the session is a synctest; log lines start with "ZdxsvGgpo".

#include "Zdxsv/GgpoShared.h"
#include "Zdxsv/SyncSettings.h"
#include "Zdxsv/ReplayList.h"
#include "MTGS.h"

namespace Zdxsv
{
	namespace
	{
		constexpr const char* DEFAULT_OPTIONS = "net=1,lobby=1";
		constexpr int PLAYERS = 2;
		int s_relay = 0; // relay=R (net): remote p is at port R + 8 * me + p (tools/zdxsv/udprelay.py per pair), not port + p
		RollbackState s_net_at[128]; // per frame & 127, at its save
		// ZDXSV_NET_TAIL=n: frames run after the end msg before the session stops (default 300).
		const int s_net_tail = [] {
			const char* e = Zdxsv::TestEnv("ZDXSV_NET_TAIL");
			return e ? std::atoi(e) : 300;
		}();
		bool NetStart(GGPOSessionCallbacks& cb);
		bool NetNextInputs();
		bool NetSyncAndApply();
		void NetLoaded(int frame);

		struct Stat
		{
			double sum = 0, max = 0;
			int n = 0;
			void Add(double v)
			{
				sum += v;
				max = std::max(max, v);
				n++;
			}
			double Mean() const { return n ? sum / n : 0; }
		};

		int s_start = 1500, s_frames = 3000, s_check = 6;
		u32 s_seed = 1;
		bool s_host_input = false, s_no_input = false, s_control_input = false;
		u16 s_mask = 0xffff;
		bool s_sync = true; // sync=0: no state hashes (checksum 0)
		std::string s_peer_host = "127.0.0.1";
		bool s_osd = true; // osd=
		std::mutex s_osd_mtx;
		std::vector<GgpoOsdLine> s_osd_lines;
		bool s_running = false; // net: GGPO_EVENTCODE_RUNNING seen
		bool s_disconnected = false;
		int s_frames_ahead = 0; // net: last GGPO_EVENTCODE_TIMESYNC
		int s_waits = 0; // net: frames that waited for a peer
		// ZDXSV_SAVE_ALL=1: delta-save every GGPO frame. Default (net, sync=0): skip the save of a frame at or
		// below GGPO's last confirmed frame (all inputs received: never a rollback target).
		const bool s_save_all = [] {
			const char* e = std::getenv("ZDXSV_SAVE_ALL");
			return e && e[0] == '1';
		}();
		int s_save_skipped = 0;
		// ZDXSV_RERUN_DRAW=0: the GS drops the draw calls of rollback rerun frames (g_gs_skip_draws): they
		// are not presented, and the next frame draws over them. Default: drawn.
		const bool s_rerun_draw = [] {
			const char* e = std::getenv("ZDXSV_RERUN_DRAW");
			return !(e && e[0] == '0');
		}();
		// The game builds a frame's draw list in its render callbacks, VU1 draws it in the next frame, and
		// the picture is shown one vblank later: the first present after a rollback shows what VU1 drew in
		// the last rerun frame, from the list of the one before. So only the last two rerun frames run the
		// render callbacks (g_rerun_draw_skip) and only the last starts VU1 microprograms (g_rerun_vu1_skip).
		// ZDXSV_RERUN_TAIL=n: the last n rerun frames run the callbacks and the last n - 1 VU1 (default 2;
		// 1 breaks the picture after each rollback, tests/zdxsv/rerunpic.sh).
		const int s_rerun_tail = [] {
			const char* e = std::getenv("ZDXSV_RERUN_TAIL");
			return e ? std::atoi(e) : 2;
		}();
		// ZDXSV_RERUN_VU1=1: VU1 in every rerun frame.
		const bool s_rerun_vu1 = [] {
			const char* e = std::getenv("ZDXSV_RERUN_VU1");
			return e && e[0] == '1';
		}();
		// ZDXSV_RERUN_EE_DRAW=1: render callbacks in every rerun frame.
		const bool s_rerun_ee_draw = [] {
			const char* e = std::getenv("ZDXSV_RERUN_EE_DRAW");
			return e && e[0] == '1';
		}();
		// LowLatencyVsync: present, rollback, then sleep and poll input (GgpoDeferThrottle). A corrected frame
		// shows one frame later. ZDXSV_PRESENT_FIRST=0: sleep and poll in VSyncStart, the rollback after them.
		const bool s_present_first = [] {
			const char* e = std::getenv("ZDXSV_PRESENT_FIRST");
			return !(e && e[0] == '0');
		}();
		bool s_throttle_deferred = false;
		int s_top_frame = 0; // the frame of the last save outside a rollback
		int s_reruns_left = 0; // rerun frames left in this rollback
		GGPOPlayerHandle s_handles[GGPO_MAX_PLAYERS] = {};
		bool s_vm_closing = false; // in GgpoOnVmShutdown: Stop does not shut the VM down (rbk)
		int s_vsyncs = 0;
		std::mt19937 s_rng;
		Input s_random[PLAYERS] = {};

		int s_rollback_frames = 0, s_loads = 0, s_mismatches = 0, s_ggpo_warnings = 0;
		Stat s_save_ms, s_hash_ms, s_load_ms;
		Stat s_rerun_ms, s_wait_ms; // between frames: rollback rerun emulation, net wait for a peer
		Stat s_sleep_ms; // between frames: limiter sleep and input poll deferred by GgpoDeferThrottle
		Stat s_rerun_mcycles; // EE cycles (millions) per rerun frame
		Stat s_emu_ms, s_exit_ms, s_ours_ms; // wall: last GgpoOnExecuteReturned end -> GgpoOnVsync -> GgpoOnExecuteReturned start -> its end
		Common::Timer::Value s_t_vsync = 0, s_t_returned = 0;
		// Output of the session: GS frames presented (g_perfmon, GS thread; read here for the report only)
		// since the start, SPU2 samples played and dropped in rerun frames.
		int s_gs_frame0 = 0;
		s64 s_spu_played = 0, s_spu_dropped = 0;

		void Parse()
		{
			s_osd = Host::GetBoolSettingValue("DEV9/Eth", "ZdxsvNetOsd", true); // osd= overrides it
			for (const std::string_view item : StringUtil::SplitString(s_options, ','))
			{
				const size_t eq = item.find('=');
				if (eq == std::string_view::npos)
					continue;
				const std::string_view key = item.substr(0, eq);
				const std::string_view value = item.substr(eq + 1);
				const int n = StringUtil::FromChars<int>(value).value_or(0);
				if (key == "start")
					s_start = n;
				else if (key == "frames")
					s_frames = n;
				else if (key == "hash")
					s_hash = value == "full" ? Hash::Full : value == "pos" ? Hash::Pos : Hash::Pw;
				else if (key == "check")
					s_check = std::clamp(n, 1, 6); // GGPO keeps MAX_PREDICTION_FRAMES + 2 = 8 states: frames 0..check
				else if (key == "seed")
					s_seed = static_cast<u32>(n);
				else if (key == "input")
				{
					s_host_input = (value == "host");
					s_no_input = (value == "none");
				}
				else if (key == "mask")
					s_mask = StringUtil::FromChars<u16>(value, 16).value_or(0xffff);
				else if (key == "control")
					s_control_input = (value == "input");
				else if (key == "port")
					s_port = n;
				else if (key == "host")
					s_peer_host = std::string(value);
				else if (key == "delay")
				{
					s_delay = n;
					s_delay_set = true;
				}
				else if (key == "mindelay")
					s_min_delay = n;
				else if (key == "sync")
					s_sync = (n != 0);
				else if (key == "net")
					s_net = (n != 0);
				else if (key == "players")
					s_players = std::clamp(n, 2, GGPO_MAX_PLAYERS);
				else if (key == "relay")
					s_relay = n;
				else if (key == "lobby")
					s_lobby = (n != 0);
				else if (key == "badsession")
					s_bad_session = (n != 0);
				else if (key == "osd")
					s_osd = (n != 0);
				else if (key == "upload")
					s_upload_url = value;
				else if (key == "advertise")
					; // GgpoLobbyAdvertisePort
				else if (key == "replay")
				{
					s_replay_off = (value == "0");
					s_replay_dir = s_replay_off ? std::string() : Path::ToNativePath(value); // '/' fails on Windows
				}
				else
					Console.Warning("ZdxsvGgpo: unknown key '%.*s'", static_cast<int>(key.size()), key.data());
			}
			if (s_net)
			{
				s_sync = false; // the peers run different games (own position): no common hash
				s_frames = std::numeric_limits<int>::max(); // ends with the battle (end msg)
			}
		}

		void Report(const char* what)
		{
			Console.WriteLn("ZdxsvGgpo: %s frames %d rollback frames %d loads %d mismatches %d ggpo warnings %d | save ms mean %.3f max %.3f skipped %d | hash ms mean %.3f | load ms mean %.3f max %.3f",
				what, s_session_frames, s_rollback_frames, s_loads, s_mismatches, s_ggpo_warnings, s_save_ms.Mean(), s_save_ms.max, s_save_skipped,
				s_hash_ms.Mean(), s_load_ms.Mean(), s_load_ms.max);
			Console.WriteLn("ZdxsvGgpo: %s wall ms per frame: emulate mean %.2f max %.1f | exit %.2f | between frames (save, ggpo, rollbacks) mean %.2f max %.1f | rerun frame mean %.2f max %.1f",
				what, s_emu_ms.Mean(), s_emu_ms.max, s_exit_ms.Mean(), s_ours_ms.Mean(), s_ours_ms.max, s_rerun_ms.Mean(), s_rerun_ms.max);
			const double n = std::max(s_session_frames, 1);
			const double ours = s_ours_ms.sum / n,
				split = (s_save_ms.sum + s_hash_ms.sum + s_load_ms.sum + s_rerun_ms.sum + s_wait_ms.sum + s_sleep_ms.sum) / n;
			Console.WriteLn("ZdxsvGgpo: %s between frames ms per frame %.2f: save %.2f hash %.2f load %.2f rerun %.2f wait %.2f sleep %.2f rest (ggpo) %.2f | sync=%d present_first=%d",
				what, ours, s_save_ms.sum / n, s_hash_ms.sum / n, s_load_ms.sum / n, s_rerun_ms.sum / n, s_wait_ms.sum / n, s_sleep_ms.sum / n,
				ours - split, s_sync, s_present_first);
			Console.WriteLn("ZdxsvGgpo: %s rerun frame split: vu1 ms %.2f, spu2 ms %.3f, ee Mcycles %.3f", what,
				Common::Timer::ConvertValueToMilliseconds(g_rerun_vu1_ticks) / std::max(s_rollback_frames, 1),
				Common::Timer::ConvertValueToMilliseconds(g_rerun_spu2_ticks) / std::max(s_rollback_frames, 1), s_rerun_mcycles.Mean());
			Console.WriteLn("ZdxsvGgpo: %s delta %s | %s", what, Zdxsv::DeltaStateTimes().c_str(), SaveState_DeltaTimes().c_str());
			Console.WriteLn("ZdxsvGgpo: %s output: presented frames %d | audio samples played %lld dropped (rerun) %lld",
				what, g_perfmon.GetFrame() - s_gs_frame0, static_cast<long long>(s_spu_played), static_cast<long long>(s_spu_dropped));
		}

		Input HostInput()
		{
			Input in = {};
			for (u32 i = 0; i < BUTTONS; i++)
				if (s_host[i] >= 0.5f)
					in.buttons |= static_cast<u16>(1u << i);
			const auto axis = [](float neg, float pos) {
				return static_cast<u8>(std::clamp(127.5f + (pos - neg) * 127.5f, 0.0f, 255.0f));
			};
			using I = PadDualshock2::Inputs;
			in.lx = axis(s_host[I::PAD_L_LEFT], s_host[I::PAD_L_RIGHT]);
			in.ly = axis(s_host[I::PAD_L_UP], s_host[I::PAD_L_DOWN]);
			in.rx = axis(s_host[I::PAD_R_LEFT], s_host[I::PAD_R_RIGHT]);
			in.ry = axis(s_host[I::PAD_R_UP], s_host[I::PAD_R_DOWN]);
			return in;
		}

		void ApplyInputs(const Input* inputs)
		{
			for (int p = 0; p < PLAYERS; p++)
				ApplyPad(p, inputs[p]);
		}

		bool SyncAndApply(bool rerun)
		{
			if (s_net)
				return NetSyncAndApply();
			Input inputs[PLAYERS] = {};
			int disconnect_flags = 0;
			const GGPOErrorCode rc = ggpo_synchronize_input(s_session, inputs, sizeof(inputs), &disconnect_flags);
			if (rc != GGPO_OK)
			{
				Console.Error("ZdxsvGgpo: synchronize_input %d", rc);
				return false;
			}
			if (rerun && s_control_input)
				inputs[0].buttons ^= static_cast<u16>(s_rng() | 1);
			ApplyInputs(inputs);
			return true;
		}

		// Local inputs of the next frame, then the synced inputs go to the pads.
		bool NextInputs()
		{
			if (s_net)
				return NetNextInputs();
			if (s_session_frames % 5 == 0)
			{
				for (Input& in : s_random)
				{
					in.buttons = static_cast<u16>(s_rng() & s_rng() & s_mask);
					if (s_no_input)
						in.buttons = 0;
					in.lx = static_cast<u8>(s_rng());
					in.ly = static_cast<u8>(s_rng());
					in.rx = in.ry = Pad::ANALOG_NEUTRAL_POSITION;
					if (s_no_input)
						in.lx = in.ly = Pad::ANALOG_NEUTRAL_POSITION;
				}
			}
			for (int p = 0; p < PLAYERS; p++)
			{
				Input in = (p == 0 && s_host_input) ? HostInput() : s_random[p];
				const GGPOErrorCode rc = ggpo_add_local_input(s_session, s_handles[p], &in, sizeof(in));
				if (rc != GGPO_OK)
				{
					Console.Error("ZdxsvGgpo: add_local_input %d", rc);
					return false;
				}
			}
			return SyncAndApply(false);
		}

		// Runs the CPU to the next vsync.
		bool RunFrame()
		{
			s_frame_ended = false;
			Cpu->Execute();
			if (!std::exchange(s_frame_ended, false))
			{
				Console.Error("ZdxsvGgpo: the CPU stopped before the end of a rerun frame");
				return false;
			}
			return true;
		}

		// per position: 0 connected, 1 interrupted, 2 disconnected (GGPO events, for the OSD)
		int s_peer_state[GGPO_MAX_PLAYERS] = {};
		void SetPeerState(GGPOPlayerHandle h, int state)
		{
			for (int p = 0; p < GGPO_MAX_PLAYERS; p++)
				if (s_handles[p] == h)
					s_peer_state[p] = state;
		}

		constexpr u32 OsdColor(u32 r, u32 g, u32 b) { return 0xff000000u | (b << 16) | (g << 8) | r; } // IM_COL32
		constexpr u32 OSD_TEXT = OsdColor(255, 255, 255);
		// flycast msColor
		u32 OsdPingColor(int ms)
		{
			return ms <= 0 ? OsdColor(64, 64, 64) : ms <= 30 ? OsdColor(87, 213, 213) : ms <= 60 ? OsdColor(0, 255, 149) :
			       ms <= 90 ? OsdColor(255, 255, 0) : ms <= 120 ? OsdColor(255, 170, 0) : OsdColor(255, 0, 0);
		}

		// net, once per frame: the OSD lines (GgpoOsdLines). log: also to the log, one line.
		void UpdateOsd(bool log)
		{
			if (!s_osd || !s_net || !s_session)
				return;
			std::vector<GgpoOsdLine> lines;
			// flycast's delay colors
			lines.push_back({fmt::format("Delay {}fr", s_delay), s_delay >= 13 ? OsdColor(255, 38, 31) : s_delay >= 10 ? OsdColor(255, 128, 0) :
			                                                      s_delay >= 5   ? OsdColor(255, 217, 0) : OSD_TEXT});
			lines.push_back({fmt::format("Roll {}  Wait {}", s_rollback_frames, s_waits), OSD_TEXT});
			for (int p = 0; p < s_players; p++)
			{
				if (p == s_net_me)
					continue;
				const LobbyPlayer who = p < static_cast<int>(s_net_players.size()) ? s_net_players[p] : LobbyPlayer{};
				lines.push_back({fmt::format("{}P {}", p + 1, who.id), OSD_TEXT});
				if (!who.name.empty())
					lines.push_back({" " + who.name, OSD_TEXT});
				if (!who.pilot.empty())
					lines.push_back({" " + who.pilot, OSD_TEXT});
				GGPONetworkStats st{};
				if (s_peer_state[p] == 2)
					lines.push_back({" Disconnected", OsdPingColor(999)});
				else if (ggpo_get_network_stats(s_session, s_handles[p], &st) == GGPO_OK)
				{
					// Interrupted in place of the ping, so the line fits the OSD's fixed width
					if (s_peer_state[p] == 1)
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
			std::lock_guard lock(s_osd_mtx);
			s_osd_lines = std::move(lines);
		}

		bool __cdecl BeginGame(const char*) { return true; }
		bool __cdecl OnEvent(GGPOEvent* ev)
		{
			switch (ev->code)
			{
				case GGPO_EVENTCODE_RUNNING:
					s_running = true;
					Console.WriteLn("ZdxsvGgpo: running");
					break;
				case GGPO_EVENTCODE_TIMESYNC:
					s_frames_ahead = ev->u.timesync.frames_ahead;
					break;
				case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
					s_disconnected = true;
					SetPeerState(ev->u.disconnected.player, 2);
					Console.Warning("ZdxsvGgpo: peer disconnected");
					break;
				case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
					SetPeerState(ev->u.connection_interrupted.player, 1);
					Console.Warning("ZdxsvGgpo: connection interrupted");
					break;
				case GGPO_EVENTCODE_CONNECTION_RESUMED:
					SetPeerState(ev->u.connection_resumed.player, 0);
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
			if (!s_save_all && !s_sync && ggpo_get_last_confirmed_frame(s_session, &confirmed) == GGPO_OK && frame <= confirmed)
				s_save_skipped++;
			else if (!Zdxsv::DeltaStateSave(frame))
				return false;
			if (s_net)
				NetSaved(frame);
			s_save_ms.Add(timer.GetTimeMilliseconds());
			timer.Reset();
			*checksum = 0;
			if (s_sync)
				HashSave(frame, checksum);
			s_hash_ms.Add(timer.GetTimeMilliseconds());
			if (!s_rerun)
				s_top_frame = frame;
			int* saved = new int(frame);
			*buffer = reinterpret_cast<unsigned char*>(saved);
			*len = sizeof(int);
			// GGPO never goes back further than its check distance / prediction window.
			Zdxsv::DeltaStateDiscardBefore(frame - std::max(s_check, 8) - 4);
			return true;
		}

		bool __cdecl LoadGameState(unsigned char* buffer, int len)
		{
			if (len != sizeof(int))
				return false;
			Common::Timer timer;
			const bool ok = Zdxsv::DeltaStateLoad(*reinterpret_cast<int*>(buffer));
			s_reruns_left = s_top_frame - *reinterpret_cast<int*>(buffer);
			if (s_net)
				NetLoaded(*reinterpret_cast<int*>(buffer));
			s_load_ms.Add(timer.GetTimeMilliseconds());
			s_loads++;
			return ok;
		}

		bool __cdecl LogGameState(char* filename, unsigned char* buffer, int len)
		{
			// Called by the synctest on a checksum mismatch, for the original and the rerun.
			if (std::strstr(filename, "original"))
			{
				if (s_mismatches++ < 20)
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
			if (!SyncAndApply(true))
				return false;
			g_ggpo_in_rollback = true;
			Common::Timer timer;
			EeProfileOnRerun();
			const u64 cycle0 = cpuRegs.cycle;
			if (!s_rerun_draw)
				MTGS::RunOnGSThread([]() { g_gs_skip_draws = true; });
			--s_reruns_left;
			g_rerun_vu1_skip = !s_rerun_vu1 && s_reruns_left >= s_rerun_tail - 1;
			g_rerun_draw_skip = !s_rerun_ee_draw && s_reruns_left >= s_rerun_tail;
			const bool ok = RunFrame();
			g_rerun_vu1_skip = g_rerun_draw_skip = false;
			s_rerun_mcycles.Add((cpuRegs.cycle - cycle0) / 1e6);
			if (!s_rerun_draw)
				MTGS::RunOnGSThread([]() { g_gs_skip_draws = false; });
			s_rerun_ms.Add(timer.GetTimeMilliseconds());
			g_ggpo_in_rollback = false;
			s_rerun = true;
			ggpo_advance_frame(s_session);
			s_rerun = false;
			s_rollback_frames++;
			return ok;
		}

		void OnLog(int level, const char* msg)
		{
			if (level > GGPO_LOG_WARNING)
				return;
			if (s_ggpo_warnings++ < 20)
				Console.Warning("ZdxsvGgpo: ggpo: %s", msg);
		}

		// net: a thread pumps GGPO's sockets every 1 ms (ggpo_idle -1: receive and send only, no callbacks), as
		// flycast's ggpoIdleLoop, so pings, input acks and relayed packets do not wait for the next vsync.
		// ZDXSV_NET_PUMP=0: no thread (GGPO polled only by the frame loop).
		// s_ggpo_mtx guards every GGPO call: the frame loop holds it through Returned, the thread per pump.
		std::recursive_mutex s_ggpo_mtx;
		std::atomic<bool> s_pump_run{false};
		struct PumpThread
		{
			std::thread t;
			void Stop()
			{
				s_pump_run = false;
				if (t.joinable())
					t.join();
			}
			~PumpThread() { Stop(); }
		} s_pump;

		void PumpStart()
		{
			const char* e = Zdxsv::TestEnv("ZDXSV_NET_PUMP");
			if (e && std::strcmp(e, "0") == 0)
				return;
			s_pump.Stop();
			s_pump_run = true;
			s_pump.t = std::thread([] {
				Threading::SetNameOfCurrentThread("ZdxsvGgpoPump");
				while (s_pump_run)
				{
					// try_lock: Stop joins this thread while it holds the lock
					if (std::unique_lock lock(s_ggpo_mtx, std::try_to_lock); lock.owns_lock() && s_session)
						ggpo_idle(s_session, -1);
					Threading::Sleep(1);
				}
			});
			Console.WriteLn("ZdxsvGgpo: pump thread started");
		}

		void Stop(const char* what)
		{
			s_pump.Stop();
			std::lock_guard ggpo_lock(s_ggpo_mtx);
			if (s_session)
				ggpo_close_session(s_session);
			s_session = nullptr;
			ggpo_set_log_function(nullptr);
			g_ggpo_active = false;
			if (!s_vm_closing)
				Host::RunOnCPUThread([] { SyncSettingsOnBattle(false); });
			Zdxsv::DeltaStateClear();
			Report(what);
			{
				std::lock_guard lock(s_osd_mtx);
				s_osd_lines.clear();
			}
			if (s_lobby)
			{
				std::lock_guard lock(s_lobby_mtx);
				if (!s_report.empty())
					s_report += std::string("close=") + what + "\nframes=" + std::to_string(s_session_frames) +
					            "\nrollback_frames=" + std::to_string(s_rollback_frames) + "\nmismatches=" + std::to_string(s_mismatches) +
					            "\ndisconnected=" + (s_disconnected ? "1" : "0") + "\n";
			}
			if (s_net)
			{
				s_net_over = true; // the battle sock goes back to the IOP
				ReplayWrite(what);
				NetReport();
			}
			if (s_rbk && !s_play_env && !s_vm_closing)
			{
				Console.WriteLn("ZdxsvGgpo: rbk exit (%s) at vsync %u", what, g_FrameCount);
				Host::RunOnCPUThread([] { Host::RequestVMShutdown(false, false, false); });
			}
		}

		bool Start()
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
			if (s_net)
				return NetStart(cb);
			if (ggpo_start_synctest(&s_session, &cb, "zdxsv", PLAYERS, sizeof(Input), s_check) != GGPO_OK)
			{
				s_session = nullptr;
				return false;
			}
			ggpo_idle(s_session, 0);
			for (int p = 0; p < PLAYERS; p++)
			{
				GGPOPlayer player{sizeof(GGPOPlayer), GGPO_PLAYERTYPE_LOCAL, p + 1};
				if (ggpo_add_player(s_session, &player, &s_handles[p]) != GGPO_OK)
					return false;
			}
			s_rng.seed(s_seed);
			Console.WriteLn("ZdxsvGgpo: synctest start=%d frames=%d check=%d seed=%u input=%s mask=%04x control=%d hash=%s",
				s_start, s_frames, s_check, s_seed, s_host_input ? "host" : s_no_input ? "none" : "random", s_mask, s_control_input,
				s_hash == Hash::Full ? "full" : s_hash == Hash::Pos ? "pos" : "pw");
			return true;
		}

		void NetLoaded(int frame)
		{
			s_net_frame = frame;
			s_rb = s_net_at[frame & 127];
		}

		bool NetStart(GGPOSessionCallbacks& cb)
		{
			if (s_net_me < 0 || s_net_me >= s_players)
			{
				Console.Error("ZdxsvGgpo: net position %d not below players=%d", s_net_me, s_players);
				return false;
			}
			const int local_port = s_lobby ? s_port : s_port + s_net_me;
			if (s_net_trace) // battle start marker (pwcheck.py --battle): BATTLES=N lobby battles in one process
			{
				const std::string code = IdValue(ReplayIds(), "battle_code");
				std::fprintf(s_net_trace, "%u B %s\n", g_FrameCount, code.empty() ? "-" : code.c_str());
			}
			if (ggpo_start_session(&s_session, &cb, "zdxsv", s_players, sizeof(NetInput), static_cast<unsigned short>(local_port), nullptr, 0) != GGPO_OK)
			{
				s_session = nullptr;
				return false;
			}
			// ZDXSV_NET_DISCONNECT_MS=ms (default 5000): longer lets a peer stall (ZDXSV_RAM_DUMP) without a disconnect.
			const char* dms = Zdxsv::TestEnv("ZDXSV_NET_DISCONNECT_MS");
			ggpo_set_disconnect_timeout(s_session, dms ? std::atoi(dms) : 5000);
			ggpo_set_disconnect_notify_start(s_session, 1000);
			// relay servers before the players, in the battle info's order on every peer (ggpo_add_relay_server)
			if (s_lobby)
				for (const Zdxsv::RelayServerAddr& r : s_net_servers)
				{
					if (ggpo_add_relay_server(s_session, r.addr.ip.c_str(), r.addr.port, r.alt.ip.empty() ? nullptr : r.alt.ip.c_str()) != GGPO_OK)
						return false;
					Console.WriteLn("ZdxsvGgpo: relay server %s%s%s", r.addr.String().c_str(), r.alt.ip.empty() ? "" : " alt ",
						r.alt.ip.empty() ? "" : r.alt.String().c_str());
				}
			for (int p = 0; p < s_players; p++)
			{
				GGPOPlayer player{};
				player.size = sizeof(GGPOPlayer);
				player.player_num = p + 1;
				player.type = (p == s_net_me) ? GGPO_PLAYERTYPE_LOCAL : GGPO_PLAYERTYPE_REMOTE;
				if (player.type == GGPO_PLAYERTYPE_REMOTE && s_lobby)
				{
					const Zdxsv::PeerAddr& a = s_net_peers[p];
					StringUtil::Strlcpy(player.u.remote.ip_address, a.ip.c_str(), sizeof(player.u.remote.ip_address));
					player.u.remote.port = a.port;
					player.u.remote.relay = p < static_cast<int>(s_net_via.size()) && s_net_via[p] != 0;
					Console.WriteLn("ZdxsvGgpo: lobby peer position %d at %s%s", p, a.String().c_str(), player.u.remote.relay ? " (relay)" : "");
				}
				else if (player.type == GGPO_PLAYERTYPE_REMOTE)
				{
					StringUtil::Strlcpy(player.u.remote.ip_address, s_peer_host.c_str(), sizeof(player.u.remote.ip_address));
					player.u.remote.port = static_cast<unsigned short>(s_relay ? s_relay + 8 * s_net_me + p : s_port + p);
				}
				if (ggpo_add_player(s_session, &player, &s_handles[p]) != GGPO_OK)
					return false;
				if (player.type == GGPO_PLAYERTYPE_LOCAL)
					ggpo_set_frame_delay(s_session, s_handles[p], s_delay);
			}
			// Every peer arms at its own first key msg: block until all are synchronized.
			Common::Timer wait;
			while (!s_running && wait.GetTimeSeconds() < 60)
			{
				ggpo_idle(s_session, 0);
				Threading::Sleep(1);
			}
			if (!s_running)
			{
				Console.Error("ZdxsvGgpo: net peers not synchronized in 60 s");
				return false;
			}
			Console.WriteLn("ZdxsvGgpo: net player %d of %d port %d delay %d, %zu msgs sent before the start, waited %.1f s",
				s_net_me + 1, s_players, local_port, s_delay, s_net_sent.size(), wait.GetTimeSeconds());
			PumpStart();
			return true;
		}

		bool NetNextInputs()
		{
			NetInput in = NetPack(s_rand_env ? RandInput() : HostInput());
			GGPOErrorCode rc = ggpo_add_local_input(s_session, s_handles[s_net_me], &in, sizeof(in));
			if (rc == GGPO_ERRORCODE_PREDICTION_THRESHOLD)
			{
				s_waits++;
				Common::Timer wait;
				while (rc == GGPO_ERRORCODE_PREDICTION_THRESHOLD && !s_disconnected && wait.GetTimeSeconds() < 10)
				{
					ggpo_idle(s_session, 0);
					Threading::Sleep(1);
					rc = ggpo_add_local_input(s_session, s_handles[s_net_me], &in, sizeof(in));
				}
				s_wait_ms.Add(wait.GetTimeMilliseconds());
			}
			if (rc != GGPO_OK)
			{
				Console.Error("ZdxsvGgpo: net add_local_input %d", rc);
				return false;
			}
			return NetSyncAndApply();
		}

		// Inputs of frame s_net_frame: own host pad to pad 0, every position's (A, B) to s_rings.zd_hist, kind-3
		// msgs with a new seq to the barrier (the n-th of each remote to recv once all peers' n-th arrived).
		bool NetSyncAndApply()
		{
			NetInput in[GGPO_MAX_PLAYERS] = {};
			int disconnect_flags = 0;
			const GGPOErrorCode rc = ggpo_synchronize_input(s_session, in, sizeof(NetInput) * s_players, &disconnect_flags);
			if (rc != GGPO_OK)
			{
				Console.Error("ZdxsvGgpo: net synchronize_input %d", rc);
				return false;
			}
			NetApply(in);
			return true;
		}
	} // namespace

	// ZDXSV_REPLAY=file.pb: plays a saved replay (PlayLoad), the net=1 hooks on, no GGPO session. Else the replay
	// list's pick (s_play_picked) for one VM. Set by GgpoOnVmInitialize.
	const char* s_play_env = nullptr;
	std::string s_play_picked;
	int s_play_picked_pov = -1;
	// Common start: the replay has no start state (or ZDXSV_REPLAY_COMMON=1). The booted state (any post-entry
	// state, tests/zdxsv/rbkprep.sh) plays the battle start with the file's lobby answers through RbkCall up to the
	// arm, which is GGPO frame 0 (PlayCommonStart).
	bool s_play_common = false;
	bool g_ggpo_enabled = false; // GgpoOnVmInitialize
	bool g_mtvu_off = false; // GgpoOnVmInitialize, cleared at VM shutdown
	s32 g_frame_period_trim_us = 0;
	// The ZDXSV_GGPO options of this VM: the variable, else DEFAULT_OPTIONS for the Z game with the
	// ZdxsvGgpo setting on, else empty = off. Set by GgpoOnVmInitialize before the CPU runs.
	std::string s_options;
	bool g_z_game = false;
	bool g_ggpo_active = false;
	bool g_ggpo_in_rollback = false;
	u64 g_rerun_vu1_ticks = 0;
	bool g_rerun_vu1_skip = false;
	bool g_rerun_draw_skip = false;
	u64 g_rerun_spu2_ticks = 0;
	bool g_gs_rerun_frame = false;
	bool g_gs_skip_draws = false;
	bool s_net_env = false; // net=1 in s_options, or a replay plays (GgpoOnVmInitialize)
	// ZDXSV_RBK=i/N: until GGPO arms, every lobby / battle connect RPC is answered here (RbkCall).
	int s_rbk_me = -1, s_rbk_n = 0;
	bool s_rbk = [] { // also set by a replay's common start (PlayLoad)
		const char* e = Zdxsv::TestEnv("ZDXSV_RBK");
		return e && std::sscanf(e, "%d/%d", &s_rbk_me, &s_rbk_n) == 2 && s_rbk_me >= 0 && s_rbk_me < s_rbk_n && s_rbk_n <= 4;
	}();
	const char* s_rand_env = Zdxsv::TestEnv("ZDXSV_RAND_INPUT");
	bool s_net = false; // net=1 parsed
	int s_players = 4; // players= (net)
	u32 s_zds_echo = 0, s_zds_skip = 0;
	// zds: kind 3 (round handshake) is the one barrier: each machine reaches it at its own frame
	// (scene/load timing: one side can be a frame later), so it goes through the GGPO input
	// and the n-th kind 3 of every remote goes to recv once all peers' n-th is in the synced stream.
	std::vector<std::vector<u8>> s_zds_k3[GGPO_MAX_PLAYERS]; // per sender, by index
	u32 s_zds_k3rel = 0;
	u32 s_zd_steps = 0, s_zd_changed = 0;
	bool s_net_armed = false, s_net_over = false;
	bool s_lobby_cut = false; // lobby=1 ping test failed: the battle connection is silent (LobbyCutCall)
	u32 s_cut_sends = 0;
	int s_net_me = -1; // local battle position
	std::vector<std::vector<u8>> s_net_sent; // every msg the game sent since armed, in order
	// ZDXSV_K3_LAG: a msg sent at GGPO frame s goes into the local input of frame s + lag. With lag above
	// GGPO's 6 prediction frames, frame s is final when s + lag is added, so a send first made in a rerun
	// still lands on a fixed frame.
	const int s_k3_lag = [] {
		const char* e = Zdxsv::TestEnv("ZDXSV_K3_LAG");
		return e ? std::max(0, std::atoi(e)) : 8;
	}();
	std::deque<NetOut> s_net_out; // committed, not yet in a local input
	std::vector<int> s_net_sent_at; // GGPO frame of each s_net_sent entry (latest timeline)
	NetInput s_net_local = {};
	int s_net_frame = 0; // GGPO frame being run (last save or load)
	RollbackState s_rb;
	FrameRings s_rings;
	int s_net_end = -1; // frames since the end msg (kind f) was sent or received, -1 = not yet
	NetStats s_ns = {};
	Hash s_hash = Hash::Pw; // hash= (synctest checksum)
	int s_port = 7001, s_delay = 0;
	bool s_delay_set = false; // delay= given: fixed; else a lobby battle picks it from the peers' rtt
	int s_min_delay = 2; // mindelay=, else setting DEV9/Eth ZdxsvGgpoMinDelay
	bool s_lobby = false; // lobby=1
	bool s_bad_session = false; // badsession=1 (test): the ping test uses another session id
	std::string s_replay_dir; // replay=DIR; without it lobby=1 saves to <data dir>/replays, other net runs none
	bool s_replay_off = false; // replay=0
	std::string s_upload_url; // upload=URL (test), else setting DEV9/Eth ZdxsvReplayUploadUrl
	// network status OSD: user id, name + pilot name per position (lobby battle info), lines of the last frame
	std::vector<LobbyPlayer> s_lobby_players, s_net_players;

	GGPOSession* s_session = nullptr;
	bool s_started = false; // the session was opened (lobby=1: until the next battle's NetReset)
	bool s_frame_ended = false; // the CPU left Execute() at a vsync
	int s_session_frames = 0;
	std::array<float, PadDualshock2::Inputs::LENGTH> s_host = {};

	// The host pad in a replay takeover: START is the takeover's retry / skip key, not game input.
	Input TakeoverHostInput()
	{
		Input in = HostInput();
		in.buttons &= static_cast<u16>(~(1u << PadDualshock2::Inputs::PAD_START));
		return in;
	}

	void ApplyPad(int p, const Input& in)
	{
		PadBase* pad = Pad::GetPad(static_cast<u8>(p));
		if (!pad)
			return;
		for (u32 i = 0; i < BUTTONS; i++)
		{
			const bool held = (in.buttons >> i) & 1;
			pad->SetRawPressureButton(i, std::make_tuple(held, static_cast<u8>(held ? 255 : 0)));
		}
		pad->SetRawAnalogs({in.lx, in.ly}, {in.rx, in.ry});
	}

	// lobby=1: the next lobby battle in this process starts from the state the first one started from
	// (Stop closed the session; its stats are reported).
	void NetReset()
	{
		s_started = s_net_armed = s_net_over = s_lobby_cut = false;
		s_frame_ended = s_running = s_disconnected = s_rerun = false;
		s_net_me = -1;
		s_cut_sends = 0;
		s_net_sent.clear();
		s_net_sent_at.clear();
		s_rb = {};
		s_net_out.clear();
		s_net_local = {};
		s_net_frame = 0;
		std::fill(std::begin(s_net_at), std::end(s_net_at), RollbackState{});
		s_net_end = -1;
		s_ns = {};
		s_rings = {};
		s_zd_steps = s_zd_changed = s_zds_echo = s_zds_skip = s_zds_k3rel = 0;
		for (auto& k3 : s_zds_k3)
			k3.clear();
		s_pw.clear();
		std::fill(std::begin(s_peer_state), std::end(s_peer_state), 0);
		std::fill(std::begin(s_handles), std::end(s_handles), GGPOPlayerHandle{});
		s_frames_ahead = s_waits = s_save_skipped = s_session_frames = s_top_frame = s_reruns_left = 0;
		s_rollback_frames = s_loads = s_mismatches = s_ggpo_warnings = s_diff_logged = 0;
		s_save_ms = s_hash_ms = s_load_ms = s_rerun_ms = s_wait_ms = s_sleep_ms = s_emu_ms = s_exit_ms = s_ours_ms = {};
		s_throttle_deferred = false;
		s_rerun_mcycles = {};
		g_rerun_vu1_ticks = g_rerun_spu2_ticks = 0;
		s_t_vsync = s_t_returned = 0;
		s_spu_played = s_spu_dropped = 0;
	}

	void GgpoOnVsync()
	{
		if (s_rbk && s_net_env && !s_net_armed)
			VMManager::SetLimiterMode(LimiterModeType::Turbo);
		// Once: the rbk knobs as received; zdxsv/rbk.sh compares this line to what it meant to pass
		static bool rbk_env_logged = false;
		if (s_rbk && !s_play_env && !rbk_env_logged)
		{
			rbk_env_logged = true;
			const auto env = [](const char* k) { const char* v = std::getenv(k); return v && *v ? v : "-"; };
			Console.WriteLn("ZdxsvGgpo: rbk env pos=%d/%d rand=%s turbo=%s clamp=%s ggpo=%s", s_rbk_me, s_rbk_n,
				env("ZDXSV_RAND_INPUT"), env("ZDXSV_RBK_TURBO"), env("ZDXSV_EE_CLAMP"), env("ZDXSV_GGPO"));
		}
		// ZDXSV_VM_TEST=frame:shutdown|reset (tests of GgpoOnVmShutdown): once, when a session or replay reaches GGPO frame `frame`
		static const char* vm_test = std::getenv("ZDXSV_VM_TEST");
		if (vm_test && g_ggpo_active && !g_ggpo_in_rollback && s_net_frame == std::atoi(vm_test))
		{
			vm_test = nullptr;
			if (std::strstr(std::getenv("ZDXSV_VM_TEST"), ":reset"))
				Host::RunOnCPUThread([] { VMManager::Reset(); });
			else
				Host::RunOnCPUThread([] { Host::RequestVMShutdown(false, false, false); });
			Console.WriteLn("ZdxsvGgpo: vm test %s at frame %d", std::getenv("ZDXSV_VM_TEST"), s_net_frame);
		}
		TracePad();
		TraceInputs();
		// ZDXSV_SNAP=dir,n[,from]: GS screenshot dir/v<vsync>.png every n vsyncs from vsync `from` (needs a
		// real renderer, not -Headless)
		static const char* snap = Zdxsv::TestEnv("ZDXSV_SNAP");
		static const char* snap_c = snap ? std::strchr(snap, ',') : nullptr;
		static const int snap_n = snap_c ? std::atoi(snap_c + 1) : 0;
		static const u32 snap_from = snap_c && std::strchr(snap_c + 1, ',') ? std::atoi(std::strchr(snap_c + 1, ',') + 1) : 0;
		if (snap_n > 0 && !g_ggpo_in_rollback && g_FrameCount >= snap_from && g_FrameCount % snap_n == 0)
			GSQueueSnapshot(fmt::format("{}\\v{}.png", std::string(snap, std::strchr(snap, ',')), g_FrameCount));
		if (!g_ggpo_enabled)
			return;
		if (!g_ggpo_active)
		{
			if (!g_ggpo_enabled || s_started)
				return;
			if (s_vsyncs++ == 0)
			{
				Parse();
				if (s_play_env)
					Host::RunOnCPUThread(PlayLoad);
			}
			if (s_play_env && !(s_play_common && s_net_armed)) // PlayLoad starts it, or the arm of a common start
				return;
			if (s_net ? !s_net_armed : s_vsyncs < s_start)
				return;
			s_started = true;
			g_ggpo_active = true;
			s_gs_frame0 = g_perfmon.GetFrame();
		}
		s_frame_ended = true;
		if (!g_ggpo_in_rollback)
		{
			s_t_vsync = Common::Timer::GetCurrentValue();
			if (s_t_returned)
				s_emu_ms.Add(Common::Timer::ConvertValueToMilliseconds(s_t_vsync - s_t_returned));
		}
		Cpu->ExitExecution();
	}

	bool GgpoDeferThrottle()
	{
		s_throttle_deferred = s_present_first && g_ggpo_active && s_session && !s_play_env && s_frame_ended;
		return s_throttle_deferred;
	}

	// The limiter sleep and input poll that VSyncStart left for after the rollback.
	static void DeferredThrottle()
	{
		if (!std::exchange(s_throttle_deferred, false))
			return;
		Common::Timer timer;
		if (!VMManager::Internal::IsExecutionInterrupted())
			VMManager::Internal::Throttle();
		VMManager::Internal::PollInputOnCPUThread();
		s_sleep_ms.Add(timer.GetTimeMilliseconds());
	}

	static void Returned();

	bool SpuOnOutput()
	{
		(g_ggpo_in_rollback ? s_spu_dropped : s_spu_played)++;
		return g_ggpo_in_rollback;
	}

	void GgpoOnExecuteReturned()
	{
		if (!g_ggpo_active || !std::exchange(s_frame_ended, false))
			return;
		const Common::Timer::Value t0 = Common::Timer::GetCurrentValue();
		if (s_t_vsync)
			s_exit_ms.Add(Common::Timer::ConvertValueToMilliseconds(t0 - s_t_vsync));
		SyncSettingsOnBattle(true); // before the session's first frame
		Returned();
		DeferredThrottle(); // a Returned that ended the session
		s_t_returned = Common::Timer::GetCurrentValue();
		s_ours_ms.Add(Common::Timer::ConvertValueToMilliseconds(s_t_returned - t0));
	}

	static void Returned()
	{
		std::lock_guard ggpo_lock(s_ggpo_mtx);
		if (s_play_env)
		{
			s_play_common ? PlayCommonStart() : PlayNext();
			return;
		}
		if (!s_session)
		{
			if (!Start())
			{
				Stop("start failed");
				return;
			}
			if (s_net)
				ReplayBegin();
			// The synctest saves frame 0 at the first synchronize_input.
			if (!NextInputs())
				Stop("failed");
			return;
		}
		if (ggpo_advance_frame(s_session) != GGPO_OK || ggpo_idle(s_session, 0) != GGPO_OK)
		{
			Stop("failed");
			return;
		}
		s_session_frames++;
		// net: the battle ended (end msg sent or received); s_net_tail frames for the peers to get it too.
		if (s_net && s_net_end >= 0 && ++s_net_end > s_net_tail)
		{
			Stop("net battle end");
			return;
		}
		if (s_disconnected)
		{
			Stop("disconnected");
			return;
		}
		// net: ahead of the peer, give it time to catch up (GGPO's suggested frames, at 60 fps).
		if (const int ahead = std::exchange(s_frames_ahead, 0); ahead > 0)
		{
			Common::Timer wait;
			while (wait.GetTimeMilliseconds() < ahead * 1000.0 / 60.0)
			{
				ggpo_idle(s_session, 0);
				Threading::Sleep(1);
			}
			s_wait_ms.Add(wait.GetTimeMilliseconds());
		}
		if (s_session_frames >= s_frames)
		{
			Stop("done");
			return;
		}
		if (s_session_frames % 600 == 0)
			Report("progress");
		UpdateOsd(s_session_frames % 600 == 0);
		DeferredThrottle();
		if (!s_session) // the input poll ran a VM shutdown or reset
			return;
		if (!NextInputs())
			Stop("failed");
	}

	bool GgpoCaptureHostInput(u32 controller, u32 bind, float value)
	{
		if (!g_ggpo_active)
			return false;
		if (controller == 0 && bind < s_host.size())
			s_host[bind] = value;
		return controller < PLAYERS;
	}

	bool g_net_hook = false; // GgpoOnVmInitialize
	bool g_zd_hook = false;
	bool g_ps_hook = true;

	void GgpoOnVmInitialize(const char* serial, u32 crc)
	{
		TakeNextReplay(s_play_picked, s_play_picked_pov);
		const char* play = std::getenv("ZDXSV_REPLAY");
		s_play_env = play ? play : s_play_picked.empty() ? nullptr : s_play_picked.c_str();
		const char* e = std::getenv("ZDXSV_GGPO");
		// Read here only, so not a config field: a change takes effect at the next VM start.
		const bool setting = Host::GetBoolSettingValue("DEV9/Eth", "ZdxsvGgpo", true);
		// ZDXSV_GAME_CRC=hex (test): the CRC taken as the Z game's, to run the wrong-game path on the Z game
		const char* want_env = Zdxsv::TestEnv("ZDXSV_GAME_CRC");
		const u32 want = want_env ? static_cast<u32>(std::strtoul(want_env, nullptr, 16)) : GAME_CRC;
		const bool serial_match = std::strcmp(serial, GAME_SERIAL) == 0;
		g_z_game = serial_match && crc == want;
		const bool lobby_default = g_z_game && setting && !s_play_env;
		if (!g_z_game) // any other game or build: no GGPO, replay, hooks or platform info
		{
			s_options.clear();
			g_ggpo_enabled = s_net_env = g_net_hook = g_zd_hook = g_ps_hook = false;
			if (serial_match || (e && std::strcmp(e, "0") != 0) || s_play_env || s_net_trace)
				Console.Warning("ZdxsvGgpo: off: not the Z game (serial %s CRC %08X, need %s %08X)", serial, crc, GAME_SERIAL, want);
			return;
		}
		SyncSettingsEnforce(); // LoadSettings ran before the game was known
		if (e && std::strcmp(e, "0") == 0) // off whatever the setting (rigs without GGPO)
			s_options.clear();
		else
			s_options = e ? e : lobby_default ? DEFAULT_OPTIONS : "";
		// as flycast's gdxsv:MinDelay (2..6); mindelay= overrides it
		s_min_delay = std::clamp(Host::GetIntSettingValue("DEV9/Eth", "ZdxsvGgpoMinDelay", 2), 2, 6);
		g_ggpo_enabled = !s_options.empty() || s_play_env;
		s_net_env = s_options.find("net=1") != std::string::npos || s_play_env;
		g_net_hook = s_net_trace != nullptr || s_net_env;
		g_zd_hook = s_net_env;
		g_ps_hook = true;
		if (!s_options.empty() || std::strcmp(serial, GAME_SERIAL) == 0)
			Console.WriteLn("ZdxsvGgpo: options '%s' (serial %s, setting %d)", s_options.c_str(), serial, setting ? 1 : 0);
		// Delta saves and loads, replay keys: VU1 memory is copied while the MTVU thread may still run on it.
		// The settings were loaded before this; VMManager::LoadCoreSettings keeps it off on later reloads.
		g_mtvu_off = g_ggpo_enabled || g_delta_state_test_enabled;
		if (g_mtvu_off && EmuConfig.Speedhacks.vuThread)
		{
			EmuConfig.Speedhacks.vuThread = false;
			Console.WriteLn("ZdxsvGgpo: MTVU speedhack off for this VM");
		}
	}

	// The game's input record (A, B) of a host pad: its button bind table (OR-linear; B bit 0 = game state, left 0).
	// A stick counts as a direction at axis <= 0x40 / >= 0xc0: the left stick sets the D-pad's B bits, the right
	// stick the same bits in A.
	void ZdPadAB(const Input& in, u16& a, u16& b)
	{
		using I = PadDualshock2::Inputs;
		static constexpr struct { I i; u16 a, b; } bind[] = {
			{I::PAD_L3, 0, 0x0002}, {I::PAD_R2, 0x0180, 0x0004}, {I::PAD_L2, 0x0280, 0x0008},
			{I::PAD_R1, 0x0300, 0x0010}, {I::PAD_CIRCLE, 0x0040, 0x0020}, {I::PAD_CROSS, 0x0080, 0x0040},
			{I::PAD_L1, 0x0020, 0x0080}, {I::PAD_TRIANGLE, 0x0100, 0x0100}, {I::PAD_SQUARE, 0x0200, 0x0200},
			{I::PAD_RIGHT, 0, 0x0400}, {I::PAD_LEFT, 0, 0x0800}, {I::PAD_DOWN, 0, 0x1000},
			{I::PAD_UP, 0, 0x2000}, {I::PAD_SELECT, 0, 0x4000}, {I::PAD_START, 0x8000, 0x8000}};
		a = b = 0;
		for (const auto& e : bind)
			if ((in.buttons >> e.i) & 1)
				a |= e.a, b |= e.b;
		const auto dirs = [](u8 x, u8 y) {
			return static_cast<u16>((x >= 0xc0 ? 0x0400 : 0) | (x <= 0x40 ? 0x0800 : 0) | (y >= 0xc0 ? 0x1000 : 0) | (y <= 0x40 ? 0x2000 : 0));
		};
		b |= dirs(in.lx, in.ly);
		a |= dirs(in.rx, in.ry);
	}

	bool s_ms_on = true;

	// A load step's pass: count the wish, then hold until NetApply sets go. true = held: v0 = 0 (step not done,
	// the scene retries it next frame), pc = the step's epilogue. Trace `<tag>H frame n`.
	static bool BarrierHold(PS& b, char tag, u32 epilogue)
	{
		if (!g_ggpo_active || !s_net_armed || s_net_over)
			return false;
		if (!b.hold)
		{
			b.hold = true;
			b.n++;
			if (s_net_trace)
				std::fprintf(s_net_trace, "%u %cH%s %d %d\n", g_FrameCount, tag, g_ggpo_in_rollback ? "r" : "", s_net_frame, b.n);
		}
		if (b.go)
		{
			b.hold = b.go = false;
			return false;
		}
		cpuRegs.GPR.n.v0.UD[0] = 0;
		cpuRegs.pc = epilogue;
		return true;
	}

	// ps: rec hook at LOAD_STEP_PC (0x2b1d80, battle load step past its load-busy check), epilogue 0x2b1fa8.
	bool OnLoadStep()
	{
		return BarrierHold(s_rb.ps, 'P', 0x2b1fa8);
	}

	// ms: rec hook at MS_STEP_PC (0x2b8698, MS-select load step past its load-busy check), epilogue 0x2b8ab8.
	bool OnDrawRun()
	{
		if (!g_rerun_draw_skip)
			return false;
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
		return true;
	}

	bool OnMsStep()
	{
		return s_ms_on && BarrierHold(s_rb.ms, 'M', 0x2b8ab8);
	}

	// zd: rec hook at the lockstep step's ring read (0x312bf4, per active position: s0 = position,
	// a1 = ring entry: +2 A, +6 B as u16; bit 0 of B is game-wide state, kept). Trace `Z frame p slot A B`.
	void OnStepCopy()
	{
		if (!g_ggpo_active || !s_net_armed)
			return;
		const int p = static_cast<s8>(cpuRegs.GPR.n.s0.UL[0]);
		const u32 e = cpuRegs.GPR.n.a1.UL[0] & 0x1ffffff;
		if (p < 0 || p >= s_players || e + 8 > Ps2MemSize::MainRam)
			return;
		u8* ram = eeMem->Main;
		u16 a, b;
		std::memcpy(&a, ram + e + 2, 2);
		std::memcpy(&b, ram + e + 6, 2);
		const int c = ram[e] & 63;
		const int k = s_net_frame;
		const u16* ab = s_rings.zd_hist[k & 127][p];
		const u16 na = ab[0];
		const u16 nb = static_cast<u16>((ab[1] & ~1u) | (b & 1u));
		s_zd_steps++;
		if (a != na || b != nb)
			s_zd_changed++;
		std::memcpy(ram + e + 2, &na, 2);
		std::memcpy(ram + e + 6, &nb, 2);
		if (s_net_trace)
			std::fprintf(s_net_trace, "%u Z%s %d %d %02x %04x %04x %04x %04x %02x %d\n", g_FrameCount, g_ggpo_in_rollback ? "r" : "",
				s_net_frame, p, ((e - 0xc61f40) / 8) & 63, na, nb, a, b, c, k);
	}

	void NetSaved(int frame)
	{
		s_net_frame = frame;
		s_net_at[frame & 127] = s_rb;
		if (s_pw_hash)
		{
			std::array<u64, 5>& h = s_pw[frame];
			for (u32 p = 0; p < 4; p++)
				h[p] = XXH3_64bits(&eeMem->Main[PW_BASE + PW_SIZE * p + PW_POS], 12);
			u16 a, b;
			std::memcpy(&a, &eeMem->Main[RNG_A], 2);
			std::memcpy(&b, &eeMem->Main[RNG_B], 2);
			h[4] = (static_cast<u64>(a) << 16) | b;
		}
		if (s_pw_dump)
		{
			const s32 f = frame;
			std::fwrite(&f, sizeof(f), 1, s_pw_dump);
			std::fwrite(&eeMem->Main[PW_BASE], PW_SIZE * 4, 1, s_pw_dump);
		}
	}

	void NetReport()
	{
		Console.WriteLn("ZdxsvGgpo: net sends %u msgs %u (sent %zu, unsent %zu) recvs %u rxmsgs %u rxbytes %u polls %u other %u nowait %u senddiff %u toolong %u maxq %u waits %d k3lag %d restamp %u late %u",
			s_ns.sends, s_ns.msgs, s_net_sent.size(), s_net_out.size(), s_ns.recvs, s_ns.rxmsgs, s_ns.rxbytes, s_ns.polls,
			s_ns.other, s_ns.nowait, s_ns.senddiff, s_ns.toolong, s_ns.maxq, s_waits, s_k3_lag, s_ns.restamp, s_ns.late);
		Console.WriteLn("ZdxsvGgpo: zd steps %u changed %u echo %u skip %u k3 %d/%d/%d/%d rel %d (fwd %u)", s_zd_steps, s_zd_changed, s_zds_echo, s_zds_skip,
			s_rb.zds_seen[0], s_rb.zds_seen[1], s_rb.zds_seen[2], s_rb.zds_seen[3], s_rb.zds_rel, s_zds_k3rel);
		if (s_pw_hash && s_net_trace)
		{
			for (const auto& [f, h] : s_pw)
				std::fprintf(s_net_trace, "0 H %d %016llx %016llx %016llx %016llx %08llx\n", f, static_cast<unsigned long long>(h[0]),
					static_cast<unsigned long long>(h[1]), static_cast<unsigned long long>(h[2]), static_cast<unsigned long long>(h[3]),
					static_cast<unsigned long long>(h[4]));
			std::fflush(s_net_trace);
			Console.WriteLn("ZdxsvGgpo: pw hashes %zu frames", s_pw.size());
			s_pw.clear();
		}
		if (s_pw_dump)
			std::fflush(s_pw_dump);
	}

	std::vector<GgpoOsdLine> GgpoOsdLines()
	{
		std::lock_guard lock(s_osd_mtx);
		return s_osd_lines;
	}
	// EE probe (iR5900.cpp): GGPO frame being run, the frame numbers of NET_TRACE H lines and PW dumps.
	int ProbeFrame() { return s_net_frame; }

	void GgpoOnVmShutdown(const char* what)
	{
		if (std::strcmp(what, "vm shutdown") == 0)
			g_mtvu_off = false; // the next VM decides again (a reset VM keeps it off)
		SyncSettingsReset();
		if (!g_ggpo_enabled)
			return;
		size_t keys = 0;
		for (const auto& k : s_play_keys)
			keys += k.size();
		Console.WriteLn("ZdxsvGgpo: %s: session %d, replay recording %d, replay keys %zu", what, g_ggpo_active && !s_play_env ? 1 : 0,
			s_replay_rec ? 1 : 0, keys);
		if (s_play_env)
		{
			g_ggpo_active = false;
			PlayReset();
		}
		else if (g_ggpo_active)
		{
			s_vm_closing = true;
			Stop(what); // peers see a disconnect; the replay is saved up to the last confirmed frame
			s_vm_closing = false;
		}
		Zdxsv::DeltaStateClear();
		NetReset();
		RbkReset();
		g_ggpo_in_rollback = false;
		s_vsyncs = 0; // the next VM start or the reset VM parses the options again (and loads a replay again)
	}
} // namespace Zdxsv

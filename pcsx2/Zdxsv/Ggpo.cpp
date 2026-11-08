// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// The GGPO session: options, frame loop, VM hooks.
// ZDXSV_GGPO="key=value,..." (docs/zdxsv/options.md) replaces DEFAULT_OPTIONS (GGPO on by default).
// Without net=1 the session is a synctest; log lines start with "ZdxsvGgpo".

#include "Zdxsv/GgpoShared.h"
#include "Zdxsv/OnlineLimits.h"
#include "Zdxsv/SyncSettings.h"
#include "Zdxsv/ReplayList.h"
#include "MTGS.h"
#include "Input/InputManager.h"
#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif
#include "ImGui/FullscreenUI.h"

namespace Zdxsv
{
	namespace
	{
		constexpr const char* DEFAULT_OPTIONS = "net=1,lobby=1";
		// ZDXSV_NET_TAIL=n: frames run after the end msg before the session stops (default 300).
		const int s_net_tail = [] {
			const char* e = Zdxsv::TestEnv("ZDXSV_NET_TAIL");
			return e ? std::atoi(e) : 300;
		}();
		bool s_sync_trimmed = false; // g_frame_period_trim_us holds a time sync wait

		int s_vsyncs = 0;
		Input s_random[GGPO_PLAYERS] = {};

		void Parse()
		{
			// a VM started in this process after one with other options (a replay after a lobby game)
			s_opt = {};
			s_opt.osd = Host::GetBoolSettingValue("DEV9/Eth", "ZdxsvNetOsd", true); // osd= overrides it
			s_opt.frame_osd = BoolSetting("ZdxsvFrameOsd", false, "ZDXSV_FRAME_OSD");
			s_opt.port = s_options.find("lobby=1") != std::string::npos ? s_lobby_port : 7001;
			for (const std::string_view item : StringUtil::SplitString(s_options, ','))
			{
				const size_t eq = item.find('=');
				if (eq == std::string_view::npos)
					continue;
				const std::string_view key = item.substr(0, eq);
				const std::string_view value = item.substr(eq + 1);
				const int n = StringUtil::FromChars<int>(value).value_or(0);
				if (key == "start")
					s_opt.start = n;
				else if (key == "frames")
					s_opt.frames = n;
				else if (key == "hash")
					s_opt.hash = value == "full" ? Hash::Full : value == "pos" ? Hash::Pos : Hash::Pw;
				else if (key == "check")
					s_opt.check = std::clamp(n, 1, 6); // GGPO keeps MAX_PREDICTION_FRAMES + 2 = 8 states: frames 0..check
				else if (key == "seed")
					s_opt.seed = static_cast<u32>(n);
				else if (key == "input")
				{
					s_opt.host_input = (value == "host");
					s_opt.no_input = (value == "none");
				}
				else if (key == "mask")
					s_opt.mask = StringUtil::FromChars<u16>(value, 16).value_or(0xffff);
				else if (key == "control")
					s_opt.control_input = (value == "input");
				else if (key == "port")
					s_opt.port = n;
				else if (key == "host")
					s_opt.peer_host = std::string(value);
				else if (key == "delay")
				{
					s_opt.delay = n;
					s_opt.delay_set = true;
				}
				else if (key == "mindelay")
					s_min_delay = n;
				else if (key == "sync")
					s_opt.sync = (n != 0);
				else if (key == "net")
					s_opt.net = (n != 0);
				else if (key == "players")
					s_opt.players = std::clamp(n, 2, GGPO_MAX_PLAYERS);
				else if (key == "relay")
					s_opt.relay = n;
				else if (key == "lobby")
					s_opt.lobby = (n != 0);
				else if (key == "badsession")
					s_opt.bad_session = (n != 0);
				else if (key == "osd")
					s_opt.osd = (n != 0);
				else if (key == "live")
					s_opt.live_url = value;
				else if (key == "upload")
					s_opt.upload_url = value;
				else if (key == "advertise")
					; // GgpoLobbyAdvertisePort
				else if (key == "replay")
				{
					s_opt.replay_off = (value == "0");
					s_opt.replay_dir = s_opt.replay_off ? std::string() : Path::ToNativePath(value); // '/' fails on Windows
				}
				else
					Console.Warning("ZdxsvGgpo: unknown key '%.*s'", static_cast<int>(key.size()), key.data());
			}
			if (s_opt.net)
			{
				s_opt.sync = false; // the peers run different games (own position): no common hash
				s_opt.frames = std::numeric_limits<int>::max(); // ends with the battle (end msg)
			}
		}

	} // namespace

	RollbackState s_net_at[128]; // per frame & 127, at its save
	bool s_vm_closing = false; // in GgpoOnVmShutdown: GgpoStop does not shut the VM down (rbk)
	std::mt19937 s_ggpo_rng;

	// A net session runs the process at high priority: other programs delay the limiter's wake and the frame less.
	void GgpoPriority(bool high)
	{
#ifdef _WIN32
		static DWORD s_normal = 0;
		if (high && !s_normal)
		{
			s_normal = GetPriorityClass(GetCurrentProcess());
			SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
		}
		else if (!high && s_normal)
		{
			SetPriorityClass(GetCurrentProcess(), s_normal);
			s_normal = 0;
		}
#endif
	}

	void GgpoReport(const char* what)
	{
		Console.WriteLn("ZdxsvGgpo: %s frames %d rollback frames %d loads %d mismatches %d ggpo warnings %d | save ms mean %.3f max %.3f skipped %d | hash ms mean %.3f | load ms mean %.3f max %.3f",
			what, s_session_frames, s_stats.rollback_frames, s_stats.loads, s_stats.mismatches, s_stats.ggpo_warnings, s_stats.save_ms.Mean(), s_stats.save_ms.max, s_run.save_skipped,
			s_stats.hash_ms.Mean(), s_stats.load_ms.Mean(), s_stats.load_ms.max);
		Console.WriteLn("ZdxsvGgpo: %s wall ms per frame: emulate mean %.2f max %.1f | exit %.2f | between frames (save, ggpo, rollbacks) mean %.2f max %.1f | rerun frame mean %.2f max %.1f",
			what, s_stats.emu_ms.Mean(), s_stats.emu_ms.max, s_stats.exit_ms.Mean(), s_stats.ours_ms.Mean(), s_stats.ours_ms.max, s_stats.rerun_ms.Mean(), s_stats.rerun_ms.max);
		const double n = std::max(s_session_frames, 1);
		const double ours = s_stats.ours_ms.sum / n,
			split = (s_stats.save_ms.sum + s_stats.hash_ms.sum + s_stats.load_ms.sum + s_stats.rerun_ms.sum + s_stats.wait_ms.sum + s_stats.sleep_ms.sum) / n;
		Console.WriteLn("ZdxsvGgpo: %s between frames ms per frame %.2f: save %.2f hash %.2f load %.2f rerun %.2f wait %.2f sleep %.2f rest (ggpo) %.2f | sync=%d",
			what, ours, s_stats.save_ms.sum / n, s_stats.hash_ms.sum / n, s_stats.load_ms.sum / n, s_stats.rerun_ms.sum / n, s_stats.wait_ms.sum / n, s_stats.sleep_ms.sum / n,
			ours - split, s_opt.sync);
		Console.WriteLn("ZdxsvGgpo: %s rerun frame split: vu1 ms %.2f, spu2 ms %.3f, ee Mcycles %.3f", what,
			Common::Timer::ConvertValueToMilliseconds(g_rerun_vu1_ticks) / std::max(s_stats.rollback_frames, 1),
			Common::Timer::ConvertValueToMilliseconds(g_rerun_spu2_ticks) / std::max(s_stats.rollback_frames, 1), s_stats.rerun_mcycles.Mean());
		Console.WriteLn("ZdxsvGgpo: %s delta %s | %s", what, Zdxsv::DeltaStateTimes().c_str(), SaveState_DeltaTimes().c_str());
		Console.WriteLn("ZdxsvGgpo: %s output: presented frames %d | audio samples played %lld dropped (rerun) %lld",
			what, g_perfmon.GetFrame() - s_stats.gs_frame0, static_cast<long long>(s_stats.spu_played), static_cast<long long>(s_stats.spu_dropped));
	}

	Input GgpoHostInput()
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

	static void ApplyInputs(const Input* inputs)
	{
		for (int p = 0; p < GGPO_PLAYERS; p++)
			ApplyPad(p, inputs[p]);
	}

	bool GgpoSyncAndApply(bool rerun)
	{
		if (s_opt.net)
			return NetSyncAndApply();
		Input inputs[GGPO_PLAYERS] = {};
		int disconnect_flags = 0;
		const GGPOErrorCode rc = ggpo_synchronize_input(s_session, inputs, sizeof(inputs), &disconnect_flags);
		if (rc != GGPO_OK)
		{
			Console.Error("ZdxsvGgpo: synchronize_input %d", rc);
			return false;
		}
		if (rerun && s_opt.control_input)
			inputs[0].buttons ^= static_cast<u16>(s_ggpo_rng() | 1);
		ApplyInputs(inputs);
		return true;
	}

	// Local inputs of the next frame, then the synced inputs go to the pads.
	static bool NextInputs()
	{
		if (s_opt.net) // the session start: no input yet, GGPO's frame delay fills the first frames
			return NetSyncAndApply();
		if (s_session_frames % 5 == 0)
		{
			for (Input& in : s_random)
			{
				in.buttons = static_cast<u16>(s_ggpo_rng() & s_ggpo_rng() & s_opt.mask);
				if (s_opt.no_input)
					in.buttons = 0;
				in.lx = static_cast<u8>(s_ggpo_rng());
				in.ly = static_cast<u8>(s_ggpo_rng());
				in.rx = in.ry = Pad::ANALOG_NEUTRAL_POSITION;
				if (s_opt.no_input)
					in.lx = in.ly = Pad::ANALOG_NEUTRAL_POSITION;
			}
		}
		for (int p = 0; p < GGPO_PLAYERS; p++)
		{
			Input in = (p == 0 && s_opt.host_input) ? GgpoHostInput() : s_random[p];
			const GGPOErrorCode rc = ggpo_add_local_input(s_session, s_handles[p], &in, sizeof(in));
			if (rc != GGPO_OK)
			{
				Console.Error("ZdxsvGgpo: add_local_input %d", rc);
				return false;
			}
		}
		return GgpoSyncAndApply(false);
	}

	// Runs the CPU to the next vsync.
	bool GgpoRunFrame()
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

	// ZDXSV_REPLAY=file.pb: plays a saved replay (PlayLoad), the net=1 hooks on, no GGPO session. Else the replay
	// list's pick (s_play_picked) for one VM. Set by GgpoOnVmInitialize.
	const char* s_play_env = nullptr;
	std::string s_play_picked;
	int s_play_picked_pov = -1;
	bool s_play_listed = false;
	// Common start: the replay has no start state (or ZDXSV_REPLAY_COMMON=1). The booted state (any post-entry
	// state, tests/zdxsv/rbkprep.sh) plays the battle start with the file's lobby answers through RbkCall up to the
	// arm, which is GGPO frame 0 (PlayCommonStart).
	bool s_play_common = false;
	bool g_ggpo_enabled = false; // GgpoOnVmInitialize
	bool g_mtvu_off = false; // GgpoOnVmInitialize, cleared at VM shutdown
	s32 g_frame_period_trim_us = 0;
	bool g_limiter_no_catch_up = false;
	// The ZDXSV_GGPO options of this VM: the variable, else DEFAULT_OPTIONS for the Z game, else empty = off.
	// Set by GgpoOnVmInitialize before the CPU runs.
	std::string s_options;
	bool g_z_game = false;
	bool g_ggpo_active = false;
	bool g_ggpo_in_rollback = false;
	u64 g_rerun_vu1_ticks = 0;
	bool g_rerun_vu1_skip = false;
	bool g_rerun_draw_skip = false;
	u64 g_rerun_spu2_ticks = 0;
	bool g_gs_rerun_frame = false;
	bool s_net_env = false; // net=1 in s_options, or a replay plays (GgpoOnVmInitialize)
	// ZDXSV_RBK=i/N: until GGPO arms, every lobby / battle connect RPC is answered here (RbkCall).
	int s_rbk_me = -1, s_rbk_n = 0;
	bool RbkFromEnv()
	{
		s_rbk_me = -1;
		s_rbk_n = 0;
		const char* e = Zdxsv::TestEnv("ZDXSV_RBK");
		return e && std::sscanf(e, "%d/%d", &s_rbk_me, &s_rbk_n) == 2 && s_rbk_me >= 0 && s_rbk_me < s_rbk_n && s_rbk_n <= 4;
	}
	// Also set by a replay's common start (PlayLoad); back to ZDXSV_RBK at VM shutdown.
	bool s_rbk = RbkFromEnv();
	const char* s_rand_env = Zdxsv::TestEnv("ZDXSV_RAND_INPUT");
	// ZDXSV_K3_LAG: a msg sent at GGPO frame s goes into the local input of frame s + lag. With lag above
	// GGPO's 6 prediction frames, frame s is final when s + lag is added, so a send first made in a rerun
	// still lands on a fixed frame.
	const int s_k3_lag = [] {
		const char* e = Zdxsv::TestEnv("ZDXSV_K3_LAG");
		return e ? std::max(0, std::atoi(e)) : 8;
	}();
	RollbackState s_rb;
	FrameRings s_rings;
	BattleNet s_battle;
	NetStats s_ns = {};
	RunState s_run;
	SessionStats s_stats;
	GGPOPlayerHandle s_handles[GGPO_MAX_PLAYERS] = {};
	GgpoOptions s_opt;
	int s_lobby_port = 7001;
	int s_min_delay = 2; // mindelay=, else setting DEV9/Eth ZdxsvGgpoMinDelay
	// network status OSD: user id, name + pilot name per position (lobby battle info), lines of the last frame
	std::vector<LobbyPlayer> s_lobby_players, s_net_players;

	GGPOSession* s_session = nullptr;
	bool s_frame_ended = false; // the CPU left Execute() at a vsync
	static bool s_frame_hold = false; // the next frame waits, the CPU does not run (GgpoHoldsFrame)
	static Common::Timer s_hold_poll;
	int s_session_frames = 0;
	std::array<float, PadDualshock2::Inputs::LENGTH> s_host = {};

	// The host pad in a replay takeover: START is the takeover's retry / skip key, not game input.
	Input TakeoverHostInput()
	{
		Input in = GgpoHostInput();
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
	// (GgpoStop closed the session; its stats are reported).
	void NetReset()
	{
		s_battle = {};
		s_frame_ended = s_rerun = false;
		s_run = {};
		s_rb = {};
		std::fill(std::begin(s_net_at), std::end(s_net_at), RollbackState{});
		s_ns = {};
		s_rings = {};
		s_pw.clear();
		NetStatusReset();
		std::fill(std::begin(s_handles), std::end(s_handles), GGPOPlayerHandle{});
		g_limiter_no_catch_up = false;
		GgpoPriority(false);
		if (std::exchange(s_sync_trimmed, false))
			g_frame_period_trim_us = 0;
		s_session_frames = 0;
		s_diff_logged = 0;
		g_rerun_vu1_ticks = g_rerun_spu2_ticks = 0;
		s_stats = {};
		GgpoFrameOsdReset();
		Zdxsv::DeltaStateTimesReset();
	}

	void GgpoOnVsync()
	{
		if (s_rbk && s_net_env && !s_battle.armed)
			VMManager::SetLimiterMode(LimiterModeType::Turbo);
		// Once: the rbk knobs as received; zdxsv/rbk.sh compares this line to what it meant to pass
		static bool rbk_env_logged = false;
		if (s_rbk && !s_play_env && !rbk_env_logged)
		{
			rbk_env_logged = true;
			const auto env = [](const char* k) { const char* v = TestEnv(k); return v && *v ? v : "-"; };
			Console.WriteLn("ZdxsvGgpo: rbk env pos=%d/%d rand=%s turbo=%s clamp=%s ggpo=%s", s_rbk_me, s_rbk_n,
				env("ZDXSV_RAND_INPUT"), env("ZDXSV_RBK_TURBO"), env("ZDXSV_EE_CLAMP"), env("ZDXSV_GGPO"));
		}
		// ZDXSV_VM_TEST=frame:shutdown|reset|mute|ui (tests of GgpoOnVmShutdown; mute: the GGPO network goes silent): once, when a session or replay reaches GGPO frame `frame`
		static const char* vm_test = TestEnv("ZDXSV_VM_TEST");
		if (vm_test && g_ggpo_active && !g_ggpo_in_rollback && s_battle.frame == std::atoi(vm_test))
		{
			const char* test = vm_test;
			vm_test = nullptr;
			if (std::strstr(test, ":mute"))
				ggpo_set_mute(s_session, true);
			else if (std::strstr(test, ":reset"))
				Host::RunOnCPUThread([] {
					OnlineAllowReset();
					VMManager::Reset();
				});
			else if (std::strstr(test, ":ui"))
				Host::RunOnCPUThread([] {
					VMManager::SetLimiterMode(LimiterModeType::Turbo);
					VMManager::FrameAdvance(1);
					Error error;
					if (!VMManager::LoadStateFromSlot(1, false, &error))
						Console.WriteLn("ZdxsvGgpo: vm test load state: %s", error.GetDescription().c_str());
					VMManager::SetPaused(true);
					FullscreenUI::OpenPauseMenu();
				});
			else
				Host::RunOnCPUThread([] { Host::RequestVMShutdown(false, false, false); });
			Console.WriteLn("ZdxsvGgpo: vm test %s at frame %d", test, s_battle.frame);
		}
		TracePad();
		TraceInputs();
		// ZDXSV_SNAP=dir,n[,from[,to]]: GS screenshot dir/v<vsync>.png every n vsyncs in [from, to) (needs a
		// real renderer, not -Headless)
		static const char* snap = Zdxsv::TestEnv("ZDXSV_SNAP");
		static const char* snap_c = snap ? std::strchr(snap, ',') : nullptr;
		static const int snap_n = snap_c ? std::atoi(snap_c + 1) : 0;
		static const char* snap_f = snap_c ? std::strchr(snap_c + 1, ',') : nullptr;
		static const u32 snap_from = snap_f ? std::atoi(snap_f + 1) : 0;
		static const char* snap_t = snap_f ? std::strchr(snap_f + 1, ',') : nullptr;
		static const u32 snap_to = snap_t ? std::atoi(snap_t + 1) : UINT32_MAX;
		if (snap_n > 0 && !g_ggpo_in_rollback && g_FrameCount >= snap_from && g_FrameCount < snap_to && g_FrameCount % snap_n == 0)
			GSQueueSnapshot(Path::Combine(std::string_view(snap, std::strchr(snap, ',') - snap), fmt::format("v{}.png", g_FrameCount)));
		if (!g_ggpo_enabled)
			return;
		if (!g_ggpo_active)
		{
			if (!g_ggpo_enabled || s_battle.started)
				return;
			if (s_vsyncs++ == 0)
			{
				Parse();
				if (s_play_env)
					Host::RunOnCPUThread(PlayLoad);
			}
			if (s_play_env && !(s_play_common && s_battle.armed)) // PlayLoad starts it, or the arm of a common start
				return;
			if (s_opt.net ? !s_battle.armed : s_vsyncs < s_opt.start)
				return;
			s_battle.started = true;
			g_ggpo_active = true;
			s_stats.gs_frame0 = g_perfmon.GetFrame();
		}
		s_frame_ended = true;
		if (!g_ggpo_in_rollback)
		{
			s_stats.t_vsync = Common::Timer::GetCurrentValue();
			if (s_stats.t_returned)
				s_stats.emu_ms.Add(Common::Timer::ConvertValueToMilliseconds(s_stats.t_vsync - s_stats.t_returned));
		}
		Cpu->ExitExecution();
	}

	bool GgpoDeferThrottle()
	{
		s_run.throttle_deferred = g_ggpo_active && s_session && !s_play_env && s_frame_ended;
		return s_run.throttle_deferred;
	}

	// The limiter sleep and input poll that VSyncStart left for after the rollback.
	static void DeferredThrottle(bool poll = true)
	{
		if (!std::exchange(s_run.throttle_deferred, false))
			return;
		Common::Timer timer;
		if (!VMManager::Internal::IsExecutionInterrupted())
			VMManager::Internal::Throttle();
		if (poll)
			VMManager::Internal::PollInputOnCPUThread();
		s_stats.sleep_ms.Add(timer.GetTimeMilliseconds());
	}

	static void Returned();

	bool SpuOnOutput()
	{
		const bool drop = g_ggpo_in_rollback || g_replay_silent_frame;
		(drop ? s_stats.spu_dropped : s_stats.spu_played)++;
		return drop;
	}

	void GgpoOnExecuteReturned()
	{
		if (!g_ggpo_active || !std::exchange(s_frame_ended, false))
			return;
		const Common::Timer::Value t0 = Common::Timer::GetCurrentValue();
		if (s_stats.t_vsync)
			s_stats.exit_ms.Add(Common::Timer::ConvertValueToMilliseconds(t0 - s_stats.t_vsync));
		SyncSettingsOnBattle(true); // before the session's first frame
		const bool framed = s_session && s_stats.t_vsync;
		Returned();
		DeferredThrottle(); // a Returned that ended the session
		s_stats.t_returned = Common::Timer::GetCurrentValue();
		s_stats.ours_ms.Add(Common::Timer::ConvertValueToMilliseconds(s_stats.t_returned - t0));
		GgpoBudgetFrame(framed && s_session, s_stats.rerun_ms.sum, s_stats.wait_ms.sum, s_stats.sleep_ms.sum);
		if (s_opt.frame_osd)
			GgpoFrameOsdSample(s_session_frames);
	}

	bool GgpoHoldsFrame()
	{
		if (!s_frame_hold)
			return false;
		Threading::Sleep(2);
		if (s_hold_poll.GetTimeMilliseconds() >= 16)
		{
			s_hold_poll.Reset();
			InputManager::PollSources();
			MTGS::PresentCurrentFrame();
		}
		s_stats.t_vsync = 0;
		s_frame_ended = true;
		return true;
	}

	static void Returned()
	{
		std::unique_lock ggpo_lock(s_ggpo_mtx);
		if (s_play_env)
		{
			s_frame_hold = !s_play_common && LiveHold();
			if (!s_frame_hold)
				s_play_common ? PlayCommonStart() : PlayNext();
			return;
		}
		if (!s_session)
		{
			if (const char* fail = GgpoStart())
			{
				GgpoStop(fail);
				return;
			}
			if (s_opt.net)
				ReplayBegin();
			// The synctest saves frame 0 at the first synchronize_input.
			if (!NextInputs())
				GgpoStop("failed");
			return;
		}
		// net: the input is read and sent before the rollback, so its age does not depend on the rollback's length.
		if (s_opt.net)
		{
			if (s_run.throttle_deferred)
			{
				VMManager::Internal::PollInputOnCPUThread();
				if (!s_session) // the input poll ran a VM shutdown or reset
					return;
			}
			// Not at frame 0: an input added before the first read lets GGPO discard frames the next read needs.
			if (s_session_frames != 0 && !NetAddLocalInput(s_battle.frame + 1))
			{
				GgpoStop("failed");
				return;
			}
		}
		if (ggpo_advance_frame(s_session) != GGPO_OK || ggpo_idle(s_session, 0) != GGPO_OK)
		{
			GgpoStop("failed");
			return;
		}
		s_session_frames++;
		NetEndCheck();
		if (s_opt.net && s_battle.end >= 0 && ++s_battle.end > s_net_tail)
		{
			GgpoStop("net battle end");
			return;
		}
		if (s_run.disconnected && s_battle.end < 0)
		{
			// A peer leaves once it confirmed the end msg, which took our input of that frame: an end msg still to
			// come is no later than our last input's frame.
			if (s_run.disconnected_until < 0)
				s_run.disconnected_until = s_battle.frame + s_opt.delay + 1;
			if (!s_opt.net || s_battle.frame > s_run.disconnected_until)
			{
				GgpoStop("disconnected");
				return;
			}
		}
		// net: ahead of the peer, give it time to catch up: GGPO's suggested frames, one frame (at 60 fps) every
		// 10 frames, so the catch-up is not one visible hitch.
		if (std::exchange(s_sync_trimmed, false))
			g_frame_period_trim_us = 0;
		if (s_run.frames_ahead > 0 && s_session_frames % 10 == 0)
		{
			s_run.frames_ahead--;
			s_run.sync_frames++;
			Common::Timer wait;
			double idle_ms = 0;
			while (wait.GetTimeMilliseconds() < 1000.0 / 60.0)
			{
				Common::Timer idle;
				ggpo_idle(s_session, 0);
				idle_ms += idle.GetTimeMilliseconds();
				Threading::Sleep(1);
			}
			// ggpo_idle can run a rollback: that is game work, not part of the wait.
			s_run.sync_max_ms = std::max(s_run.sync_max_ms, wait.GetTimeMilliseconds() - idle_ms);
			s_stats.wait_ms.Add(wait.GetTimeMilliseconds());
			// The limiter keeps an absolute schedule: without this it skips its next sleep and the wait gains nothing.
			g_frame_period_trim_us = static_cast<s32>(wait.GetTimeMilliseconds() * 1000.0);
			s_sync_trimmed = true;
		}
		if (s_session_frames >= s_opt.frames)
		{
			GgpoStop("done");
			return;
		}
		if (s_session_frames % 600 == 0)
			GgpoReport("progress");
		NetStatusUpdate(s_session_frames % 600 == 0);
		if (s_opt.net && s_session_frames % 60 == 0)
			NetStatusSamplePings();
		if (s_opt.net)
			NetStatusSampleFps();
		if (s_opt.net)
		{
			const bool ok = NetWaitPredictionLimit();
			ggpo_lock.unlock();
			DeferredThrottle(false);
			ggpo_lock.lock();
			if (!ok || !NetSyncAndApply())
				GgpoStop("failed");
			return;
		}
		DeferredThrottle();
		if (!s_session) // the input poll ran a VM shutdown or reset
			return;
		if (!NextInputs())
			GgpoStop("failed");
	}

	bool GgpoCaptureHostInput(u32 controller, u32 bind, float value)
	{
		if (!g_ggpo_active)
			return false;
		if (controller == 0 && bind < s_host.size())
			s_host[bind] = value;
		return controller < GGPO_PLAYERS;
	}

	bool g_net_hook = false; // GgpoOnVmInitialize
	bool g_zd_hook = false;
	bool g_ps_hook = true;

	void GgpoOnVmInitialize(const char* serial, u32 crc)
	{
		TakeNextReplay(s_play_picked, s_play_picked_pov);
		const char* play = TestEnv("ZDXSV_REPLAY");
		s_play_env = play ? play : s_play_picked.empty() ? nullptr : s_play_picked.c_str();
		s_play_listed = !play && !s_play_picked.empty();
		const char* e = TestEnv("ZDXSV_GGPO");
		// ZDXSV_GAME_CRC=hex (test): the CRC taken as the Z game's, to run the wrong-game path on the Z game
		const char* want_env = Zdxsv::TestEnv("ZDXSV_GAME_CRC");
		const u32 want = want_env ? static_cast<u32>(std::strtoul(want_env, nullptr, 16)) : GAME_CRC;
		const bool serial_match = std::strcmp(serial, GAME_SERIAL) == 0;
		const char* ext = TestEnv("ZDXSV_EXTENSIONS");
		const bool ext_off = ext && std::strcmp(ext, "0") == 0;
		g_z_game = serial_match && crc == want && !ext_off;
		SingleDrawListOnVmInitialize();
		AllyHpOnVmInitialize();
		NoDev9OnVmInitialize();
		const bool lobby_default = g_z_game && !s_play_env;
		if (!g_z_game) // any other game or build, or ZDXSV_EXTENSIONS=0: no GGPO, replay, hooks or platform info
		{
			s_options.clear();
			g_ggpo_enabled = s_net_env = g_net_hook = g_zd_hook = g_ps_hook = false;
			if (ext_off)
				Console.WriteLn("ZdxsvGgpo: off: ZDXSV_EXTENSIONS=0, all zdxsv extensions off");
			else if (serial_match || (e && std::strcmp(e, "0") != 0) || s_play_env || s_net_trace)
				Console.Warning("ZdxsvGgpo: off: not the Z game (serial %s CRC %08X, need %s %08X)", serial, crc, GAME_SERIAL, want);
			return;
		}
		SyncSettingsEnforce(); // LoadSettings ran before the game was known
		if (e && std::strcmp(e, "0") == 0) // off (rigs without GGPO)
			s_options.clear();
		else
			s_options = e ? e : lobby_default ? DEFAULT_OPTIONS : "";
		// mindelay= overrides it
		s_min_delay = std::clamp(Host::GetIntSettingValue("DEV9/Eth", "ZdxsvGgpoMinDelay", 2), 2, 6);
		if (s_options.find("lobby=1") != std::string::npos)
		{
			s_lobby_port = Host::GetIntSettingValue("DEV9/Eth", "ZdxsvGgpoPort", 0);
			if (s_lobby_port <= 0 || s_lobby_port > 0xFFFF)
			{
				std::random_device rd;
				s_lobby_port = std::uniform_int_distribution<int>(29700, 29800)(rd);
				Host::SetBaseIntSettingValue("DEV9/Eth", "ZdxsvGgpoPort", s_lobby_port);
				Host::CommitBaseSettingChanges();
				Console.WriteLn("ZdxsvGgpo: battle port %d picked and saved", s_lobby_port);
			}
		}
		g_ggpo_enabled = !s_options.empty() || s_play_env;
		s_net_env = s_options.find("net=1") != std::string::npos || s_play_env;
		g_net_hook = s_net_trace != nullptr || s_net_env || g_no_dev9;
		g_zd_hook = s_net_env;
		g_ps_hook = true;
		if (!s_options.empty() || std::strcmp(serial, GAME_SERIAL) == 0)
			Console.WriteLn("ZdxsvGgpo: options '%s' (serial %s)", s_options.c_str(), serial);
		// Delta saves and loads, replay keys: VU1 memory is copied while the MTVU thread may still run on it.
		// The settings were loaded before this; VMManager::LoadCoreSettings keeps it off on later reloads.
		g_mtvu_off = g_ggpo_enabled || g_delta_state_test_enabled;
		if (g_mtvu_off && EmuConfig.Speedhacks.vuThread)
		{
			EmuConfig.Speedhacks.vuThread = false;
			Console.WriteLn("ZdxsvGgpo: MTVU speedhack off for this VM");
		}
	}

	void NetSaved(int frame)
	{
		s_battle.frame = frame;
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
		Console.WriteLn("ZdxsvGgpo: net sends %u msgs %u (sent %zu, unsent %zu) recvs %u rxmsgs %u rxbytes %u polls %u other %u nowait %u senddiff %u toolong %u maxq %u waits %d sync %d (max %.1f ms) k3lag %d restamp %u late %u",
			s_ns.sends, s_ns.msgs, s_battle.sent.size(), s_battle.out.size(), s_ns.recvs, s_ns.rxmsgs, s_ns.rxbytes, s_ns.polls,
			s_ns.other, s_ns.nowait, s_ns.senddiff, s_ns.toolong, s_ns.maxq, s_run.waits, s_run.sync_frames, s_run.sync_max_ms, s_k3_lag, s_ns.restamp, s_ns.late);
		Console.WriteLn("ZdxsvGgpo: zd steps %u changed %u echo %u skip %u k3 %d/%d/%d/%d rel %d (fwd %u)", s_battle.zd_steps, s_battle.zd_changed, s_battle.zds_echo, s_battle.zds_skip,
			s_rb.zds_seen[0], s_rb.zds_seen[1], s_rb.zds_seen[2], s_rb.zds_seen[3], s_rb.zds_rel, s_battle.zds_k3rel);
		if (s_pw_hash && s_net_trace)
		{
			s_pw.erase(s_pw.upper_bound(s_battle.last_real), s_pw.end());
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
	// EE probe (iR5900.cpp): GGPO frame being run, the frame numbers of NET_TRACE H lines and PW dumps.
	int ProbeFrame() { return s_battle.frame; }

	void GgpoOnVmShutdown(const char* what)
	{
		if (std::strcmp(what, "vm shutdown") == 0)
			g_mtvu_off = false; // the next VM decides again (a reset VM keeps it off)
		SyncSettingsReset();
		OnlineLimitsReset();
		FastLoginReset();
		StopCutRefusal();
		FastBootReset();
		SingleDrawListReset();
		NoDev9Reset();
		s_rbk = RbkFromEnv();
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
			GgpoStop(what); // peers see a disconnect; the replay is saved up to the last confirmed frame
			s_vm_closing = false;
		}
		Zdxsv::DeltaStateClear();
		NetReset();
		RbkReset();
		g_ggpo_in_rollback = false;
		s_frame_hold = s_frame_ended = false;
		s_vsyncs = 0; // the next VM start or the reset VM parses the options again (and loads a replay again)
	}
} // namespace Zdxsv

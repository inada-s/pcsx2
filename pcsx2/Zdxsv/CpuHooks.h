// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the CPU and timing code: recompilers (iR5900.cpp, iR3000A.cpp), Interpreter.cpp, Counters.cpp,
// MTGS.cpp, VMManager.cpp, VU1micro.cpp. Only flags and declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

class SettingsInterface;

namespace Zdxsv
{
	extern bool g_ggpo_enabled; // GGPO options (ZDXSV_GGPO, else on by default) or a replay
	extern bool g_ggpo_active; // a session runs
	extern bool g_ggpo_in_rollback; // rerunning frames: no throttle
	extern u64 g_rerun_vu1_ticks; // Common::Timer ticks in VU1 microcode during rerun frames (microVU.cpp)
	// this rerun frame starts no VU1 microprogram (vu1ExecMicro; Ggpo.cpp AdvanceFrame, ZDXSV_RERUN_VU1)
	extern bool g_rerun_vu1_skip;
	extern bool g_replay_fast_frame; // not throttled
	// ZDXSV_VU1_OPTEST: once, at the first VU1 micro program start (VuOpTest.cpp)
	void VuOpTest();
	// ZDXSV_EE_OPTEST: in VSyncStart; at vsync 300 runs the COP1 op test as guest code, restores at its end (EeOpTest.cpp)
	void EeOpTestOnVsync();
	// GGPO, a replay or ZDXSV_DELTA_TEST in this VM: the MTVU speedhack stays off (VMManager::LoadCoreSettings)
	extern bool g_mtvu_off;
	// live spectating (Ggpo.cpp LivePace, CPU thread): microseconds added to the frame limiter's period at nominal
	// speed, to hold a distance behind the live edge without whole-frame stalls
	extern s32 g_frame_period_trim_us;

	extern bool g_z_game; // the disc is the Z game: serial SLPS-25419 and its ELF CRC (GgpoOnVmInitialize)
	extern bool g_aeug_game; // the disc is SLPS-25305 (AEUG vs. Titans) with its ELF CRC (GgpoOnVmInitialize)
	// In VMManager::LoadCoreSettings, the Z game: settings that change emulation results to PCSX2's defaults, the
	// speedhacks too in a battle; an OSD message names the overridden ones (SyncSettings.cpp)
	void SyncSettingsEnforce();
	void OnlineLimitsOnVsync();
	// In VSyncStart, the Z game: when the game starts DNAS on the way to the lobby, log in through the login API with
	// the token in <settings>/zdxsv_auth_token.txt and jump to the lobby connect, skipping
	// DNAS and the login browser (FastLogin.cpp); again at every DNAS start.
	// No token or no answer: the game goes on as without it. FastLoginReset in GgpoOnVmShutdown drops a pending login.
	void FastLoginOnVsync();
	void FastLoginReset();
	// Boot to the lobby user_id selection at unlimited speed, pressing through the menus on the fixed zdxsv route (FastBoot.cpp).
	void FastBootOnVsync();
	void FastBootReset();
	// Z game: the game draws a frame's list in that frame, not the next.
	extern bool g_single_draw_list;
	void SingleDrawListOnVmInitialize();
	// In VSyncStart with g_single_draw_list: patches the game code once it is loaded.
	void SingleDrawListOnVsync();
	// In GgpoOnVmShutdown.
	void SingleDrawListReset();
	// Z game, setting ZdxsvAllyHp: the HUD shows the teammate's HP where it shows the own max HP (AllyHp.cpp).
	void AllyHpOnVmInitialize();
	// In VMManager::LoadCoreSettings.
	void AllyHpLoadSettings(SettingsInterface& si);
	// In VSyncStart, the Z game.
	void AllyHpOnVsync();
	// Z game, setting ZdxsvStickMessageButton: that button held + D-pad or left stick = the right stick (StickMessage.cpp).
	void StickMessageOnVmInitialize();
	// In VMManager::LoadCoreSettings.
	void StickMessageLoadSettings(SettingsInterface& si);
	// In GgpoOnVmShutdown.
	void StickMessageReset();
	// Z game with a server host: online with or without DEV9, the net RPCs answered on the EE (NoDev9.cpp).
	extern bool g_no_dev9;
	void NoDev9OnVmInitialize();
	// In VSyncStart with g_no_dev9: the network device pick without dev9.
	void NoDev9OnVsync();
	// At NET_RPC_PC with g_no_dev9. true: answered.
	bool NoDev9OnNetCall();
	// In VMManager::IdlePollUpdate (CPU thread, 10 Hz) while paused with g_no_dev9: the lobby connection is read, so its
	// line checks are answered (no game reads while paused).
	void NoDev9OnPausedIdle();
	// In GgpoOnVmShutdown.
	void NoDev9Reset();
	// In VMManager::Initialize, before the CPU runs: the state file to boot from, or "".
	const char* FastBootInitialState();

	// In VMManager::Initialize, once the disc serial and CRC are known and before the CPU runs: the GGPO options
	// of this VM (ZDXSV_GGPO, else the default) and the flags above that follow them. Any other
	// game (or another build of it): all off, the hooks too, whatever ZDXSV_GGPO / ZDXSV_REPLAY say.
	void GgpoOnVmInitialize(const char* serial, u32 crc);
	// In VMManager::Shutdown and VMManager::Reset, CPU thread, before the VM state goes: a running session ends
	// (Stop: peers see a disconnect, the replay is saved), the replay key files are deleted, the per-battle state
	// goes back to start values. what = "vm shutdown" / "vm reset".
	void GgpoOnVmShutdown(const char* what);
	// In VMManager::Internal::Throttle. Net session: a late frame is not paid back by shorter ones: the cause of the
	// delay (a rollback, the prediction barrier) is usually still there, and the frame interval would swing.
	extern bool g_limiter_no_catch_up;
	// In VSyncStart.
	void GgpoOnVsync();
	// In VSyncStart with LowLatencyPresent, after the present. true: a session frame ended, and
	// GgpoOnExecuteReturned does the limiter sleep and the input poll after the rollback, so a
	// rollback shorter than the sleep does not delay the next present.
	bool GgpoDeferThrottle();
	// In VMManager::Execute, before the CPU runs. true: the frame is held (a live replay waits for data at its
	// newest frame): the CPU does not run, GgpoOnExecuteReturned asks again, and the VM loop handles events between.
	bool GgpoHoldsFrame();
	// In VMManager::Execute, after the CPU returned.
	void GgpoOnExecuteReturned();

	// Z battle net HLE. The EE recompiler calls OnNetCall when the game enters its
	// net RPC wrapper (NET_RPC_PC: fno in a0, request header + data at 0xc22c9c).
	constexpr u32 NET_RPC_PC = 0x30e380;
	constexpr u32 AEUG_NET_RPC_PC = 0x2ebad0; // SLPS-25305: no-DEV9 only
	inline u32 NetRpcPc() { return g_aeug_game ? AEUG_NET_RPC_PC : NET_RPC_PC; }
	extern bool g_net_hook; // the recompiler emits the OnNetCall call at NET_RPC_PC
	// At NET_RPC_PC: true = the RPC was answered here (v0 set, pc = ra), skip the wrapper.
	bool OnNetCall();
	// Battle hook PCs per game. SLPS-25305: static matches of the SLPS-25419 code, not run yet (needs proper testing).
	struct GgpoHookPcs
	{
		u32 net_recv_ret; // fno 0x14 recv, return of its wait RPC
		u32 step_copy; // lockstep step's ring read: a1 = ring entry, s0 = position
		u32 load_step, load_step_end; // battle load step past its load-busy check; its epilogue
		u32 ms_step, ms_step_end; // MS-select load step past its load-busy check; its epilogue
		u32 draw_run, draw_run2; // render callback runners of the battle frame step, run after the frame logic
		u32 obj_walk, obj_call; // object draw walker; its handler call (jalr, handler in v0)
	};
	constexpr GgpoHookPcs GGPO_PCS_Z = {0x30ecf0, 0x312bf4, 0x2b1d80, 0x2b1fa8, 0x2b8698, 0x2b8ab8, 0x20ff30, 0x20ff90, 0x2ac4e0, 0x2ac524};
	constexpr GgpoHookPcs GGPO_PCS_AEUG = {0x2ec440, 0x2f0354, 0x281370, 0x2815c0, 0x286d38, 0x287118, 0x1eea70, 0x1eead0, 0x27c7a0, 0x27c7e4};
	inline const GgpoHookPcs& GgpoPcs() { return g_aeug_game ? GGPO_PCS_AEUG : GGPO_PCS_Z; }
	void OnNetRecv();
	// zd=1: at step_copy the ring entry gets the GGPO input.
	extern bool g_zd_hook;
	void OnStepCopy();
	// Play start: at load_step; true = held (v0 = 0, pc = epilogue).
	extern bool g_ps_hook;
	bool OnLoadStep();
	// At ms_step (same hold, under g_ps_hook).
	bool OnMsStep();
	// At draw_run / draw_run2: true = skipped in this rerun frame (pc = ra), see g_rerun_draw_skip.
	// OnDrawRun may run the object draw walker (obj_walk) alone instead; OnObjCall then skips its
	// handler calls but the ones RerunKeepDrawsHandler names.
	extern bool g_rerun_draw_skip;
	bool OnDrawRun();
	bool OnDrawRun2();
	bool OnObjCall();
	int ProbeFrame(); // GGPO frame being run (EE probe lines)

	extern bool g_ee_probe; // ZDXSV_EE_PROBE or ZDXSV_EE_WATCH is set
	void EeProfileOnRerun(); // each rerun frame, CPU thread: ZDXSV_EE_PROFILE (RecProbe.cpp)
	// EE recompiler, before each non-delay-slot instruction: any call to emit at pc.
	inline bool RecHooksOn() { return g_ee_probe || g_net_hook || g_zd_hook || g_ps_hook; }
	// Emits the probe and hook calls for pc; dispatcher = where a hook that answered leaves the block.
	void RecEmitHooks(u32 pc, const void* dispatcher);
	// EE interpreter, before each non-delay-slot instruction: the hooks of RecEmitHooks but the probes.
	inline bool IntHooksOn() { return g_net_hook || g_zd_hook || g_ps_hook; }
	// true = a hook answered and moved the pc: the instruction at pc does not run (IntHooks.cpp).
	bool IntRunHooks(u32 pc);
	// The pcs IntRunHooks acts on (the ARM64 EE recompiler calls it before these only).
	inline bool IsIntHookPc(u32 pc)
	{
		const GgpoHookPcs& p = GgpoPcs();
		return pc == NetRpcPc() || pc == p.net_recv_ret || pc == p.step_copy || pc == p.load_step || pc == p.ms_step ||
			   pc == p.draw_run || pc == p.draw_run2 || pc == p.obj_call;
	}

	// ZDXSV_DELTA_TEST: synctest in a running game, see Zdxsv/DeltaState.cpp.
	extern bool g_delta_state_test_enabled;
	void DeltaStateOnVsync();

	extern bool g_input_latency_enabled; // ZDXSV_INPUT_LATENCY is set (debug, Zdxsv/InputLatency.h)

	// CPU thread, every vsync, when the emulated frame ends (before limiter sleep and push).
	void InputLatencyOnFrameEnd();
	// CPU thread, around the frame push to the GS thread (gsPostVsyncStart).
	void InputLatencyOnPush(bool after);
	// CPU thread, every vsync, after the host input poll.
	void InputLatencyOnVsync();
	// GS thread, before GSvsync (present) of a vsync.
	void InputLatencyOnPresentStart();
	// GS thread, after a vsync's frame was presented.
	void InputLatencyOnPresent();

	// ZDXSV_EVENT_LOG=<file>[,<lines>[,<from>]] (RecProbe.cpp): from EventLogStart, the start values (S), then one line per EE and IOP event test,
	// up to <lines> (default 2000000); two builds of a CPU core change must write the same file. With <from>, after that many S/E/I
	// lines also one B line per EE recompiler block entry.
	extern bool g_event_log; // a log is open
	void EventLogStart(); // replay playback, after the common state load
	void EventLogEe(); // _cpuEventTest_Shared entry
	void EventLogIop(); // iopEventTest entry
	bool EventLogBlocks(); // the EE recompilers emit EventLogBlock at each block start
	void EventLogBlock(u32 pc);
} // namespace Zdxsv

// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the CPU and timing code: recompilers (iR5900.cpp, iR3000A.cpp), Counters.cpp,
// MTGS.cpp, VMManager.cpp. Only flags and declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	extern bool g_ggpo_enabled; // GGPO options (ZDXSV_GGPO or the ZdxsvGgpo setting) or a replay
	extern bool g_ggpo_active; // a session runs
	extern bool g_ggpo_in_rollback; // rerunning frames: no throttle
	// GGPO, a replay or ZDXSV_DELTA_TEST in this VM: the MTVU speedhack stays off (VMManager::LoadCoreSettings)
	extern bool g_mtvu_off;
	// live spectating (Ggpo.cpp LivePace, CPU thread): microseconds added to the frame limiter's period at nominal
	// speed, to hold a distance behind the live edge without whole-frame stalls (flycast gdxsv_frame_period_trim_us)
	extern s32 g_frame_period_trim_us;

	extern bool g_z_game; // the disc is the Z game: serial SLPS-25419 and its ELF CRC (GgpoOnVmInitialize)

	// In VMManager::Initialize, once the disc serial and CRC are known and before the CPU runs: the GGPO options
	// of this VM (ZDXSV_GGPO, else the ZdxsvGgpo setting) and the flags above that follow them. Any other
	// game (or another build of it): all off, the hooks too, whatever ZDXSV_GGPO / ZDXSV_REPLAY say.
	void GgpoOnVmInitialize(const char* serial, u32 crc);
	// In VMManager::Shutdown and VMManager::Reset, CPU thread, before the VM state goes: a running session ends
	// (Stop: peers see a disconnect, the replay is saved), the replay key files are deleted, the per-battle state
	// goes back to start values. what = "vm shutdown" / "vm reset".
	void GgpoOnVmShutdown(const char* what);
	// In VSyncStart.
	void GgpoOnVsync();
	// In VMManager::Execute, after the CPU returned.
	void GgpoOnExecuteReturned();

	// Z battle net HLE. The EE recompiler calls OnNetCall when the game enters its
	// net RPC wrapper (NET_RPC_PC: fno in a0, request header + data at 0xc22c9c).
	constexpr u32 NET_RPC_PC = 0x30e380;
	extern bool g_net_hook; // the recompiler emits the OnNetCall call at NET_RPC_PC
	// At NET_RPC_PC: true = the RPC was answered here (v0 set, pc = ra), skip the wrapper.
	bool OnNetCall();
	// fno 0x14 recv (0x30ec70), return of its wait RPC.
	constexpr u32 NET_RECV_RET_PC = 0x30ecf0;
	void OnNetRecv();
	// zd=1: lockstep step's ring read (0x312bf4: a1 = ring entry, s0 = position) gets the GGPO input.
	constexpr u32 STEP_COPY_PC = 0x312bf4;
	extern bool g_zd_hook;
	void OnStepCopy();
	// ZDXSV_ZDS_PS=1: battle load step past its load-busy check; true = held (v0 = 0, pc = epilogue).
	constexpr u32 LOAD_STEP_PC = 0x2b1d80;
	extern bool g_ps_hook;
	bool OnLoadStep();
	int ProbeFrame(); // GGPO frame being run (EE probe lines)

	extern bool g_ee_probe; // ZDXSV_EE_PROBE or ZDXSV_EE_WATCH is set
	// EE recompiler, before each non-delay-slot instruction: any call to emit at pc.
	inline bool RecHooksOn() { return g_ee_probe || g_net_hook || g_zd_hook || g_ps_hook; }
	// Emits the probe and hook calls for pc; dispatcher = where a hook that answered leaves the block.
	void RecEmitHooks(u32 pc, const void* dispatcher);

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
} // namespace Zdxsv

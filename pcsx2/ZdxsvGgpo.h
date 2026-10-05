// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

// zdxsv GGPO (#31 step 3): while a session runs, the CPU leaves Execute() at every vsync, so a
// frame is the unit GGPO works in (like flycast's emu.run()). Between frames: save (delta state),
// maybe roll back (load + rerun frames inside the advance_frame callback), then the synced pad
// inputs of the next frame are written to both pads. Host pad input is held back meanwhile.
namespace ZdxsvGgpo
{
	extern bool g_enabled; // ZDXSV_GGPO is set
	extern bool g_active; // a session runs
	extern bool g_in_rollback; // rerunning frames: no throttle

	// In VSyncStart.
	void OnVsync();
	// In VMManager::Execute, after the CPU returned.
	void OnExecuteReturned();
	// In Pad::SetControllerState: true when the host input was taken (session running).
	bool CaptureHostInput(u32 controller, u32 bind, float value);

	// Z battle net HLE (#31 step 4). The EE recompiler calls OnNetRpc when the game enters its
	// net RPC wrapper (NET_RPC_PC: fno in a0, request header + data at 0xc22c9c).
	// ZDXSV_NET_TRACE=<file>: log every battle send (fno 0x10, sock 0) and its parsed key slots.
	constexpr u32 NET_RPC_PC = 0x30e380;
	extern bool g_net_hook; // the recompiler emits the OnNetRpc call at NET_RPC_PC
	void OnNetRpc();
	// Lobby battles (net=1,lobby=1): our GGPO UDP port for the lobby's platform info, 0 = off.
	int LobbyPort();
	// The lobby's battle info (DEV9 thread): ok = every other player has a GGPO address;
	// byPosition = (IPv4 network byte order, port) per battle position, own position {0, 0}.
	void SetLobbyPeers(bool ok, std::vector<std::pair<u32, u16>> byPosition);
	// Slowest peer rtt in ms (-1 = unknown) for the lobby battle's auto delay (no delay= key):
	// max(mindelay, ceil(rtt / 2 / 16 ms)), read when GGPO arms.
	void SetLobbyRttSource(std::function<int(int& up, int& total)> source);
} // namespace ZdxsvGgpo

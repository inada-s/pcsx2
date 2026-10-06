// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "Zdxsv/Lobby.h"
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// zdxsv GGPO (#31 step 3): while a session runs, the CPU leaves Execute() at every vsync, so a
// frame is the unit GGPO works in (like flycast's emu.run()). Between frames: save (delta state),
// maybe roll back (load + rerun frames inside the advance_frame callback), then the synced pad
// inputs of the next frame are written to both pads. Host pad input is held back meanwhile.
namespace Zdxsv
{
	extern bool g_ggpo_enabled; // ZDXSV_GGPO is set
	extern bool g_ggpo_active; // a session runs
	extern bool g_ggpo_in_rollback; // rerunning frames: no throttle

	// In VSyncStart.
	void GgpoOnVsync();
	// In VMManager::Execute, after the CPU returned.
	void GgpoOnExecuteReturned();
	// In Pad::SetControllerState: true when the host input was taken (session running).
	bool GgpoCaptureHostInput(u32 controller, u32 bind, float value);

	// Z battle net HLE (#31 step 4). The EE recompiler calls OnNetRpc when the game enters its
	// net RPC wrapper (NET_RPC_PC: fno in a0, request header + data at 0xc22c9c).
	// ZDXSV_NET_TRACE=<file>: log every battle send (fno 0x10, sock 0) and its parsed key slots.
	constexpr u32 NET_RPC_PC = 0x30e380;
	extern bool g_net_hook; // the recompiler emits the OnNetRpc call at NET_RPC_PC
	void OnNetRpc();
	// Lobby battles (net=1,lobby=1): our GGPO UDP port for the lobby's platform info, 0 = off.
	int GgpoLobbyPort();
	// advertise=P (test): the port the platform info announces instead, at 127.0.0.1; 0 = off.
	int GgpoLobbyAdvertisePort();
	// The lobby's battle info (DEV9 thread): ok = every other player has a GGPO address;
	// byPosition = GGPO address candidates (IPv4 / IPv6, Zdxsv::GgpoPeers) per battle position, own
	// position empty; session = ggpo_session (0 = none: stays on the battle server), pingMs = ggpo_ping_ms.
	// Without delay=, starts the ping test (Zdxsv::StartPingTest) on our GGPO port; GGPO arms after it
	// ended, at the candidate it picked per peer; else (or nothing answered) at the first candidate.
	// ids = report lines naming the battle ("battle_code=..\nuser_id=..\n"); players = user id and name
	// per position (network status OSD); relays = the battle info's relay servers (pinged by the ping test,
	// registered with GGPO in order; a peer is reached through one when the ping test picked it).
	void SetLobbyPeers(bool ok, std::vector<std::vector<Zdxsv::PeerAddr>> byPosition, u32 session, int pingMs, std::string ids,
		std::vector<std::pair<std::string, std::string>> players, std::vector<Zdxsv::BattleInfo::Relay> relays = {});
	// The last lobby battle's P2PMatchingReport body ("key=value" lines: result=ggpo/cut/server, ping test
	// rtt and address per peer, delay, frames, close reason), then cleared; "" if none. The lobby connection
	// sends it (0x9952) after the next platform info, as flycast after its next login.
	std::string TakeLobbyReport();
	// Replay play (ZDXSV_REPLAY): seek by `frames` from the current frame (hotkeys "Zdxsv Replay: Seek
	// Back / Forward 10 s"), done at the next frame start; at the end (paused) it plays on. No-op otherwise.
	void ReplaySeekBy(int frames);

	// Network status OSD (as flycast's drawNetworkStat) while a net session runs: input delay, rollback
	// frames, frames waited for a peer, predicted frames; per opponent: position, user id, name, GGPO
	// ping or "disconnected". Built once per frame on the CPU thread; read by the GS thread's ImGui
	// overlay. Empty when no session runs or with ZDXSV_GGPO osd=0. color = IM_COL32 value.
	struct GgpoOsdLine
	{
		std::string text;
		u32 color;
	};
	std::vector<GgpoOsdLine> GgpoOsdLines();
} // namespace Zdxsv

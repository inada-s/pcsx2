// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/InputHooks.h"
#include "Zdxsv/Lobby.h"
#include "Zdxsv/UiHooks.h"
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// zdxsv GGPO: while a session runs, the CPU leaves Execute() at every vsync, so a
// frame is the unit GGPO works in (like flycast's emu.run()). Between frames: save (delta state),
// maybe roll back (load + rerun frames inside the advance_frame callback), then the synced pad
// inputs of the next frame are written to both pads. Host pad input is held back meanwhile.
namespace Zdxsv
{
	// Flags and hook entry points: Zdxsv/CpuHooks.h, InputHooks.h, UiHooks.h.

	// ZDXSV_NET_TRACE=<file>: log every battle send (fno 0x10, sock 0) and its parsed key slots.
	// Lobby battles (net=1,lobby=1): our GGPO UDP port for the lobby's platform info, 0 = off.
	int GgpoLobbyPort();
	// advertise=P (test): the port the platform info announces instead, at 127.0.0.1; 0 = off.
	int GgpoLobbyAdvertisePort();
	// The lobby's battle info (DEV9 thread): ok = every other player has a GGPO address;
	// byPosition = GGPO address candidates (IPv4 / IPv6, Zdxsv::GgpoPeers) per battle position, own
	// position empty; session = ggpo_session (0 = none: the battle connection is cut), pingMs = ggpo_ping_ms.
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
	// Replay play (ZDXSV_REPLAY): start frames of the rounds played so far (control bar marks).
	std::vector<int> ReplayRoundStarts();
	// Replay play: the key display's input runs of the shown position (buttons, held frames); false when off.
	bool ReplayKeys(std::vector<std::pair<u16, int>>& runs);

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

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

// zdxsv GGPO: while a session runs, the CPU leaves Execute() at every vsync, so a frame is the unit
// GGPO works in. Between frames: save, maybe roll back and rerun, then feed the synced inputs.
namespace Zdxsv
{
	// Flags and hook entry points: Zdxsv/CpuHooks.h, InputHooks.h, UiHooks.h.

	// Lobby battles (net=1,lobby=1): our GGPO UDP port for the lobby's platform info, 0 = off.
	int GgpoLobbyPort();
	// advertise=P (test): the port the platform info announces instead, at 127.0.0.1; 0 = off.
	int GgpoLobbyAdvertisePort();
	// The lobby's battle info (DEV9 thread): ok = every other player has a GGPO address;
	// byPosition = Zdxsv::GgpoPeers; session = ggpo_session (0 = none: the battle connection is cut),
	// pingMs = ggpo_ping_ms. Without delay=, starts the ping test; GGPO arms after it at the path it picked.
	// ids = report lines naming the battle; players = per position (network status OSD, replay header).
	struct LobbyPlayer
	{
		std::string id, name, pilot; // name, pilot: UTF-8, "" if none
	};
	void SetLobbyPeers(bool ok, std::vector<std::vector<Zdxsv::PeerAddr>> byPosition, u32 session, int pingMs, std::string ids,
		std::vector<LobbyPlayer> players, std::vector<Zdxsv::BattleInfo::Relay> relays = {});
	// The last lobby battle's P2PMatchingReport body ("key=value" lines), then cleared; "" if none.
	// Sent to the lobby (0x9952) after the next platform info.
	std::string TakeLobbyReport();
	// Replay play (ZDXSV_REPLAY): start frames of the rounds played so far (control bar marks).
	std::vector<int> ReplayRoundStarts();
	// Replay play: per round, W / L / D (draw) for the shown position's team; the file's round_data, else the rounds played so far.
	std::string ReplayRoundResults();
	// Replay play: the key display's input runs of the shown position (buttons, held frames); false when off.
	bool ReplayKeys(std::vector<std::pair<u16, int>>& runs);

	// Network status OSD lines, built once per frame on the CPU thread for the GS thread's overlay.
	// Empty when no session runs or with osd=0. color = IM_COL32 value.
	struct GgpoOsdLine
	{
		std::string text;
		u32 color;
	};
	std::vector<GgpoOsdLine> GgpoOsdLines();
} // namespace Zdxsv

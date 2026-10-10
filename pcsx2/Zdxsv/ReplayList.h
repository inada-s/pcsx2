// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The replay list (Qt: Tools → zdxsv Replays): local files, the lobby's uploaded replays and live battles.

#include "common/Pcsx2Types.h"

#include <string>
#include <vector>

namespace Zdxsv
{
	inline constexpr const char* GAME_SERIAL = "SLPS-25419";
	inline constexpr u32 GAME_CRC = 0x435D8236; // ELF CRC of SLPS_254.19: the hooks' fixed guest addresses are this build's

	// UI thread, before the VM starts: that VM plays src (a ZDXSV_REPLAY value) from point of view pov (0-based,
	// -1 = the recorder's). ZDXSV_REPLAY / ZDXSV_REPLAY_POV win. Only the next VM start takes it.
	void SetNextReplay(std::string src, int pov);
	// GgpoOnVmInitialize: the SetNextReplay pick, cleared; false = none.
	bool TakeNextReplay(std::string& src, int& pov);

	// <data dir>/replays: where lobby battles save without replay=DIR.
	std::string ReplayDir();

	struct ReplayFileUser
	{
		std::string name, pilot;
		int pos = 0;
	};
	struct ReplayFileInfo
	{
		std::string battle_code;
		s64 start_at = 0; // unix seconds
		int players = 0, rounds = 0, frames = 0;
		std::vector<ReplayFileUser> users;
	};
	// The header fields of a .pb (replay.proto BattleLogFile); false = not readable.
	bool ReadReplayInfo(const std::string& path, ReplayFileInfo* info);

	// [DEV9/Eth] ZdxsvLobbyApiUrl: the lobby's public API (http://host:port, /lbs/replay and /lbs/live); default https://zdxsv.net, "" = none.
	std::string LobbyApiUrl();
	// udp://<API host>:<STUN port>/<code>: the live stream of a /lbs/live battle; "" without an API URL.
	std::string LiveReplaySource(const std::string& battle_code);
} // namespace Zdxsv

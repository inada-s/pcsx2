// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The replay list (Qt: Tools → zdxsv Replays): local files, the lobby's uploaded replays and live battles.

#include "common/Pcsx2Types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Zdxsv
{
	inline constexpr const char* GAME_SERIAL = "SLPS-25419";
	inline constexpr u32 GAME_CRC = 0x435D8236; // ELF CRC of SLPS_254.19: the hooks' fixed guest addresses are this build's

	// UI thread, before the VM starts: that VM plays src (a ZDXSV_REPLAY value) from point of view pov (0-based,
	// -1 = the recorder's). ZDXSV_REPLAY / ZDXSV_REPLAY_POV win. Only the next VM start takes it.
	void SetNextReplay(std::string src, int pov);

	// Translated name of a battle MS id (player work +0x1f2a); nullptr past the known names.
	const char* MsName(u32 id);

	// GgpoOnVmInitialize: the SetNextReplay pick, cleared; false = none.
	bool TakeNextReplay(std::string& src, int& pov);

	// Where lobby battles save without replay=DIR: setting DEV9/Eth ZdxsvReplayPath (relative: to the data dir), else
	// or when it cannot be created <data dir>/replays.
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
	// A .pb as saved and uploaded is gzip encoded; older builds saved it plain, which never starts with the gzip magic.
	bool ReplayIsGzip(const u8* data, size_t size);
	// The gzip encoding of pb; empty on a zlib failure.
	std::vector<u8> ReplayGzip(const std::vector<u8>& pb);
	// Decodes gzip data into out, at most max bytes: 0 = done, 1 = over max, 2 = not gzip data.
	int ReplayGunzip(const u8* data, size_t size, size_t max, std::vector<u8>& out);
	// The header fields of a .pb (replay.proto BattleLogFile), plain: seeking past the others (inputs, states), gzip: decoded
	// whole; any thread. false = not readable.
	bool ReadReplayInfo(const std::string& path, ReplayFileInfo* info);

	// [DEV9/Eth] ZdxsvLobbyApiUrl: the lobby's public API (http://host:port, /lbs/replay and /lbs/live); default the release build's server, "" = none.
	std::string LobbyApiUrl();
	// The LiveSource of a /lbs/live battle at LobbyApiUrl().
	std::string LiveReplaySource(const std::string& battle_code);
} // namespace Zdxsv

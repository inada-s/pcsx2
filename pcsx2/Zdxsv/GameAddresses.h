// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Guest addresses and states of SLPS-25419 that more than one file reads (g_z_game checks serial and CRC).
// Addresses one file uses stay in that file.

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	inline constexpr u32 GAME_GP = 0x6dd770;
	inline constexpr u32 GAME_SCENE_BLOCK = GAME_GP - 0x5ce4; // pointer; byte 0 = scene: 1 title, 2 main menu, 7 network mode
	// u8 network menu state; +1 = its sub state, +2 = the sub state's step.
	inline constexpr u32 GAME_NET_STATE = 0xcd1360;
	inline constexpr u32 GAME_NET_SUB = GAME_NET_STATE + 1;
	inline constexpr u32 GAME_NET_STEP = GAME_NET_STATE + 2;

	enum GameNetState : u8
	{
		GameNetConnectMenu = 3, // broadband, dial-up, last connection
		GameNetSettingList = 6,
		GameNetDnas = 0x21,
		GameNetLoginBrowser = 0x27,
	};
} // namespace Zdxsv

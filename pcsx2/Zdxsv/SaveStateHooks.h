// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the save state code (SaveState.cpp). Only flags and declarations (AGENTS.md, Seams With
// Upstream Code).

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	extern bool g_ggpo_enabled; // ZDXSV_GGPO is set

	// Lobby save states, a debugging feature, off unless ZDXSV_LOBBY_STATE=1:
	// DEV9 state is saved/loaded with save states, and after a load DEV9 adopts
	// TCP connections the PS2 opened before the save. Off = upstream behaviour.
	bool LobbyStateEnabled();
} // namespace Zdxsv

// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the save state code (SaveState.cpp). Only flags and declarations (AGENTS.md, Seams With
// Upstream Code).

#include "common/Pcsx2Defs.h"

// True while SaveState_DeltaLoad (Zdxsv/DeltaFreeze.h) runs: it loads at the point the state was saved
// (vsync, mid rcntUpdate), so freeze functions skip their load-time fix-ups to restore the state exactly.
extern bool g_SaveStateDeltaLoad;
// During a delta save/load: IOP RAM and SPU2 RAM are left out (Zdxsv::DeltaState restores them page by page).
extern bool g_SaveStateDeltaPagedRam;
// Freeze functions: the next size bytes at pos are scratch (recorded only by a delta save).
extern void SaveState_DeltaMarkScratch(size_t pos, size_t size);

namespace Zdxsv
{
	extern bool g_ggpo_enabled; // GGPO options (ZDXSV_GGPO or the ZdxsvGgpo setting) or a replay

	// Lobby save states, a debugging feature, off unless ZDXSV_LOBBY_STATE=1:
	// DEV9 state is saved/loaded with save states, and after a load DEV9 adopts
	// TCP connections the PS2 opened before the save. Off = upstream behaviour.
	bool LobbyStateEnabled();

	// DEV9 state in full and delta states. Only with ZDXSV_LOBBY_STATE=1, ZDXSV_GGPO or ZDXSV_REPLAY;
	// otherwise nothing is written (an empty entry is not added to the zip) and a DEV9.bin in a state
	// is ignored, as upstream. Replay states need it: without it the IOP's SMAP driver (restored) and
	// DEV9 (not) disagree on the next TX buffer, the IOP spins on TXDNV and the game's net RPCs never return.
	inline bool SaveStateDev9Enabled() { return LobbyStateEnabled() || g_ggpo_enabled; }
} // namespace Zdxsv

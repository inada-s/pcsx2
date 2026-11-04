// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the save state code (SaveState.cpp). Only flags and declarations (AGENTS.md, Seams With
// Upstream Code).

#include "common/Pcsx2Defs.h"

class StateWrapper;

// True while SaveState_DeltaLoad (Zdxsv/DeltaFreeze.h) runs: it loads at the point the state was saved
// (vsync, mid rcntUpdate), so freeze functions skip their load-time fix-ups to restore the state exactly.
extern bool g_SaveStateDeltaLoad;
// During a delta save/load: IOP RAM and SPU2 RAM are left out (Zdxsv::DeltaState restores them page by page).
extern bool g_SaveStateDeltaPagedRam;
// Freeze functions: the next size bytes at pos are scratch (recorded only by a delta save).
extern void SaveState_DeltaMarkScratch(size_t pos, size_t size);

namespace Zdxsv
{
	extern bool g_ggpo_enabled; // GGPO options (ZDXSV_GGPO, else on by default) or a replay

	// DEV9 state in full and delta states. Only with ZDXSV_GGPO or ZDXSV_REPLAY; otherwise a DEV9.bin
	// in a state is ignored, as upstream. Replay states need it: the IOP's SMAP driver (restored) and
	// DEV9 must agree on the next TX buffer.
	inline bool SaveStateDev9Enabled() { return g_ggpo_enabled; }

	// No-DEV9 slots (NoDev9.cpp) in full states, on the no-DEV9 path whenever the DEV9 state is kept.
	bool SaveStateNoDev9Enabled();
	bool NoDev9DoState(StateWrapper& sw);
} // namespace Zdxsv

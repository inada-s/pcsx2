// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the UI: hotkeys (Hotkeys.cpp), the replay control bar (ImGuiOverlays.cpp, VMManager.cpp).
// Only flags and declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	extern bool g_ggpo_enabled; // ZDXSV_GGPO is set

	// Replay play (ZDXSV_REPLAY): seek by `frames` from the current frame (hotkeys "Zdxsv Replay: Seek
	// Back / Forward 10 s"), done at the next frame start; at the end (paused) it plays on. No-op otherwise.
	void ReplaySeekBy(int frames);
	// Replay control bar state; false when no replay plays. The actions below run on the CPU thread.
	bool ReplayBarInfo(int& frame, int& frames, int& pov, u32& povs, int& target);
	void ReplaySeekTo(int frame);
	void ReplayTogglePause();
	// Replay play: switch to the next position that has a file (ZDXSV_REPLAY=a.zdxr;b.zdxr) at the current frame.
	void ReplayNextPov();
	// Replay play: key display on / off.
	void ReplayToggleKeys();
	// Replay play: jump to the start of the round `delta` rounds from the current one (round 0 = the briefing).
	void ReplayJumpRound(int delta);
} // namespace Zdxsv

// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the UI: hotkeys (Hotkeys.cpp), the replay control bar (ImGuiOverlays.cpp, VMManager.cpp).
// Only flags and declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	extern bool g_ggpo_enabled; // GGPO options (ZDXSV_GGPO or the ZdxsvGgpo setting) or a replay

	// Replay play (ZDXSV_REPLAY): seek by `frames` from the current frame (hotkeys "Zdxsv Replay: Seek
	// Back / Forward 10 s"), done at the next frame start; at the end (paused) it plays on. No-op otherwise.
	void ReplaySeekBy(int frames);
	// Replay control bar state; false when no replay plays. The actions below run on the CPU thread.
	bool ReplayBarInfo(int& frame, int& frames, int& pov, u32& povs, int& target);
	void ReplaySeekTo(int frame);
	void ReplayTogglePause();
	// Replay play: switch to the next position that has a file (ZDXSV_REPLAY=a.pb;b.pb) at the current frame.
	void ReplayNextPov();
	// Replay play: key display on / off.
	void ReplayToggleKeys();
	// Replay play: jump to the start of the round `delta` rounds from the current one (round 0 = the briefing).
	void ReplayJumpRound(int delta);
	// Replay play: take over the own position from the current frame (hotkey "Take Over"; while taken over:
	// retry from that frame, as START), skip the input matching, cancel it, or go back to the replay.
	void ReplayTakeover();
	void ReplayTakeoverSkip();
	void ReplayTakeoverCancel();
	void ReplayTakeoverReturn();
	// Takeover state for the overlay (GS thread): phase 0 off, 1 matching, 2 countdown, 3 taken over; target /
	// current = (B) bits of the replay's input at the frame / of the host pad. False when no replay plays.
	bool ReplayTakeoverInfo(int& phase, u16& target, u16& current, float& countdown);
	// Paused replay (VMManager idle, CPU thread): the takeover's input matching and countdown.
	void ReplayTakeoverIdle();

	// GGPO network status, replay keys and replay control bar (ImGuiManager::RenderOverlays). GS thread.
	void DrawOverlays(float scale, float margin, float spacing);
} // namespace Zdxsv

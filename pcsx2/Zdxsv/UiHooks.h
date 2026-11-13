// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the UI: hotkeys (Hotkeys.cpp), the replay control bar (ImGuiOverlays.cpp, VMManager.cpp).
// Only flags and declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

class Error;

namespace Zdxsv
{
	extern bool g_ggpo_enabled; // GGPO options (ZDXSV_GGPO, else on by default) or a replay
	extern bool g_ggpo_active; // a session runs
	extern bool g_replay_child; // this process only plays a replay or live battle for its parent: no play time

	// VMManager pause, frame advance, speed change and state load: true (refused) during a net battle, the
	// reason in `error`, else an OSD notice. A paused or fast peer stalls every other peer of the battle.
	bool GgpoBattleRefuses(Error* error = nullptr);
	// FullscreenUI::OpenPauseMenu (hotkey "Open Pause Menu", default Escape): true (refused, OSD notice) during
	// a net battle or while the game is connected to the zdxsv lobby, where the menu opens by accident.
	bool OnlinePauseMenuRefuses();

	// True: the upstream features zdxsv limits are off ([DEV9/Eth] ZdxsvFeatureLimits).
	bool FeaturesLimited();

	extern bool g_online_limits;
	// The reason goes to `error`, else to the OSD.
	bool OnlineRefuses(Error* error = nullptr);
	bool OnlineNowAnyThread();
	bool OnlineBattleNowAnyThread();
	bool OnlineRefusesReset();

	// Replay play (ZDXSV_REPLAY): seek by `frames` from the current frame (hotkeys "Zdxsv Replay: Seek
	// Back / Forward 10 s"), done at the next frame start; at the end (paused) it plays on. No-op otherwise.
	void ReplaySeekBy(int frames);
	// Replay control bar state; false when no replay plays. The actions below run on the CPU thread.
	bool ReplayBarInfo(int& frame, int& frames, int& pov, u32& povs, int& target);
	// The shown round's frames [lo, hi) and its index of `rounds` (round -1: the whole replay, no rounds known).
	bool ReplayBarSpan(int& lo, int& hi, int& round, int& rounds);
	int ReplaySpeedPercent();
	bool ReplayCanTakeOver();
	// Playing: seek `dir` seconds; paused: a one-frame step.
	void ReplayStep(int dir);
	// VMManager::SetState: true = leave the SPU2 output paused (g_replay_step_hold).
	bool ReplayStepHoldsAudio(bool paused);
	void ReplaySpeedBy(int delta);
	u32 ReplayPadPokes();
	// Under g_ggpo_active (VMManager): state saves and loads and disc changes.
	bool ReplayRefuses(Error* error = nullptr);
	void ReplaySeekTo(int frame);
	void ReplayTogglePause();
	// Replay play: key display on / off.
	void ReplayToggleKeys();
	// Replay play: jump to the start of the round `delta` rounds from the current one (round 0 = the briefing; with
	// skipping on and the file's rounds, round 1 = the first round's briefing).
	void ReplayJumpRound(int delta);
	// Replay play: take over the own position from the current frame (hotkey "Take Over"; while taken over:
	// retry from that frame, as START), cancel the countdown, or go back to the replay.
	void ReplayTakeover();
	void ReplayTakeoverCancel();
	void ReplayTakeoverReturn();
	// Takeover state for the overlay (GS thread): phase 0 off, 2 countdown, 3 taken over; target / current = (B) bits
	// of the replay's input at the frame / of the host pad. False when no replay plays.
	bool ReplayTakeoverInfo(int& phase, u16& target, u16& current, float& countdown);
	// Paused replay (VMManager idle, CPU thread): the host pad's replay controls and the takeover countdown.
	void ReplayTakeoverIdle();
	// The paused idle poll runs at the fullscreen UI rate: a step from the pad or the control bar waits for it.
	bool ReplayIdleFastPoll();
	// Idle presents slower than the display refresh: faster ones queue up on the GS thread, so the next step's
	// picture is shown late and a state load waits for the queue.
	bool ReplayIdlePresentDue();

	// GGPO network status, replay keys and replay control bar (ImGuiManager::RenderOverlays). GS thread.
	void DrawOverlays(float scale, float margin, float spacing);
} // namespace Zdxsv

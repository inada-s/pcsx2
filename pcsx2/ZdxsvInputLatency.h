// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// zdxsv debug: pad input latency measurement. Off unless the environment
// variable ZDXSV_INPUT_LATENCY is set (see ZdxsvInputLatency.cpp for its keys).
// Injects button presses into pad 1 and timestamps each stage:
// press (virtual host event) -> host poll applies it -> SIO2 pad read returns
// it to the game -> watched EE RAM byte changes -> the frame pushed at that
// vsync is presented. Without addr= it searches EE RAM for the byte the
// presses move (e.g. a menu cursor) instead.

#pragma once

#include "common/Pcsx2Types.h"

namespace ZdxsvInputLatency
{
	extern bool g_enabled;

	// CPU thread, every vsync, after the host input poll.
	void OnVsync();
	// CPU thread, SIO2 pad poll that returns the button word to the game (active low).
	void OnPadPoll(u8 unifiedSlot, u32 buttons);
	// GS thread, after a vsync's frame was presented.
	void OnPresent();
} // namespace ZdxsvInputLatency

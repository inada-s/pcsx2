// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the pad code (SIO/Pad). Only flags and declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	extern bool g_ggpo_active; // a session runs

	// In Pad::SetControllerState: true when the host input was taken (session running).
	bool GgpoCaptureHostInput(u32 controller, u32 bind, float value);

	extern bool g_input_latency_enabled; // ZDXSV_INPUT_LATENCY is set (debug, Zdxsv/InputLatency.h)

	// CPU thread, SIO2 pad poll that returns the button word to the game (active low).
	void InputLatencyOnPadPoll(u8 unifiedSlot, u32 buttons);
} // namespace Zdxsv

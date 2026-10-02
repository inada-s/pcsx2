// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include <cstdio>

// zdxsv GGPO (#31 step 3): while a session runs, the CPU leaves Execute() at every vsync, so a
// frame is the unit GGPO works in (like flycast's emu.run()). Between frames: save (delta state),
// maybe roll back (load + rerun frames inside the advance_frame callback), then the synced pad
// inputs of the next frame are written to both pads. Host pad input is held back meanwhile.
namespace ZdxsvGgpo
{
	extern bool g_enabled; // ZDXSV_GGPO is set
	extern bool g_active; // a session runs
	extern bool g_in_rollback; // rerunning frames: no throttle
	extern std::FILE* g_trace; // trace= probe: open while frames near the traced one run

	// In VSyncStart.
	void OnVsync();
	// In VMManager::Execute, after the CPU returned.
	void OnExecuteReturned();
	// In Pad::SetControllerState: true when the host input was taken (session running).
	bool CaptureHostInput(u32 controller, u32 bind, float value);
} // namespace ZdxsvGgpo

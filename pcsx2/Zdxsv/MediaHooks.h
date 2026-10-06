// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the video and audio output: GSRenderer.cpp (GS thread), SPU2 spu2.cpp (CPU thread).
// Only flags and declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	extern bool g_ggpo_active; // a session runs
	extern bool g_ggpo_in_rollback; // rerunning frames

	// GS thread: the vsync packet being handled ends a rollback rerun frame (MTGS::PostVsyncStart copies
	// g_ggpo_in_rollback into the packet). GSRenderer::VSync skips its present, as for a duplicate frame:
	// a rollback of N frames shows the corrected frame once, not N + 1 frames.
	extern bool g_gs_rerun_frame;

	// spu2Output while a session runs: counts the sample; true = drop it (a rerun frame), so a rollback
	// does not play its frames' sound again.
	bool SpuOnOutput();
} // namespace Zdxsv

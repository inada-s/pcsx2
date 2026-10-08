// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Shared by the EE recompiler hook files (RecHooks.cpp, RecProbe.cpp); not a hook header.

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	// ZDXSV_EE_PROBE / ZDXSV_EE_WATCH calls before the instruction at pc (RecProbe.cpp).
	void RecEmitProbes(u32 pc);
} // namespace Zdxsv

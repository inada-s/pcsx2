// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the PINE server (PINE.cpp). Only flags and declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	// PINEServer::ParseCommand, PINE thread: true (the request fails) while the Z game is online, unless
	// ZDXSV_PINE_ONLINE=1 (OnlineLimits.cpp).
	bool PineRefuses();
} // namespace Zdxsv

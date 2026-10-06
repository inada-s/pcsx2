// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// zdxsv debug: pad input latency measurement. Off unless the environment
// variable ZDXSV_INPUT_LATENCY is set (see Zdxsv/InputLatency.cpp for its keys).
// Injects button presses into pad 1 and timestamps each stage:
// press (virtual host event) -> host poll applies it -> SIO2 pad read returns
// it to the game -> watched EE RAM byte changes -> the frame pushed at that
// vsync is presented. Without addr= it searches EE RAM for the byte the
// presses move (e.g. a menu cursor) instead.

#pragma once

// g_enabled and the hook entry points: Zdxsv/CpuHooks.h, Zdxsv/InputHooks.h.
#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/InputHooks.h"

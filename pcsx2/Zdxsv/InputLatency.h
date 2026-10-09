// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Debug: pad input latency measurement (ZDXSV_INPUT_LATENCY). Presses a button on pad 1 and timestamps each stage:
// press, host poll, SIO2 pad read, EE RAM change, present. Without addr= it searches EE RAM for the byte the presses
// change.

#pragma once

// g_enabled and the hook entry points: Zdxsv/CpuHooks.h, Zdxsv/InputHooks.h.
#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/InputHooks.h"

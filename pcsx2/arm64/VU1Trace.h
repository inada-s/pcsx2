// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Calls between the VU1 interpreter and the ARM64 VU1 trace recompiler (VU1Trace.inl).

// Runs one trace from VU1's TPC within the cycle budget; false when the caller must step instead.
bool _vu1TraceStep(u64 startcycles, u32 cycles);
void _vu1TraceClear();
void _vu1TraceForget();
// The end of a program, after the delay slot of an E-bit pair.
void _vu1EndProgram();

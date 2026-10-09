// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <utility>
#include <vector>

// zdxsv delta state: everything but EE RAM and the GS thread, in memory. CPU thread, vsync.
// Used by Zdxsv/DeltaState.cpp; g_SaveStateDeltaLoad, SaveState_DeltaMarkScratch: Zdxsv/SaveStateHooks.h.
bool SaveState_DeltaSave(std::vector<u8>& buffer);
bool SaveState_DeltaLoad(const std::vector<u8>& buffer);
std::string SaveState_DeltaDescribe(const std::vector<u8>& buffer, size_t offset);
// Mean wall ms per section of the delta saves and loads so far.
std::string SaveState_DeltaTimes();
// Byte ranges (offset, size) of the last SaveState_DeltaSave holding values no later frame reads
// as saved (decode scratch, MTGS-thread counters reset on load): left out of hashes and compares.
const std::vector<std::pair<size_t, size_t>>& SaveState_DeltaScratch();

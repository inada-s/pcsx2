// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <utility>
#include <vector>

namespace Zdxsv
{
	// zdxsv delta state: everything but EE RAM and the GS thread, in memory. CPU thread, vsync.
	// Used by Zdxsv/DeltaState.cpp; g_delta_freeze_load, DeltaFreezeMarkScratch: Zdxsv/SaveStateHooks.h.
	bool DeltaFreezeSave(std::vector<u8>& buffer);
	bool DeltaFreezeLoad(const std::vector<u8>& buffer);
	std::string DeltaFreezeDescribe(const std::vector<u8>& buffer, size_t offset);
	// Mean wall ms per section of the delta saves and loads so far.
	std::string DeltaFreezeTimes();
	void DeltaFreezeTimesReset();
	// Byte ranges (offset, size) of the last DeltaFreezeSave holding values no later frame reads
	// as saved (decode scratch, MTGS-thread counters reset on load): left out of hashes and compares.
	const std::vector<std::pair<size_t, size_t>>& DeltaFreezeScratch();
	// On: delta states leave out IOP RAM and SPU2 RAM (Zdxsv::DeltaState tracks their pages).
	void DeltaFreezeSetPagedRam(bool on);
	// Writes part of IOP RAM, clearing recompiled IOP code over the words that differ.
	void DeltaFreezeRestoreIopPage(u32 offset, const u8* data, u32 size);
} // namespace Zdxsv

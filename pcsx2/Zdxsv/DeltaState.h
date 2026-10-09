// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "Zdxsv/CpuHooks.h"

#include <string>
#include <vector>

// zdxsv delta state: fast per-frame save/load of the VM for rollback (GGPO).
// EE, IOP and SPU2 RAM are copy-on-write (EE: vtlb mmap_Delta*): a save keeps only the old data
// of the pages written since the previous save. The rest (SaveState_DeltaSave) is copied whole.
// GS thread state is not saved. CPU thread only, at the same point every frame (vsync).
namespace Zdxsv
{
	// Frames must be saved in increasing order (after a DeltaStateLoad(f), the next save is > f).
	bool DeltaStateSave(int frame);
	// frame must be saved; saved frames after it are dropped.
	bool DeltaStateLoad(int frame);
	// Drops the saved frames before frame, except the newest.
	void DeltaStateDiscardBefore(int frame);
	// The state of a saved frame without EE, IOP and SPU2 RAM, or nullptr.
	const std::vector<u8>* DeltaStateGetState(int frame);
	// Hash of the state of the last saved frame, without its scratch bytes (SaveState_DeltaScratch).
	u64 DeltaStateHash(const std::vector<u8>& state);
	// Hash per page of EE RAM, then IOP RAM, then SPU2 RAM (not in the states: see DeltaState.cpp).
	void DeltaStateHashRam(std::vector<u64>& pages);
	// "EE 0x..." / "IOP 0x..." / "SPU2 0x..." for an index of DeltaStateHashRam.
	std::string DeltaStateRamPageName(size_t i);
	// Zeroes the scratch bytes in a copy of the state of the last saved frame, for compares.
	void DeltaStateMaskScratch(std::vector<u8>& state);
	// Mean wall ms of the parts of DeltaStateSave so far.
	std::string DeltaStateTimes();
	// Drops everything and stops EE RAM tracking.
	void DeltaStateClear();

	// g_delta_state_test_enabled, DeltaStateOnVsync: Zdxsv/CpuHooks.h.
} // namespace Zdxsv

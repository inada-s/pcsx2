// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <vector>

// zdxsv delta state (#31): fast per-frame save/load of the VM for rollback (GGPO).
// EE RAM is copy-on-write (vtlb mmap_Delta*): a save keeps only the old data of the pages
// written since the previous save. The rest (SaveState_DeltaSave) is copied whole.
// GS thread state is not saved. CPU thread only, at the same point every frame (vsync).
namespace ZdxsvDeltaState
{
	// Frames must be saved in increasing order (after a Load(f), the next save is > f).
	bool Save(int frame);
	// frame must be saved; saved frames after it are dropped.
	bool Load(int frame);
	// Drops the saved frames before frame, except the newest.
	void DiscardBefore(int frame);
	// The state of a saved frame without EE RAM, or nullptr.
	const std::vector<u8>* GetState(int frame);
	// Hash of the state of the last saved frame, without its scratch bytes (SaveState_DeltaScratch).
	u64 HashState(const std::vector<u8>& state);
	// Zeroes the scratch bytes in a copy of the state of the last saved frame, for compares.
	void MaskScratch(std::vector<u8>& state);
	// Mean wall ms of the parts of Save so far.
	std::string Times();
	// Drops everything and stops EE RAM tracking.
	void Clear();

	// The EE/IOP recompilers end a block where the next PC is already compiled, so block ends
	// (and the cycles at which events are tested) depend on the code cache history, which a
	// rollback does not restore. When set, blocks end only at branches and page splits.
	extern bool g_fixed_blocks; // EE and IOP

	// ZDXSV_DELTA_TEST: synctest in a running game, see ZdxsvDeltaState.cpp.
	extern bool g_test_enabled;
	void OnVsync();
} // namespace ZdxsvDeltaState

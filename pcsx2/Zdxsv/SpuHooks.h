// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	// Stopped voices that skip mixing until their decode could reach the next block (SpuLazyVoices.cpp).
	struct SpuLazyVoices
	{
		u32 mask[2];
		u32 from[2][24];
		u32 wake[2][24];
		u32 hold[2][24];
	};
	extern SpuLazyVoices g_spu_lazy;

	void SpuLazyWakeAll();
	void SpuLazyCatchUp(u32 core, u32 voice, u32 upto);
	bool SpuLazyTrySleep(u32 core, u32 voice, u32 cycles);
	void SpuLazyReset();

	// Before anything outside the mixer reads or writes SPU2 state.
	static __fi void SpuLazyWake()
	{
		if (g_spu_lazy.mask[0] | g_spu_lazy.mask[1])
			SpuLazyWakeAll();
	}

	inline void SpuLazyOnFreeze(bool load)
	{
		if (load)
			SpuLazyReset();
		else
			SpuLazyWake();
	}

	// In the mixer, per stopped voice and tick: true = skip this voice's step.
	static __fi bool SpuLazySkip(u32 core, u32 voice, u32 cycles)
	{
		if (g_spu_lazy.mask[core] & (1u << voice))
		{
			if (cycles != g_spu_lazy.wake[core][voice])
				return true;
			SpuLazyCatchUp(core, voice, cycles);
			return false;
		}
		if (g_spu_lazy.hold[core][voice])
		{
			g_spu_lazy.hold[core][voice]--;
			return false;
		}
		return SpuLazyTrySleep(core, voice, cycles);
	}
} // namespace Zdxsv

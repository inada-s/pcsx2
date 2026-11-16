// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// A stopped voice's steps read only its own registers and its current block header. Such a voice skips
// mixing until its decode could reach the next block, and is caught up with the same steps before
// anything outside the mixer reads or writes SPU2 state.

#include "Zdxsv/SpuHooks.h"

#include "SPU2/defs.h"

#include <algorithm>
#include <bit>

namespace Zdxsv
{
	SpuLazyVoices g_spu_lazy = {};

	namespace
	{
		bool LazyVoiceAllowed(u32 coreidx, u32 voiceidx)
		{
			const V_Voice& vc(Cores[coreidx].Voices[voiceidx]);
			// Voices 1 and 3 write their output to SPU RAM; a modulated voice reads the previous voice.
			if (Cores[0].IRQEnable || Cores[1].IRQEnable || voiceidx == 1 || voiceidx == 3 || (vc.Modulated && voiceidx != 0))
				return false;

			// Reverb and the core output areas below SPU2_DYN_MEMLINE are written every tick.
			const u32 block = vc.NextA & 0xFFFF8;
			if (block < SPU2_DYN_MEMLINE)
				return false;

			for (int i = 0; i < 2; i++)
			{
				const u32 start = Cores[i].EffectsStartA & 0x3f'ffff;
				const u32 end = (Cores[i].EffectsEndA & 0x3f'ffff) | 0xffff;
				if (end > 0xFFFFF || (block + 7 >= start && block <= end))
					return false;
			}
			return true;
		}

		u32 LazyVoiceTicks(const V_Voice& vc)
		{
			const s32 pitch = std::min<s32>(vc.Pitch, 0x3FFF);
			if (pitch <= 0)
				return 0;

			const s32 decodes_to_next_block = 8 - (vc.NextA & 7);
			const s64 need = static_cast<s32>(vc.DecPosWrite - vc.DecPosRead) - 12 + 4 * (decodes_to_next_block - 1);
			if (need <= 0)
				return 0;

			const s64 ticks = ((need << 12) - vc.SP + pitch - 1) / pitch;
			return static_cast<u32>(std::min<s64>(ticks, 0x10000));
		}
	} // namespace

	bool SpuLazyTrySleep(u32 core, u32 voice, u32 cycles)
	{
		// A voice that cannot sleep now is retried after a delay; a missed sleep only costs speed.
		const u32 ticks = LazyVoiceTicks(Cores[core].Voices[voice]);
		if (!LazyVoiceAllowed(core, voice))
		{
			g_spu_lazy.hold[core][voice] = std::clamp<u32>(ticks, 1, 64) - 1;
			return false;
		}
		if (ticks < 2)
		{
			g_spu_lazy.hold[core][voice] = Cores[core].Voices[voice].Pitch <= 0 ? 63 : 0;
			return false;
		}
		g_spu_lazy.mask[core] |= 1u << voice;
		g_spu_lazy.from[core][voice] = cycles;
		g_spu_lazy.wake[core][voice] = cycles + ticks;
		return true;
	}

	void SpuLazyCatchUp(u32 core, u32 voice, u32 upto)
	{
		V_Voice& vc(Cores[core].Voices[voice]);
		u32 n = upto - g_spu_lazy.from[core][voice];
		g_spu_lazy.mask[core] &= ~(1u << voice);
		if (n == 0)
			return;

		if (vc.Volume.Left.Enable || vc.Volume.Right.Enable)
		{
			for (; n > 0; n--)
				spu2StepStoppedVoice(core, voice);
			return;
		}

		// Without a volume slide, IRQ or block change, the steps repeat the same header read and only move
		// the decode and pitch counters.
		spu2UpdateBlockHeader(core, voice);
		const s32 pitch = std::min<s32>(vc.Pitch, 0x3FFF);
		u32 write = vc.DecPosWrite;
		u32 read = vc.DecPosRead;
		u32 decodes = 0;
		s32 sp = vc.SP;
		for (; n > 0; n--)
		{
			if (static_cast<int>(write - read) <= 12)
			{
				write += 4;
				decodes++;
			}
			sp += pitch;
			read += sp >> 12;
			sp &= 0xfff;
		}
		vc.DecPosWrite = write;
		vc.DecPosRead = read;
		vc.SP = sp;
		vc.NextA = (vc.NextA + decodes) & 0xFFFFF;
	}

	void SpuLazyWakeAll()
	{
		for (u32 c = 0; c < 2; c++)
		{
			while (g_spu_lazy.mask[c])
				SpuLazyCatchUp(c, std::countr_zero(g_spu_lazy.mask[c]), Cycles + 1);
		}
	}

	void SpuLazyReset()
	{
		g_spu_lazy = {};
	}
} // namespace Zdxsv

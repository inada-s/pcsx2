// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/Settings.h"

#include "Memory.h"

#include "common/Console.h"

#include <iterator>

namespace Zdxsv
{
	namespace
	{
		struct Word
		{
			u32 addr;
			u32 original;
			u32 patched;
		};

		// SLPS-25419 (g_z_game checks serial and CRC). The game keeps two draw-list slots, chosen by the
		// frame flag u32 0x7ae1c0. Stock: the main loop sends slot[flag ^ 1] (last frame's list) before
		// the game logic, which then builds this frame's list into slot[flag]. Patched: the send moves
		// after the logic and takes slot[flag], so a list is drawn in the frame that built it.
		constexpr Word kCode[] = {
			// 0x1246f0, list send: clear slot[flag ^ 1], send slot[flag]
			{0x124724, 0x300200ff, 0x38a50001},
			{0x12474c, 0xac620370, 0xac600370},
			{0x124750, 0xac62036c, 0xac60036c},
			{0x124754, 0xac620368, 0xac600368},
			{0x12475c, 0x38430001, 0x38430000},
			// 0x21c6f0, VIF1 DMA end handler: the remaining chains and the buffer of slot[flag]
			{0x21c728, 0x38a50001, 0x38a50000},
			{0x21c9f0, 0x38630001, 0x38630000},
			// main loop: no send before the logic; after it, send and wait on kCave instead of 0x1249a0
			{0x224c00, 0x0c0491bc, 0x00000000},
			{0x224cb0, 0x00000000, 0x00000000},
			{0x224cb4, 0x00000000, 0x0c0491bc},
			{0x224cb8, 0x0c049268, 0x24040001},
			{0x224cbc, 0x24040001, 0x0c03c000},
			{0x224cc0, 0x00000000, 0x24040001},
			{0x224cc4, 0x00000000, 0x00000000},
			{0x224cc8, 0x00000000, 0x1440fffc},
			{0x224ccc, 0x1440fffa, 0x00000000},
			{0x224cd0, 0x00000000, 0x00000000},
		};

		// Free RAM in this game: a copy of the list wait 0x1249a0 that checks slot[flag]. The game zeroes
		// it during boot, after its code is loaded.
		constexpr u32 kCave = 0xf0000;
		constexpr u32 kCaveCode[] = {
			0x27bdfff0, 0x3c02007b, 0xffbf0000, 0x3c01007b, 0x8c23e1c0, 0x2442e21c, 0x38640000, 0x00041900,
			0x00642023, 0x00041880, 0x00641823, 0x000318c0, 0x00431021, 0x8c420000, 0x14400005, 0x3c011001,
			0x8c229000, 0x30420100, 0x10400003, 0x00000000, 0x10000005, 0x24020001, 0x0000202d, 0x0c040334,
			0x0000282d, 0x0000102d, 0xdfbf0000, 0x03e00008, 0x27bd0010,
		};

		struct State
		{
			bool patched = false; // the game code matched once: from then on, words that read their original value are patched
		} s_state;

		bool Original()
		{
			for (const Word& w : kCode)
			{
				if (memRead32(w.addr) != w.original)
					return false;
			}
			for (u32 i = 0; i < std::size(kCaveCode); i++)
			{
				if (memRead32(kCave + i * 4) != 0)
					return false;
			}
			return true;
		}

		// Through the vtlb, so recompiled blocks of the word are dropped.
		void Patch(u32 addr, u32 original, u32 patched)
		{
			if (memRead32(addr) == original)
				memWrite32(addr, patched);
		}
	} // namespace

	bool g_single_draw_list = false;

	void SingleDrawListOnVmInitialize()
	{
		g_single_draw_list = g_z_game && BoolSetting("ZdxsvSingleDrawList", true, "ZDXSV_SINGLE_DRAW_LIST");
		s_state = {};
		if (g_single_draw_list)
			Console.WriteLn("Zdxsv: single draw list on");
	}

	void SingleDrawListReset()
	{
		s_state = {};
	}

	void SingleDrawListOnVsync()
	{
		if (!s_state.patched)
		{
			if (!Original())
				return;
			s_state.patched = true;
			Console.WriteLn("Zdxsv: single draw list patched");
		}
		for (const Word& w : kCode)
			Patch(w.addr, w.original, w.patched);
		for (u32 i = 0; i < std::size(kCaveCode); i++)
			Patch(kCave + i * 4, 0, kCaveCode[i]);
	}
} // namespace Zdxsv

// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/CpuHooks.h"

#include "Host.h"
#include "Memory.h"

#include "common/SettingsInterface.h"

#include <cstdlib>

namespace Zdxsv
{
	namespace
	{
		// Guest addresses per game (g_z_game / g_aeug_game check serial and CRC).
		// SLPS-25305: static matches of the SLPS-25419 code, needs proper testing.
		struct AllyHpGame
		{
			u32 scene_block; // pointer; +0x52 = the own player's index
			u32 players; // player structs, player_size bytes each
			u32 player_size;
			u32 max_hp; // s16
			u32 hp; // s16
			u32 ally_hp; // s16 that no game code reads or writes: the ally HP for the HUD
			u32 team; // u8
			u32 phase; // u8: 6 battle, 7 round or game end
			// The two HUD functions that draw the own HP as "HP / max", each reading max HP twice:
			// lh $v1|$a1, max_hp($s1), $s1 = the own player's struct.
			u32 max_hp_loads[4];
		};
		constexpr AllyHpGame kGvszGame = {0x6dd770 - 0x5ce4, 0x839330, 0x2200, 0x1f0, 0x1f2, 0x1ee, 0x21d5, 0xc627b4,
			{0x14dbc4, 0x14dc1c, 0x15ac34, 0x15ac8c}};
		constexpr AllyHpGame kAeugGame = {0x5b8e70 - 0x5d74, 0x7b6af0, 0x2100, 0x1e0, 0x1e2, 0x1de, 0x20d5, 0xb1f184,
			{0x1495d4, 0x14962c, 0x155154, 0x1551ac}};
		const AllyHpGame& Game() { return g_z_game ? kGvszGame : kAeugGame; }
		constexpr u32 kRamSize = 0x2000000;

		struct State
		{
			bool enabled = true;
		} s_state;

		// ZDXSV_ALLY_HP=0 / =1 over the setting.
		bool Enabled(bool setting)
		{
			const char* e = std::getenv("ZDXSV_ALLY_HP");
			return e ? e[0] != '0' : setting;
		}

		bool IsMaxHpLoad(u32 word, u32 offset)
		{
			return (word & 0xffe0ffff) == (0x86200000 | offset) && ((word >> 16) & 0x1f) != 0;
		}

		// Through the vtlb, so recompiled blocks of the word are dropped.
		void SetLoadOffset(const AllyHpGame& g, u32 offset)
		{
			for (u32 addr : g.max_hp_loads)
			{
				const u32 word = memRead32(addr);
				if ((word & 0xffff) != offset && (IsMaxHpLoad(word, g.max_hp) || IsMaxHpLoad(word, g.ally_hp)))
					memWrite32(addr, (word & 0xffff0000) | offset);
			}
		}

		// The own player's struct and its teammate's; false outside a battle or without a teammate.
		bool Players(const AllyHpGame& g, u32* own, u32* ally)
		{
			const u8 phase = memRead8(g.phase);
			if (phase != 6 && phase != 7)
				return false;
			const u32 scene = memRead32(g.scene_block);
			if (scene == 0 || scene >= kRamSize)
				return false;
			const u8 me = memRead8(scene + 0x52);
			if (me >= 4)
				return false;
			*own = g.players + g.player_size * me;
			const u8 team = memRead8(*own + g.team);
			for (u32 p = 0; p < 4; p++)
			{
				const u32 other = g.players + g.player_size * p;
				if (p != me && memRead8(other + g.team) == team && static_cast<s16>(memRead16(other + g.max_hp)) > 0)
				{
					*ally = other;
					return true;
				}
			}
			return false;
		}
	} // namespace

	void AllyHpLoadSettings(SettingsInterface& si)
	{
		s_state.enabled = Enabled(si.GetBoolValue("DEV9/Eth", "ZdxsvAllyHp", true));
	}

	void AllyHpOnVmInitialize()
	{
		s_state.enabled = Enabled(Host::GetBoolSettingValue("DEV9/Eth", "ZdxsvAllyHp", true));
	}

	void AllyHpOnVsync()
	{
		const AllyHpGame& g = Game();
		u32 own, ally;
		if (s_state.enabled && Players(g, &own, &ally))
		{
			const s16 hp = static_cast<s16>(memRead16(ally + g.hp));
			memWrite16(own + g.ally_hp, static_cast<u16>(hp < 0 ? 0 : hp));
			SetLoadOffset(g, g.ally_hp);
		}
		else
		{
			SetLoadOffset(g, g.max_hp);
		}
	}
} // namespace Zdxsv

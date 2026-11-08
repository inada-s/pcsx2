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
		// SLPS-25419 (g_z_game checks serial and CRC).
		constexpr u32 kGp = 0x6dd770;
		constexpr u32 kSceneBlock = kGp - 0x5ce4; // pointer; +0x52 = the own player's index
		constexpr u32 kPlayers = 0x839330; // player structs, 0x2200 bytes each
		constexpr u32 kPlayerSize = 0x2200;
		constexpr u32 kMaxHp = 0x1f0; // s16
		constexpr u32 kHp = 0x1f2; // s16
		// s16 that no game code reads or writes: the ally HP for the HUD.
		constexpr u32 kAllyHp = 0x1ee;
		constexpr u32 kTeam = 0x21d5; // u8
		constexpr u32 kPhase = 0xc627b4; // u8: 6 battle, 7 round or game end
		constexpr u32 kRamSize = 0x2000000;

		// The two HUD functions that draw the own HP as "HP / max", each reading max HP twice:
		// lh $v1|$a1, 0x1f0($s1), $s1 = the own player's struct.
		constexpr u32 kMaxHpLoads[] = {0x14dbc4, 0x14dc1c, 0x15ac34, 0x15ac8c};

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
		void SetLoadOffset(u32 offset)
		{
			for (u32 addr : kMaxHpLoads)
			{
				const u32 word = memRead32(addr);
				if ((word & 0xffff) != offset && (IsMaxHpLoad(word, kMaxHp) || IsMaxHpLoad(word, kAllyHp)))
					memWrite32(addr, (word & 0xffff0000) | offset);
			}
		}

		// The own player's struct and its teammate's; false outside a battle or without a teammate.
		bool Players(u32* own, u32* ally)
		{
			const u8 phase = memRead8(kPhase);
			if (phase != 6 && phase != 7)
				return false;
			const u32 scene = memRead32(kSceneBlock);
			if (scene == 0 || scene >= kRamSize)
				return false;
			const u8 me = memRead8(scene + 0x52);
			if (me >= 4)
				return false;
			*own = kPlayers + kPlayerSize * me;
			const u8 team = memRead8(*own + kTeam);
			for (u32 p = 0; p < 4; p++)
			{
				const u32 other = kPlayers + kPlayerSize * p;
				if (p != me && memRead8(other + kTeam) == team && static_cast<s16>(memRead16(other + kMaxHp)) > 0)
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
		u32 own, ally;
		if (s_state.enabled && Players(&own, &ally))
		{
			const s16 hp = static_cast<s16>(memRead16(ally + kHp));
			memWrite16(own + kAllyHp, static_cast<u16>(hp < 0 ? 0 : hp));
			SetLoadOffset(kAllyHp);
		}
		else
		{
			SetLoadOffset(kMaxHp);
		}
	}
} // namespace Zdxsv

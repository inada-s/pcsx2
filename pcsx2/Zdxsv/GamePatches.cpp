// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// The Z game's patches, built in: pnach files could change what the game computes, so the Z game reads none.

#include "Zdxsv/PatchHooks.h"
#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/ReplayList.h"

namespace Zdxsv
{
	namespace
	{
		// The group name is the one EnableWideScreenPatches turns on. Render values only.
		constexpr const char* PATCH_Z_GAME_PNACH = R"(gametitle=Kidou Senshi Gundam - Gundam vs. Z Gundam (J)(SLPS-25419)

[Widescreen 16:9]
description=Renders the game in 16:9 aspect ratio, instead of 4:3.
gsaspectratio=16:9
patch=1,EE,0026cf84,word,3c023f40
patch=1,EE,20843D30,extended,44bff400
patch=1,EE,20843EF0,extended,443FE7FF
patch=1,EE,208440B0,extended,443FE7FF
)";
	} // namespace

	const char* PatchZGamePnach(u32 crc)
	{
		return g_z_game && crc == GAME_CRC ? PATCH_Z_GAME_PNACH : nullptr;
	}
} // namespace Zdxsv

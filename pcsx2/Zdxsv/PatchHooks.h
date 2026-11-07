// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the patch loader (Patch.cpp). Only flags and declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	// Patch::EnumeratePnachFiles (patch loading and the patch and cheat lists of the settings): while the Z game runs,
	// its ELF CRC `crc` gets this pnach text as its only patches and no cheats; no pnach file is read (patches
	// folder, cheats folder, patches.zip). nullptr: another game or CRC, pnach files as upstream.
	const char* PatchZGamePnach(u32 crc);
} // namespace Zdxsv

// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv hooks of the patch loader (Patch.cpp). Only flags and declarations (AGENTS.md, Seams With Upstream Code).

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	// nullptr: another game or CRC.
	const char* PatchZGamePnach(u32 crc);

	// true: skip the zip without a warning. Releases ship no patches.zip.
	bool PatchesZipMissing(const char* path);
} // namespace Zdxsv

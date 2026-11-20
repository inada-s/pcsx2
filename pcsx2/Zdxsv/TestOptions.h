// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Every ZDXSV_* environment variable is read through TestEnv() (docs/zdxsv/options.md). A Release
// build behaves as if none were set; Devel and Debug builds (PCSX2_DEVBUILD) read them.

#include <cstdlib>
#include <cstring>

namespace Zdxsv
{
	inline const char* TestEnv(const char* name)
	{
#ifdef PCSX2_DEVBUILD
		return std::getenv(name);
#else
		return (std::strncmp(name, "ZDXSV_", 6) == 0 || std::strncmp(name, "GGPO_", 5) == 0) ? nullptr : std::getenv(name);
#endif
	}
} // namespace Zdxsv

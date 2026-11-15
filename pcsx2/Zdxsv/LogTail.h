// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <vector>

namespace Zdxsv
{
	// The last log lines (info level and above) kept in memory whatever the log settings, for crash and
	// match reports. Start before other threads log.
	void LogTailStart();
	void LogTailStop();

	// Oldest first, each "[time] message".
	std::vector<std::string> LogTailLines();

	// For a crashed process: no lock (the crashed thread may hold it), no allocation. Oldest first.
	void LogTailVisitUnlocked(void (*visit)(void* ctx, const char* line, u32 len), void* ctx);
} // namespace Zdxsv

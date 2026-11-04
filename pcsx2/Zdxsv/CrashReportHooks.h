// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	// Call before the CPU thread starts: on Linux a crash handler installed after PageFaultHandler::Install()
	// would take the page faults fastmem relies on. can_ask: a window may be shown to ask for consent.
	void CrashReportsStart(bool can_ask);
	void CrashReportsStop();

	// False in a build without the SDK or a DSN: nothing can be sent, the settings page hides the option.
	bool CrashReportsAvailable();

	// Turning off stops uploads at once. Turning on takes effect now if reports started, else at the next start.
	void CrashReportsSetEnabled(bool enabled);
} // namespace Zdxsv

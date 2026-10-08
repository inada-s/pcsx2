// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Environment options meant for tests and diagnostics (docs/zdxsv/options.md: Use "test", "control",
// "diagnostic"). Every build reads them, so a released build can take part in the rig tests
// (tests/zdxsv, run.py): a battle between a release and a new build checks that they stay in sync.
// Reading them through TestEnv() marks them in the code.

#include <cstdlib>

namespace Zdxsv
{
	inline const char* TestEnv(const char* name) { return std::getenv(name); }
} // namespace Zdxsv

// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Test options that can be abused (AGENTS.md, File and Code Structure): they change game memory or inputs,
// disturb the battle of other players, redirect the updater or write files to a path given from outside.
// They work only in a test build: CMake -DZDXSV_TEST_OPTIONS=ON, MSBuild /p:ZdxsvTestOptions=true.
// Release builds leave it off; there TestEnv() is a constant nullptr and the code behind it is dead.
// The list: docs/zdxsv/options.md, "Test build".

#include <cstdlib>

namespace Zdxsv
{
#ifdef ZDXSV_TEST_OPTIONS
	inline constexpr bool TEST_OPTIONS = true;
	inline const char* TestEnv(const char* name) { return std::getenv(name); }
#else
	inline constexpr bool TEST_OPTIONS = false;
	inline const char* TestEnv(const char*) { return nullptr; }
#endif
} // namespace Zdxsv

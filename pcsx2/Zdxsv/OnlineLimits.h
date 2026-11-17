// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once


#include <string>

struct Pcsx2Config;

namespace Zdxsv
{
	// CPU thread.
	bool OnlineNow();
	bool OnlineBattleNow();
	void OnlineAllowReset();
	// any thread
	bool OnlinePineAllowed();
	// The state the settings were last applied with; OnlineNow can be ahead of it.
	bool OnlineLimitsActive();
	void OnlineLimitsForce(Pcsx2Config& c, std::string* changed);
	void OnlineLimitsReset();
} // namespace Zdxsv

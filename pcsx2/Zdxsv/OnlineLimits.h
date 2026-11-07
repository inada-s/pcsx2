// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// PCSX2 features limited while the Z game is online (OnlineLimits.cpp). Included by zdxsv code only.

#include <string>

struct Pcsx2Config;

namespace Zdxsv
{
	// Connected to the zdxsv lobby or in a net battle (GgpoBattle.cpp). CPU thread.
	bool OnlineNow();
	bool OnlineBattleNow();
	void OnlineAllowReset();
	// The state the settings were last applied with (OnlineLimitsOnVsync). CPU thread.
	bool OnlineLimitsActive();
	// In SyncSettingsEnforce while online: the limited settings off in `c`; names of the changed ones appended to
	// `changed` (nullptr: not wanted).
	void OnlineLimitsForce(Pcsx2Config& c, std::string* changed);
	// VM shutdown and reset: offline.
	void OnlineLimitsReset();
} // namespace Zdxsv

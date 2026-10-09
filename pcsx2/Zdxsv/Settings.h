// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv settings: [DEV9/Eth] Zdxsv* in PCSX2.ini, set on the zdxsv settings page (docs/zdxsv/options.md).
// Order: the environment variable if set, then the setting, then the default.

#include "Host.h"

#include <cstdlib>

namespace Zdxsv
{
	// default of ZdxsvReplayStateUrl: the post-entry state a replay starts from
	inline constexpr const char* REPLAY_STATE_URL = "https://storage.googleapis.com/zdxsv/misc/rbk-p1.p2s";

	// env: "0" = off, any other value = on
	inline bool BoolSetting(const char* key, bool default_value, const char* env)
	{
		if (const char* e = std::getenv(env))
			return e[0] != '0';
		return Host::GetBoolSettingValue("DEV9/Eth", key, default_value);
	}
} // namespace Zdxsv

// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv settings: [DEV9/Eth] Zdxsv* in PCSX2.ini, set on the zdxsv settings page (docs/zdxsv/options.md).
// Order: the environment variable if set, then the setting, then the default.

#include "Host.h"
#include "Zdxsv/TestOptions.h"

#include <cstdlib>
#include <string>

namespace Zdxsv
{
	// default of ZdxsvReplayStateUrl: the post-entry state a replay starts from
	inline constexpr const char* REPLAY_STATE_URL = "https://storage.googleapis.com/zdxsv/misc/rbk-p1.p2s";
	// SHA-256 of the file at REPLAY_STATE_URL: a new file there needs a new client
	inline constexpr const char* REPLAY_STATE_SHA256 = "bd6db1b5ed73e3f8321161601a89846ea5fce125b3cf194e6cf938ef30f72fdb";
	// default of ZdxsvUdpTestUrl: the udptest function (zdxsv infra/udptest)
	const char* UdpTestUrlDefault();

	// the host the game's server names are looked up as; "" = none
	const char* ServerHost();

	// the zdxsv public API + path; "" without one
	std::string ServerApiUrl(const char* path);

	// env: "0" = off, any other value = on
	inline bool BoolSetting(const char* key, bool default_value, const char* env)
	{
		if (const char* e = TestEnv(env))
			return e[0] != '0';
		return Host::GetBoolSettingValue("DEV9/Eth", key, default_value);
	}
} // namespace Zdxsv

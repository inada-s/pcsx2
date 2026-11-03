// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv settings: [DEV9/Eth] Zdxsv* in PCSX2.ini, set on the zdxsv settings page (docs/zdxsv/options.md).
// Order: the environment variable if set, then the setting, then the default.

#include "Host.h"

#include <cstdlib>
#include <string>

// Set only by the release build (CMake ZDXSV_SERVER_HOST, ZDXSV_UDPTEST_URL): a build from source connects to
// no server until the player sets one.
#ifndef ZDXSV_SERVER_HOST
#define ZDXSV_SERVER_HOST ""
#endif
#ifndef ZDXSV_UDPTEST_URL
#define ZDXSV_UDPTEST_URL ""
#endif

namespace Zdxsv
{
	// default of ZdxsvReplayStateUrl: the post-entry state a replay starts from
	inline constexpr const char* REPLAY_STATE_URL = "https://storage.googleapis.com/zdxsv/misc/rbk-p1.p2s";
	// the host the game's server names are looked up as
	inline constexpr const char* SERVER_HOST = ZDXSV_SERVER_HOST;
	// default of ZdxsvUdpTestUrl: the udptest function (zdxsv infra/udptest)
	inline constexpr const char* UDPTEST_URL = ZDXSV_UDPTEST_URL;

	// https://SERVER_HOST:8443<path>; "" without a server host
	inline std::string ServerApiUrl(const char* path)
	{
		return SERVER_HOST[0] ? std::string("https://") + SERVER_HOST + ":8443" + path : std::string();
	}

	// env: "0" = off, any other value = on
	inline bool BoolSetting(const char* key, bool default_value, const char* env)
	{
		if (const char* e = std::getenv(env))
			return e[0] != '0';
		return Host::GetBoolSettingValue("DEV9/Eth", key, default_value);
	}
} // namespace Zdxsv

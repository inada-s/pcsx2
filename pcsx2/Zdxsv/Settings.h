// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// zdxsv settings: [DEV9/Eth] Zdxsv* in PCSX2.ini, set on the zdxsv settings page (docs/zdxsv/options.md).
// Order: the environment variable if set, then the setting, then the default.

#include "Host.h"

#include <cstdlib>
#include <string>

// The release build sets them (CMake ZDXSV_SERVER_HOST, ZDXSV_API_URL, ZDXSV_UDPTEST_URL): a build from source
// connects to a local zdxsv (its scripts/run-local.sh).
#ifndef ZDXSV_SERVER_HOST
#define ZDXSV_SERVER_HOST "127.0.0.1"
#endif
#ifndef ZDXSV_API_URL
#define ZDXSV_API_URL "http://127.0.0.1:8203"
#endif
#ifndef ZDXSV_UDPTEST_URL
#define ZDXSV_UDPTEST_URL ""
#endif

namespace Zdxsv
{
	// defaults of ZdxsvReplayStateUrl (SLPS-25419) / ZdxsvAeugReplayStateUrl (SLPS-25305): the post-entry state a replay starts from
	inline constexpr const char* REPLAY_STATE_URL = "https://storage.googleapis.com/zdxsv/misc/rbk-p1.p2s";
	inline constexpr const char* AEUG_REPLAY_STATE_URL = "https://storage.googleapis.com/zdxsv/misc/aeug-p1.p2s";
	// default of ZdxsvUdpTestUrl: the udptest function (zdxsv infra/udptest)
	inline constexpr const char* UDPTEST_URL = ZDXSV_UDPTEST_URL;

	// the host the game's server names are looked up as; "" = none
	inline const char* ServerHost()
	{
		const char* e = std::getenv("ZDXSV_SERVER_HOST");
		return e ? e : ZDXSV_SERVER_HOST;
	}

	// the zdxsv public API + path; "" without one
	inline std::string ServerApiUrl(const char* path)
	{
		const char* e = std::getenv("ZDXSV_API_URL");
		const std::string base = e ? e : ZDXSV_API_URL;
		return base.empty() ? base : base + path;
	}

	// env: "0" = off, any other value = on
	inline bool BoolSetting(const char* key, bool default_value, const char* env)
	{
		if (const char* e = std::getenv(env))
			return e[0] != '0';
		return Host::GetBoolSettingValue("DEV9/Eth", key, default_value);
	}
} // namespace Zdxsv

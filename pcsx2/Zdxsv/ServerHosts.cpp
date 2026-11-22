// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/ServerHosts.h"
#include "Zdxsv/Settings.h"

#include <atomic>

// The release build sets them on this file only (CMake ZDXSV_SERVER_HOST, ZDXSV_API_URL, ZDXSV_UDPTEST_URL), so
// they do not change the other files' compile commands. A build from source connects to a local zdxsv (its
// scripts/run-local.sh).
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
	namespace
	{
		std::atomic<bool> s_used_server_host{false};
	} // namespace

	void ServerHostUsed()
	{
		s_used_server_host = true;
	}

	bool DnsUsedServerHost()
	{
		return s_used_server_host;
	}

	const char* UdpTestUrlDefault()
	{
		return ZDXSV_UDPTEST_URL;
	}

	const char* ServerHost()
	{
		const char* e = TestEnv("ZDXSV_SERVER_HOST");
		return e ? e : ZDXSV_SERVER_HOST;
	}

	std::string ServerApiUrl(const char* path)
	{
		const char* e = TestEnv("ZDXSV_API_URL");
		const std::string base = e ? e : ZDXSV_API_URL;
		return base.empty() ? base : base + path;
	}
} // namespace Zdxsv

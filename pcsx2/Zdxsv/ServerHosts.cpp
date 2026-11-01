// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/Dev9Hooks.h"
#include "Zdxsv/ServerHosts.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

namespace Zdxsv
{
	namespace
	{
		// The zdxsv game server. Its address comes from DNS, so a server move needs no new release.
		// zdxsv.net itself is the website since 2026-10; the game server is lbs.zdxsv.net.
		constexpr const char* kServerHost = "lbs.zdxsv.net";

		// The servers the game connects to: login, DNAS, lobby.
		constexpr const char* kGameHosts[] = {
			"www01.kddi-mmbb.jp",
			"gate1.jp.dnas.playstation.org",
			"ca1202.mmcp6",
			"ca1203.mmcp6",
		};

		// Address the game hosts were mapped to by default host entries.
		constexpr u8 kStaleAddress[4] = {153, 121, 44, 150};

		std::atomic<bool> s_used_server_host{false};

		struct ServerAddresses
		{
			std::mutex mutex;
			std::vector<u32> ips;
		};
		ServerAddresses s_servers;

		bool IsGameHost(const char* url)
		{
			return std::any_of(std::begin(kGameHosts), std::end(kGameHosts),
				[url](const char* host) { return StringUtil::Strcasecmp(url, host) == 0; });
		}
	} // namespace

	const char* DnsLookupName(const char* url)
	{
		if (!IsGameHost(url))
			return url;
		Console.WriteLn("DEV9: DNS: %s looked up as %s", url, kServerHost);
		s_used_server_host = true;
		if (g_z_game)
			HttpsLatencyStart();
		return kServerHost;
	}

	bool DnsUsedServerHost()
	{
		return s_used_server_host;
	}

	bool DnsIsStaleHostEntry(const char* url, const u8* address)
	{
		return IsGameHost(url) && std::memcmp(address, kStaleAddress, sizeof(kStaleAddress)) == 0;
	}

	void DnsOnAnswer(const char* name, const u8* address)
	{
		if (!IsGameHost(name))
			return;
		u32 ip;
		std::memcpy(&ip, address, sizeof(ip));
		std::lock_guard lock(s_servers.mutex);
		if (std::find(s_servers.ips.begin(), s_servers.ips.end(), ip) == s_servers.ips.end())
			s_servers.ips.push_back(ip);
	}

	bool IsServerAddress(u32 ip)
	{
		std::lock_guard lock(s_servers.mutex);
		return std::find(s_servers.ips.begin(), s_servers.ips.end(), ip) != s_servers.ips.end();
	}
} // namespace Zdxsv

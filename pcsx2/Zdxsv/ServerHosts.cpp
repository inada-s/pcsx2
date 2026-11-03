// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/ServerHosts.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace Zdxsv
{
	namespace
	{
		std::atomic<bool> s_used_server_host{false};

		struct ServerAddresses
		{
			std::mutex mutex;
			std::vector<u32> ips;
		};
		ServerAddresses s_servers;
	} // namespace

	void ServerHostUsed()
	{
		s_used_server_host = true;
	}

	bool DnsUsedServerHost()
	{
		return s_used_server_host;
	}

	void ServerAddressAdd(u32 ip)
	{
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

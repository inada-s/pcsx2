// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/ServerHosts.h"

#include <atomic>

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
} // namespace Zdxsv

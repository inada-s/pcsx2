// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/Dev9Hooks.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace Zdxsv
{
	namespace
	{
		// The zdxsv server. Its address comes from DNS, so a server move needs no new release.
		constexpr const char* kServerHost = "zdxsv.net";

		// The servers the game connects to: login, DNAS, lobby.
		constexpr const char* kGameHosts[] = {
			"www01.kddi-mmbb.jp",
			"gate1.jp.dnas.playstation.org",
			"ca1202.mmcp6",
			"ca1203.mmcp6",
		};

		// Address the game hosts were mapped to by default host entries.
		constexpr u8 kStaleAddress[4] = {153, 121, 44, 150};

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
		HttpsLatencyStart();
		return kServerHost;
	}

	bool DnsIsStaleHostEntry(const char* url, const u8* address)
	{
		return IsGameHost(url) && std::memcmp(address, kStaleAddress, sizeof(kStaleAddress)) == 0;
	}
} // namespace Zdxsv

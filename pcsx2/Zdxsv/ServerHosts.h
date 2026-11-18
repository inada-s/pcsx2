// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace Zdxsv
{
	// The game's hosts resolve to the build's ServerHost() (no ZDXSV_SERVER_HOST env): the game plays on
	// the zdxsv server of the build.
	void ServerHostUsed();
	bool DnsUsedServerHost();
} // namespace Zdxsv

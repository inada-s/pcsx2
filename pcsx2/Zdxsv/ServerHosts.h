// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	// The game's hosts resolve to the built-in zdxsv server (no ZDXSV_NODEV9 address): the game plays on
	// the public server.
	void ServerHostUsed();
	bool DnsUsedServerHost();
	// ip (network order) is a zdxsv server: the game's hosts resolved to it.
	bool IsServerAddress(u32 ip);
	void ServerAddressAdd(u32 ip);
} // namespace Zdxsv

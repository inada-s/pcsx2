// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	// A game host was looked up as the zdxsv server (no host entry for it): the game plays on the public server.
	bool DnsUsedServerHost();
	// ip (network order) was a DNS answer for a game host: a zdxsv server.
	bool IsServerAddress(u32 ip);
} // namespace Zdxsv

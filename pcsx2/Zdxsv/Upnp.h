// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <string>

namespace Zdxsv
{
	// [DEV9/Eth] ZdxsvUPnP: map the GGPO port when the connectivity test finds it closed.
	bool UpnpEnabled();

	// Maps UDP `port` on the router to this host (lease 1 day, removed at exit). Blocks up to ~2 s
	// (discovery). False: error says why.
	bool UpnpMapUdp(uint16_t port, std::string& error);

	// The LAN address the router maps to; empty before a successful discovery.
	std::string UpnpLocalAddress();

	// Removes every mapping this process added.
	void UpnpUnmapAll();
} // namespace Zdxsv

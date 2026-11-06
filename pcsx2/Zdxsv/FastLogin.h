// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace Zdxsv
{
	// The login API's result is in guest RAM and the game was sent to the lobby connect (net state 0x27).
	bool FastLoginApplied();
	// Fast login sent the game back to the connection menu (no session); cleared by FastLoginReset.
	bool FastLoginWentBack();
} // namespace Zdxsv

// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

namespace Zdxsv
{
	// App start, ZDXSV_AUTH_SIGNIN_TEST: "1" (or a carry-over code) signs in once and logs the start URL instead of
	// opening a browser; "signout" signs out.
	void AuthSignInFromEnv();
} // namespace Zdxsv

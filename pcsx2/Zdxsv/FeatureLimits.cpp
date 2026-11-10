// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/UiHooks.h"

#include "Host.h"

namespace Zdxsv
{
	bool FeaturesLimited()
	{
		return Host::GetBoolSettingValue("DEV9/Eth", "ZdxsvFeatureLimits", true);
	}
} // namespace Zdxsv

// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/UiHooks.h"

#include "Host.h"

#include "common/SettingsInterface.h"

namespace Zdxsv
{
	namespace
	{
		constexpr const char* FEATURE_LIMITS_SECTION = "DEV9/Eth";
		constexpr const char* FEATURE_LIMITS_KEY = "ZdxsvFeatureLimits";
	} // namespace

	bool FeaturesLimited()
	{
		return Host::GetBoolSettingValue(FEATURE_LIMITS_SECTION, FEATURE_LIMITS_KEY, true);
	}

	bool FeaturesLimitedLocked()
	{
		return Host::GetSettingsInterface()->GetBoolValue(FEATURE_LIMITS_SECTION, FEATURE_LIMITS_KEY, true);
	}
} // namespace Zdxsv

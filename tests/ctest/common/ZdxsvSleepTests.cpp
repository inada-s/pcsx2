// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "common/HostSys.h"
#include "common/Pcsx2Defs.h"
#include "common/Threading.h"
#include <gtest/gtest.h>

TEST(ZdxsvSleep, SleepUntilWaitsForTheTarget)
{
	const u64 period = GetTickFrequency() / 50; // 20 ms
	for (int i = 0; i < 5; i++)
	{
		const u64 target = GetCPUTicks() + period;
		Threading::SleepUntil(target);
		// SleepUntil sleeps once and does not spin; the OS may wake it up to about 1 ms early on a timer slack.
		EXPECT_GE(GetCPUTicks() + GetTickFrequency() / 1000, target);
	}
}

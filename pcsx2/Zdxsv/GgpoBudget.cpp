// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// The rollback budget: the part of the one-way latency the input delay leaves to rollbacks, learned per PC
// from the frame slack of past battles (docs/zdxsv/rollback.md).

#include "Zdxsv/GgpoShared.h"
#include "Zdxsv/TestOptions.h"

#include "common/Console.h"
#include "common/Timer.h"

#include "Host.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace Zdxsv
{
	namespace
	{
		constexpr float kBudgetMax = 1.5f;
		constexpr float kBudgetDefault = 1.0f;
		constexpr int kBudgetMinFrames = 1800;
		constexpr double kSlackStep = 0.05;
		constexpr int kSlackBuckets = 201;
		constexpr double kFrameMs = 1000.0 / 59.94;
		// The learned budget stays unused until rollbacks cost less on PS2.
		constexpr bool kBudgetUsed = false;
		constexpr const char* kBudgetKey = "ZdxsvRollbackBudget";

		struct BudgetState
		{
			std::mutex mtx;
			std::array<int, kSlackBuckets> hist{};
			int frames = 0;
			bool have_prev = false;
			Common::Timer::Value prev = 0;
			double prev_rerun = 0, prev_wait = 0, prev_sleep = 0;
		};
		BudgetState s_budget;
	} // namespace

	float GgpoBudgetLearned()
	{
		return std::clamp(Host::GetBaseFloatSettingValue("DEV9/Eth", kBudgetKey, kBudgetDefault), 0.0f, kBudgetMax);
	}

	float GgpoBudgetUsed()
	{
		const char* force = TestEnv("ZDXSV_ROLLBACK_BUDGET");
		return (kBudgetUsed || (force && std::strcmp(force, "1") == 0)) ? GgpoBudgetLearned() : 0.0f;
	}

	int GgpoBudgetDelay(int rtt, int min_delay, float budget)
	{
		return std::max(min_delay, rtt > 0 ? static_cast<int>(std::ceil(rtt / 32.0 - budget)) : 0);
	}

	// A frame starts and ends where the frame loop hands back to the VM; the sums are the session's running
	// totals of rollback reruns, net waits and limiter sleeps.
	void GgpoBudgetFrame(bool sample, double rerun_ms, double wait_ms, double sleep_ms)
	{
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		std::lock_guard lock(s_budget.mtx);
		if (sample && s_budget.have_prev)
		{
			const double rerun = rerun_ms - s_budget.prev_rerun, wait = wait_ms - s_budget.prev_wait;
			const double busy = Common::Timer::ConvertValueToMilliseconds(now - s_budget.prev) - (sleep_ms - s_budget.prev_sleep);
			const double slack = kFrameMs - (busy - rerun);
			const double emulation = busy - wait - rerun;
			if (emulation > 0)
			{
				const double fits = std::max(0.0, slack) / emulation;
				s_budget.hist[std::min<int>(kSlackBuckets - 1, static_cast<int>(fits / kSlackStep))]++;
				s_budget.frames++;
			}
		}
		s_budget.have_prev = sample;
		s_budget.prev = now;
		s_budget.prev_rerun = rerun_ms;
		s_budget.prev_wait = wait_ms;
		s_budget.prev_sleep = sleep_ms;
	}

	// The session's budget is the rollback depth that fits in a frame, minus one frame; the setting moves
	// halfway to it.
	void GgpoBudgetSessionEnd(bool learn)
	{
		std::array<int, kSlackBuckets> hist;
		int frames;
		{
			std::lock_guard lock(s_budget.mtx);
			hist = s_budget.hist;
			frames = s_budget.frames;
			s_budget.hist.fill(0);
			s_budget.frames = 0;
			s_budget.have_prev = false;
		}
		if (!learn || frames < kBudgetMinFrames)
			return;
		double fits = 0;
		for (int i = 0, below = 0; i < kSlackBuckets; i++)
		{
			below += hist[i];
			if (below * 2 >= frames)
			{
				fits = (i + 0.5) * kSlackStep;
				break;
			}
		}
		const float old = GgpoBudgetLearned();
		const float session = std::clamp(static_cast<float>(fits - 1), 0.0f, kBudgetMax);
		const float updated = 0.5f * old + 0.5f * session;
		Host::SetBaseFloatSettingValue("DEV9/Eth", kBudgetKey, updated);
		Host::CommitBaseSettingChanges();
		Console.WriteLn("ZdxsvGgpo: rollback budget: fits %.2f frames (%d frames), budget %.2f -> %.2f, used %.2f",
			fits, frames, old, updated, GgpoBudgetUsed());
	}
} // namespace Zdxsv

// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Replay benchmark (ZDXSV_REPLAY_BENCH): the rollback cycle of a GGPO battle on a replay's frames. Every frame from
// `at` is saved as a delta state; after each `depth` played frames the first of them is loaded and the frames run
// again as rollback reruns. Logs wall ms of a played frame, a rerun frame, a save and a load.

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/DeltaState.h"
#include "Zdxsv/GgpoShared.h"
#include "Zdxsv/TestOptions.h"

#include "common/Console.h"
#include "common/Timer.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace Zdxsv
{
	namespace
	{
		struct ReplayBenchState
		{
			int at = -1;
			int depth = 0;
			int reps = 0;
			int from = -1;
			int rep = 0;
			bool timing = false;
			double inner_ms = 0;
			Common::Timer::Value t_frame = 0;
			std::vector<double> played, rerun, save, load;
		} s_bench;

		void ReplayBenchPart(const char* name, std::vector<double> v)
		{
			if (v.empty())
				return;
			std::sort(v.begin(), v.end());
			double sum = 0;
			for (const double x : v)
				sum += x;
			const auto q = [&v](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))]; };
			Console.WriteLn("ZdxsvGgpo: replay bench %s: n %zu mean %.3f median %.3f p10 %.3f p90 %.3f max %.3f ms",
				name, v.size(), sum / v.size(), q(0.5), q(0.1), q(0.9), v.back());
		}

		void ReplayBenchEnd(const char* why)
		{
			Console.WriteLn("ZdxsvGgpo: replay bench %s: at %d depth %d, %d of %d rollbacks", why, s_bench.at, s_bench.depth,
				s_bench.rep, s_bench.reps);
			ReplayBenchPart("played frame", s_bench.played);
			ReplayBenchPart("rerun frame", s_bench.rerun);
			ReplayBenchPart("save", s_bench.save);
			ReplayBenchPart("load", s_bench.load);
			DeltaStateClear();
			s_bench = {};
		}

		void ReplayBenchRollback(int next)
		{
			Common::Timer timer;
			RerunKeepBeforeLoad(true);
			const bool ok = DeltaStateLoad(s_bench.from);
			RerunKeepAfterLoad();
			s_bench.load.push_back(timer.GetTimeMilliseconds());
			if (!ok)
			{
				ReplayBenchEnd("load failed");
				return;
			}
			s_rb = s_net_at[s_bench.from & 127];
			const int tail = g_single_draw_list ? 0 : 2;
			for (int f = s_bench.from; f < next; f++)
			{
				const int left = next - 1 - f;
				PlayFrame(f);
				g_rerun_vu1_skip = left >= tail - (g_single_draw_list ? 0 : 1);
				g_rerun_draw_skip = left >= tail;
				g_ggpo_in_rollback = true;
				timer.Reset();
				const bool ran = GgpoRunFrame();
				if (g_rerun_draw_skip)
					RerunKeepAfterRerunFrame(left == 0);
				g_rerun_vu1_skip = g_rerun_draw_skip = false;
				g_ggpo_in_rollback = false;
				s_bench.rerun.push_back(timer.GetTimeMilliseconds());
				if (!ran)
				{
					ReplayBenchEnd("rerun stopped");
					return;
				}
			}
			s_bench.rep++;
			s_bench.from = next;
		}
	} // namespace

	void ReplayBenchStart()
	{
		s_bench = {};
		const char* e = TestEnv("ZDXSV_REPLAY_BENCH");
		if (!e || std::sscanf(e, "%d:%d:%d", &s_bench.at, &s_bench.depth, &s_bench.reps) != 3 || s_bench.at < 1 ||
			s_bench.depth < 1 || s_bench.depth > 60 || s_bench.reps < 1)
		{
			s_bench = {};
			return;
		}
		s_bench.from = s_bench.at;
		Console.WriteLn("ZdxsvGgpo: replay bench: from frame %d, %d rollbacks of %d frames", s_bench.at, s_bench.reps, s_bench.depth);
	}

	void ReplayBenchSave(int f)
	{
		if (s_bench.at < 0 || f < s_bench.at || DeltaStateGetState(f))
			return;
		Common::Timer timer;
		if (!DeltaStateSave(f))
		{
			ReplayBenchEnd("save failed");
			return;
		}
		const double ms = timer.GetTimeMilliseconds();
		s_bench.save.push_back(ms);
		s_bench.inner_ms += ms;
		DeltaStateDiscardBefore(f - s_bench.depth - 4);
	}

	void ReplayBenchNext(int next)
	{
		if (s_bench.at < 0)
			return;
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (s_bench.timing)
			s_bench.played.push_back(Common::Timer::ConvertValueToMilliseconds(now - s_bench.t_frame) - s_bench.inner_ms);
		s_bench.timing = false;
		if (next < s_bench.at)
			return;
		if (next == s_bench.from + s_bench.depth)
			ReplayBenchRollback(next);
		else if (next < s_bench.from || next > s_bench.from + s_bench.depth)
			ReplayBenchEnd("left the frames");
		if (s_bench.at >= 0 && s_bench.rep >= s_bench.reps)
			ReplayBenchEnd("done");
		if (s_bench.at < 0)
			return;
		s_bench.timing = true;
		s_bench.inner_ms = 0;
		s_bench.t_frame = Common::Timer::GetCurrentValue();
	}

	void ReplayBenchReset()
	{
		if (s_bench.at >= 0)
			DeltaStateClear();
		s_bench = {};
	}
} // namespace Zdxsv

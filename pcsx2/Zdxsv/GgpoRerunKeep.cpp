// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// Rerun keep: EE RAM ranges a rollback does not roll back (docs/zdxsv/rollback.md).

#include "Zdxsv/GgpoShared.h"

namespace Zdxsv
{
	namespace
	{
		// Per-player bone matrix slots (4 x 0x65d0), written only by the render callbacks. A draw reads
		// the previous frame's slot, so rerun frames that skip the callbacks would leave a stale pose.
		constexpr u32 BONE_SLOTS = 0x8afda0, BONE_SLOTS_END = BONE_SLOTS + 4 * 0x65d0;

		struct RerunKeepConfig
		{
			std::vector<std::pair<u32, u32>> ranges;
			bool any_phase = false; // ranges from ZDXSV_RERUN_KEEP: kept outside battle play too
		};

		// ZDXSV_RERUN_KEEP=0: off; =lo-hi,... (hex, end exclusive): these ranges instead of the bone slots.
		const RerunKeepConfig s_keep_config = [] {
			RerunKeepConfig c;
			const char* e = TestEnv("ZDXSV_RERUN_KEEP");
			if (!e)
			{
				c.ranges.emplace_back(BONE_SLOTS, BONE_SLOTS_END);
				return c;
			}
			c.any_phase = true;
			while (*e)
			{
				char* end;
				const unsigned long lo = std::strtoul(e, &end, 16);
				if (end == e || *end != '-')
					break;
				const unsigned long hi = std::strtoul(end + 1, &end, 16);
				if (lo < hi && hi <= Ps2MemSize::MainRam)
					c.ranges.emplace_back(static_cast<u32>(lo), static_cast<u32>(hi));
				if (*end != ',')
					break;
				e = end + 1;
			}
			return c;
		}();

		struct RerunKeep
		{
			std::vector<u8> saved;
			bool armed = false;
			bool logged = false;
		};
		RerunKeep s_keep;

		bool RerunKeepPhase()
		{
			return s_keep_config.any_phase || eeMem->Main[TICK_STATE] == TICK_PLAY;
		}
	} // namespace

	void RerunKeepBeforeLoad(bool callbacks_skipped)
	{
		s_keep.armed = callbacks_skipped && !s_keep_config.ranges.empty() && RerunKeepPhase();
		if (!s_keep.armed)
			return;
		s_keep.saved.clear();
		for (const auto& [lo, hi] : s_keep_config.ranges)
			s_keep.saved.insert(s_keep.saved.end(), &eeMem->Main[lo], &eeMem->Main[hi]);
	}

	void RerunKeepAfterLoad()
	{
		if (!s_keep.armed || !RerunKeepPhase())
			return;
		s_keep.armed = false;
		size_t off = 0;
		for (const auto& [lo, hi] : s_keep_config.ranges)
		{
			std::memcpy(&eeMem->Main[lo], &s_keep.saved[off], hi - lo);
			off += hi - lo;
		}
		if (!s_keep.logged)
		{
			s_keep.logged = true;
			Console.WriteLn("ZdxsvGgpo: rerun keep %zu ranges", s_keep_config.ranges.size());
		}
	}
} // namespace Zdxsv

// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/LogTail.h"

#include "common/Console.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace Zdxsv
{
	namespace
	{
		constexpr u32 LOG_TAIL_LINES = 100;
		constexpr u32 LOG_TAIL_LINE_SIZE = 512;

		struct LogTailState
		{
			std::mutex mutex;
			char lines[LOG_TAIL_LINES][LOG_TAIL_LINE_SIZE];
			u32 lens[LOG_TAIL_LINES] = {};
			u32 next = 0;
			u32 count = 0;
		};
		LogTailState s_log_tail;

		void LogTap(LOGLEVEL, ConsoleColors, std::string_view message)
		{
			LogTailState& s = s_log_tail;
			std::lock_guard lock(s.mutex);
			char* const line = s.lines[s.next];
			const int n = std::snprintf(line, LOG_TAIL_LINE_SIZE, "[%10.4f] %.*s", Log::GetCurrentMessageTime(),
				static_cast<int>(message.size()), message.data());
			s.lens[s.next] = n < 0 ? 0 : std::min<u32>(static_cast<u32>(n), LOG_TAIL_LINE_SIZE - 1);
			s.next = (s.next + 1) % LOG_TAIL_LINES;
			s.count = std::min<u32>(s.count + 1, LOG_TAIL_LINES);
		}
	} // namespace

	void LogTailStart()
	{
		Log::SetTapOutputLevel(LOGLEVEL_INFO, LogTap);
	}

	void LogTailStop()
	{
		Log::SetTapOutputLevel(LOGLEVEL_NONE, nullptr);
	}

	std::vector<std::string> LogTailLines()
	{
		LogTailState& s = s_log_tail;
		std::lock_guard lock(s.mutex);
		std::vector<std::string> out;
		out.reserve(s.count);
		const u32 first = (s.next + LOG_TAIL_LINES - s.count) % LOG_TAIL_LINES;
		for (u32 i = 0; i < s.count; i++)
		{
			const u32 slot = (first + i) % LOG_TAIL_LINES;
			out.emplace_back(s.lines[slot], s.lens[slot]);
		}
		return out;
	}

	void LogTailVisitUnlocked(void (*visit)(void* ctx, const char* line, u32 len), void* ctx)
	{
		const LogTailState& s = s_log_tail;
		const u32 first = (s.next + LOG_TAIL_LINES - s.count) % LOG_TAIL_LINES;
		for (u32 i = 0; i < s.count; i++)
		{
			const u32 slot = (first + i) % LOG_TAIL_LINES;
			visit(ctx, s.lines[slot], s.lens[slot]);
		}
	}
} // namespace Zdxsv

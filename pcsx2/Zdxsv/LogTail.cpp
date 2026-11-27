// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/LogTail.h"

#include "common/Console.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

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

		bool LogTailStartsWithNoCase(const char* p, std::string_view s)
		{
			for (size_t i = 0; i < s.size(); i++)
			{
				if (std::tolower(static_cast<unsigned char>(p[i])) != std::tolower(static_cast<unsigned char>(s[i])))
					return false;
			}
			return true;
		}

		// A home folder's name is the OS account name: the path segment after a home root becomes '*'.
		u32 LogTailScrubPaths(char* line, u32 len)
		{
			static constexpr std::string_view roots[] = {"\\Users\\", "/Users/", "/home/", "\\Documents and Settings\\"};
			u32 out = 0;
			u32 i = 0;
			while (i < len)
			{
				const std::string_view* root = nullptr;
				for (const std::string_view& r : roots)
				{
					if (len - i >= r.size() && LogTailStartsWithNoCase(line + i, r))
						root = &r;
				}
				if (!root)
				{
					line[out++] = line[i++];
					continue;
				}
				std::memmove(line + out, line + i, root->size());
				out += static_cast<u32>(root->size());
				i += static_cast<u32>(root->size());
				const u32 name = i;
				while (i < len && line[i] != '\\' && line[i] != '/' && line[i] != '"' && line[i] != '\'')
					i++;
				if (i > name)
					line[out++] = '*';
			}
			return out;
		}

		// Upload URLs (also logged by the HTTP downloader) carry the battle's session id, their credential.
		void LogTailScrubSessionIds(char* line, u32 len)
		{
			static constexpr std::string_view key = "session_id=";
			for (u32 i = 0; i + key.size() <= len; i++)
			{
				if (std::memcmp(line + i, key.data(), key.size()) != 0)
					continue;
				u32 end = i + static_cast<u32>(key.size());
				if (end < len && line[end] == '-')
					end++;
				while (end < len && std::isdigit(static_cast<unsigned char>(line[end])))
					end++;
				for (u32 k = i + static_cast<u32>(key.size()); k + 2 < end; k++)
					line[k] = '*';
				i = end - 1;
			}
		}

		void LogTap(LOGLEVEL, ConsoleColors, std::string_view message)
		{
			LogTailState& s = s_log_tail;
			std::lock_guard lock(s.mutex);
			char* const line = s.lines[s.next];
			const int n = std::snprintf(line, LOG_TAIL_LINE_SIZE, "[%10.4f] %.*s", Log::GetCurrentMessageTime(),
				static_cast<int>(message.size()), message.data());
			s.lens[s.next] = LogTailScrubPaths(line, n < 0 ? 0 : std::min<u32>(static_cast<u32>(n), LOG_TAIL_LINE_SIZE - 1));
			LogTailScrubSessionIds(line, s.lens[s.next]);
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

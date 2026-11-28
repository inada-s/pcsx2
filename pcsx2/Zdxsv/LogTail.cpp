// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/LogTail.h"

#include "Config.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <ShlObj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

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
			std::vector<std::string> data_paths;
			std::vector<std::string> account_names;
		};
		LogTailState s_log_tail;

		bool LogTailIsSeparator(char c)
		{
			return c == '\\' || c == '/';
		}

		bool LogTailIsNameChar(char c)
		{
			const unsigned char u = static_cast<unsigned char>(c);
			return u >= 0x80 || std::isalnum(u) || c == '_' || c == '-' || c == '.' || c == '$';
		}

		u32 LogTailSkipSeparators(const char* line, u32 len, u32 i)
		{
			while (i < len && LogTailIsSeparator(line[i]))
				i++;
			return i;
		}

		// A separator in `path` matches a run of separators, so JSON-escaped and mixed-slash spellings match.
		// Returns the end of the match, or 0.
		u32 LogTailMatchPath(const char* line, u32 len, u32 i, std::string_view path)
		{
			for (size_t k = 0; k < path.size();)
			{
				if (LogTailIsSeparator(path[k]))
				{
					if (i >= len || !LogTailIsSeparator(line[i]))
						return 0;
					i = LogTailSkipSeparators(line, len, i);
					while (k < path.size() && LogTailIsSeparator(path[k]))
						k++;
					continue;
				}
				if (i >= len || std::tolower(static_cast<unsigned char>(line[i])) != std::tolower(static_cast<unsigned char>(path[k])))
					return 0;
				i++;
				k++;
			}
			return i;
		}

		// The data folder becomes "<data>". A path segment that is the OS account name, or that follows a home
		// root (Users, home, Documents and Settings), becomes '*': folder names carry the account name.
		u32 LogTailScrubPaths(char* line, u32 len, const std::vector<std::string>& data_paths,
			const std::vector<std::string>& account_names)
		{
			static constexpr std::string_view roots[] = {"Users", "home", "Documents and Settings"};
			char out[LOG_TAIL_LINE_SIZE];
			u32 n = 0;
			const auto emit = [&](std::string_view s) {
				for (const char c : s)
				{
					if (n < LOG_TAIL_LINE_SIZE - 1)
						out[n++] = c;
				}
			};
			u32 i = 0;
			while (i < len)
			{
				const bool segment_start = i > 0 && LogTailIsSeparator(line[i - 1]);
				u32 end = 0;
				for (const std::string& p : data_paths)
				{
					const u32 m = LogTailMatchPath(line, len, i, p);
					if (m && (m == len || !LogTailIsNameChar(line[m])))
						end = std::max(end, m);
				}
				if (end)
				{
					emit("<data>");
					i = end;
					continue;
				}
				if (segment_start)
				{
					for (const std::string& name : account_names)
					{
						const u32 m = LogTailMatchPath(line, len, i, name);
						if (m && (m == len || !LogTailIsNameChar(line[m])))
							end = std::max(end, m);
					}
					if (end)
					{
						emit("*");
						i = end;
						continue;
					}
					bool masked = false;
					for (const std::string_view root : roots)
					{
						const u32 m = LogTailMatchPath(line, len, i, root);
						if (!m || m >= len || !LogTailIsSeparator(line[m]))
							continue;
						const u32 name = LogTailSkipSeparators(line, len, m);
						end = name;
						while (end < len && !LogTailIsSeparator(line[end]) && line[end] != '"' && line[end] != '\'')
							end++;
						if (end > name)
						{
							emit(std::string_view(line + i, name - i));
							emit("*");
							i = end;
							masked = true;
						}
						break;
					}
					if (masked)
						continue;
				}
				emit(std::string_view(line + i, 1));
				i++;
			}
			std::memcpy(line, out, n);
			return n;
		}

		std::string LogTailLastSegment(std::string_view path)
		{
			while (!path.empty() && LogTailIsSeparator(path.back()))
				path.remove_suffix(1);
			const size_t slash = path.find_last_of("\\/");
			return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
		}

		std::vector<std::string> LogTailAccountNames()
		{
			std::vector<std::string> names;
#ifdef _WIN32
			wchar_t user[256];
			DWORD size = std::size(user);
			if (GetUserNameW(user, &size) && size > 1)
				names.push_back(StringUtil::WideStringToUTF8String(std::wstring_view(user, size - 1)));
			PWSTR profile;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)))
			{
				names.push_back(LogTailLastSegment(StringUtil::WideStringToUTF8String(profile)));
				CoTaskMemFree(profile);
			}
#else
			char buf[4096];
			passwd pw;
			passwd* result = nullptr;
			if (getpwuid_r(getuid(), &pw, buf, sizeof(buf), &result) == 0 && result)
			{
				if (result->pw_name)
					names.push_back(result->pw_name);
				if (result->pw_dir)
					names.push_back(LogTailLastSegment(result->pw_dir));
			}
#endif
			std::erase_if(names, [](const std::string& s) { return s.empty(); });
			return names;
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
			s.lens[s.next] = LogTailScrubPaths(line, n < 0 ? 0 : std::min<u32>(static_cast<u32>(n), LOG_TAIL_LINE_SIZE - 1),
				s.data_paths, s.account_names);
			LogTailScrubSessionIds(line, s.lens[s.next]);
			s.next = (s.next + 1) % LOG_TAIL_LINES;
			s.count = std::min<u32>(s.count + 1, LOG_TAIL_LINES);
		}
	} // namespace

	void LogTailStart()
	{
		{
			LogTailState& s = s_log_tail;
			std::lock_guard lock(s.mutex);
			s.data_paths.clear();
			for (const std::string* p : {&EmuFolders::DataRoot, &EmuFolders::Settings})
			{
				std::string path(*p);
				while (!path.empty() && LogTailIsSeparator(path.back()))
					path.pop_back();
				if (!path.empty())
					s.data_paths.push_back(std::move(path));
			}
			s.account_names = LogTailAccountNames();
		}
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

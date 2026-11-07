// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/Zdxsv/CrashReportHooks.h"
#include "pcsx2/Zdxsv/TestOptions.h"

#include "pcsx2/BuildVersion.h"
#include "pcsx2/Config.h"
#include "pcsx2/Host.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>

#ifdef ZDXSV_HAS_SENTRY
#include <sentry.h>
#endif

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// Set only by the release build (CMake ZDXSV_SENTRY_DSN).
#ifndef ZDXSV_SENTRY_DSN
#define ZDXSV_SENTRY_DSN ""
#endif

namespace Zdxsv
{
	namespace
	{
		constexpr const char* CRASH_REPORTS_SECTION = "DEV9/Eth";
		constexpr const char* CRASH_REPORTS_KEY = "ZdxsvCrashReports";

		constexpr u32 LOG_TAIL_LINES = 100;
		constexpr u32 LOG_TAIL_LINE_SIZE = 512;

		struct CrashReportsState
		{
			bool running = false;
			std::mutex log_mutex;
			char log_lines[LOG_TAIL_LINES][LOG_TAIL_LINE_SIZE];
			u32 log_lens[LOG_TAIL_LINES] = {};
			u32 log_next = 0;
			u32 log_count = 0;
#ifdef _WIN32
			std::wstring log_tail_path;
#else
			std::string log_tail_path;
#endif
		};
		CrashReportsState s_crash_reports;

		const char* CrashReportsEnvDsn()
		{
			const char* const dsn = TestEnv("ZDXSV_SENTRY_DSN");
			return (dsn && dsn[0]) ? dsn : nullptr;
		}

		const char* CrashReportsDsn()
		{
			const char* const env_dsn = CrashReportsEnvDsn();
			return env_dsn ? env_dsn : ZDXSV_SENTRY_DSN;
		}

#ifdef ZDXSV_HAS_SENTRY
		void LogTap(LOGLEVEL, ConsoleColors, std::string_view message)
		{
			CrashReportsState& s = s_crash_reports;
			std::lock_guard lock(s.log_mutex);
			char* const line = s.log_lines[s.log_next];
			const int n = std::snprintf(line, LOG_TAIL_LINE_SIZE, "[%10.4f] %.*s", Log::GetCurrentMessageTime(),
				static_cast<int>(message.size()), message.data());
			s.log_lens[s.log_next] = n < 0 ? 0 : std::min<u32>(static_cast<u32>(n), LOG_TAIL_LINE_SIZE - 1);
			s.log_next = (s.log_next + 1) % LOG_TAIL_LINES;
			s.log_count = std::min(s.log_count + 1, LOG_TAIL_LINES);
		}

		// Runs in the crashed process: no lock (the crashed thread may hold it), no allocation.
		void WriteLogTail()
		{
			CrashReportsState& s = s_crash_reports;
			if (s.log_tail_path.empty())
				return;
#ifdef _WIN32
			const HANDLE out = CreateFileW(s.log_tail_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
			if (out == INVALID_HANDLE_VALUE)
				return;
			const auto write = [out](const char* p, u32 n) { DWORD w = 0; WriteFile(out, p, n, &w, nullptr); };
#else
			const int out = open(s.log_tail_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (out < 0)
				return;
			const auto write = [out](const char* p, u32 n) { (void)!::write(out, p, n); };
#endif
			const u32 first = (s.log_next + LOG_TAIL_LINES - s.log_count) % LOG_TAIL_LINES;
			for (u32 i = 0; i < s.log_count; i++)
			{
				const u32 slot = (first + i) % LOG_TAIL_LINES;
				write(s.log_lines[slot], s.log_lens[slot]);
				write("\n", 1);
			}
#ifdef _WIN32
			CloseHandle(out);
#else
			close(out);
#endif
		}

		sentry_value_t CrashReportsOnCrash(const sentry_ucontext_t*, sentry_value_t event, sentry_hint_t*, void*)
		{
			WriteLogTail();
			return event;
		}

		void CrashReportsInit()
		{
			sentry_options_t* const options = sentry_options_new();
			sentry_options_set_dsn(options, CrashReportsDsn());
			sentry_options_set_release(options, (std::string("pcsx2-zdxsv@") + BuildVersion::GitRev).c_str());
			sentry_options_set_environment(options, CrashReportsEnvDsn() ? "development" : "production");
			sentry_options_set_require_user_consent(options, 1);
			sentry_options_set_auto_session_tracking(options, 0);

			const std::string database = Path::Combine(EmuFolders::DataRoot, "sentry");
			// Read when the crash is reported: the settings at that moment.
			const std::string ini = Path::Combine(EmuFolders::Settings, "PCSX2.ini");
			// Written at the crash; a file left from an earlier run must not be sent with a new one.
			const std::string log_tail = Path::Combine(database, "emulog-tail.txt");
			FileSystem::DeleteFilePath(log_tail.c_str());
#ifdef _WIN32
			// Wide paths: a Windows user name can be outside the system code page.
			const std::string handler = Path::Combine(EmuFolders::AppRoot, "crashpad_handler.exe");
			sentry_options_set_handler_pathw(options, StringUtil::UTF8StringToWideString(handler).c_str());
			sentry_options_set_database_pathw(options, StringUtil::UTF8StringToWideString(database).c_str());
			sentry_options_add_attachmentw(options, StringUtil::UTF8StringToWideString(ini).c_str());
			s_crash_reports.log_tail_path = StringUtil::UTF8StringToWideString(log_tail);
			sentry_options_add_attachmentw(options, s_crash_reports.log_tail_path.c_str());
#else
			const std::string handler = Path::Combine(EmuFolders::AppRoot, "crashpad_handler");
			sentry_options_set_handler_path(options, handler.c_str());
			sentry_options_set_database_path(options, database.c_str());
			sentry_options_add_attachment(options, ini.c_str());
			s_crash_reports.log_tail_path = log_tail;
			sentry_options_add_attachment(options, log_tail.c_str());
#endif
			sentry_options_set_on_crash(options, CrashReportsOnCrash, nullptr);

			if (sentry_init(options) != 0)
			{
				Console.Error("Zdxsv: crash reports: the SDK did not start");
				return;
			}

			sentry_user_consent_give();
			s_crash_reports.running = true;
			Log::SetTapOutputLevel(LOGLEVEL_INFO, LogTap);
			Console.WriteLn("Zdxsv: crash reports on");
		}
#endif
	} // namespace

	bool CrashReportsAvailable()
	{
#ifdef ZDXSV_HAS_SENTRY
		return CrashReportsDsn()[0] != '\0';
#else
		return false;
#endif
	}

	void CrashReportsStart()
	{
#ifdef ZDXSV_HAS_SENTRY
		if (!CrashReportsAvailable())
			return;

		const char* const test = TestEnv("ZDXSV_CRASH_REPORT_TEST");
		const bool test_crash = test && test[0] == '1';

		if (!test_crash && !Host::GetBaseBoolSettingValue(CRASH_REPORTS_SECTION, CRASH_REPORTS_KEY, true))
			return;

		CrashReportsInit();

		if (s_crash_reports.running && test_crash)
			*static_cast<volatile int*>(nullptr) = 0;
#endif
	}

	void CrashReportsStop()
	{
#ifdef ZDXSV_HAS_SENTRY
		if (!s_crash_reports.running)
			return;

		Log::SetTapOutputLevel(LOGLEVEL_NONE, nullptr);
		sentry_close();
		s_crash_reports.running = false;
#endif
	}

	void CrashReportsSetEnabled(bool enabled)
	{
#ifdef ZDXSV_HAS_SENTRY
		if (!s_crash_reports.running)
			return;

		if (enabled)
			sentry_user_consent_give();
		else
			sentry_user_consent_revoke();
#else
		(void)enabled;
#endif
	}
} // namespace Zdxsv

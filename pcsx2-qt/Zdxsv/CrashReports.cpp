// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/Zdxsv/CrashReportHooks.h"
#include "pcsx2/Zdxsv/LogTail.h"
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
#include <optional>
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

		struct CrashReportsState
		{
			bool running = false;
			std::string settings_copy_path;
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
		bool CrashReportsSettingsSectionDropped(std::string_view header)
		{
			return header == "[Folders]" || header == "[GameList]" || header == "[Achievements]";
		}

		std::string CrashReportsSettingsText(std::string_view ini)
		{
			std::string out;
			bool dropped_section = false;
			for (const std::string_view raw : StringUtil::SplitString(ini, '\n', false))
			{
				const std::string_view line = StringUtil::StripWhitespace(raw);
				if (line.starts_with('['))
					dropped_section = CrashReportsSettingsSectionDropped(line);
				if (dropped_section)
					continue;
				const size_t eq = line.find('=');
				if (eq != std::string_view::npos && Path::IsAbsolute(StringUtil::StripWhitespace(line.substr(eq + 1))))
					continue;
				out.append(raw);
				out.push_back('\n');
			}
			return out;
		}

		void CrashReportsWriteSettingsCopy()
		{
			const CrashReportsState& s = s_crash_reports;
			if (s.settings_copy_path.empty())
				return;
			const std::optional<std::string> ini =
				FileSystem::ReadFileToString(Path::Combine(EmuFolders::Settings, "PCSX2.ini").c_str());
			if (!FileSystem::WriteStringToFile(s.settings_copy_path.c_str(), CrashReportsSettingsText(ini.value_or(""))))
				Console.Error("Zdxsv: crash reports: could not write the settings copy");
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
			LogTailVisitUnlocked(
				[](void* ctx, const char* line, u32 len) {
					const auto& w = *static_cast<const decltype(write)*>(ctx);
					w(line, len);
					w("\n", 1);
				},
				const_cast<void*>(static_cast<const void*>(&write)));
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
			const std::string ini = Path::Combine(database, "PCSX2.ini");
			s_crash_reports.settings_copy_path = ini;
			FileSystem::EnsureDirectoryExists(database.c_str(), false);
			CrashReportsWriteSettingsCopy();
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
			// Not AppRoot: on macOS that is the folder holding the .app bundle.
			const std::string handler = Path::Combine(Path::GetDirectory(FileSystem::GetProgramPath()), "crashpad_handler");
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
			Console.WriteLn("Zdxsv: crash reports on, kept in %s", database.c_str());
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
		LogTailStart();
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
		LogTailStop();
#ifdef ZDXSV_HAS_SENTRY
		if (!s_crash_reports.running)
			return;

		sentry_close();
		s_crash_reports.running = false;
#endif
	}

	void CrashReportsSettingsSaved()
	{
#ifdef ZDXSV_HAS_SENTRY
		if (s_crash_reports.running)
			CrashReportsWriteSettingsCopy();
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

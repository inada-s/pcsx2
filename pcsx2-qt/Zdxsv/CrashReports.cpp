// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/Zdxsv/CrashReportHooks.h"
#include "pcsx2/Zdxsv/TestOptions.h"

#include "pcsx2/BuildVersion.h"
#include "pcsx2/Config.h"
#include "pcsx2/Host.h"

#include "common/Console.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <QtCore/QCoreApplication>
#include <QtWidgets/QMessageBox>

#include <string>

#ifdef ZDXSV_HAS_SENTRY
#include <sentry.h>
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

		bool CrashReportsAskPlayer()
		{
			const QMessageBox::StandardButton answer = QMessageBox::question(nullptr,
				QCoreApplication::translate("ZdxsvCrashReports", "Send crash reports?"),
				QCoreApplication::translate("ZdxsvCrashReports",
					"When PCSX2 crashes, it can send a crash report to the zdxsv developers so the crash can be fixed.\n\n"
					"A report contains part of the memory of PCSX2 at the moment of the crash, your version of PCSX2, "
					"your operating system and a random ID of this installation. It can contain data of your game "
					"session. Sentry, the service that receives reports, also gets your IP address and can store your "
					"approximate location.\n\n"
					"You can change this later in Settings > zdxsv."));
			return answer == QMessageBox::Yes;
		}

#ifdef ZDXSV_HAS_SENTRY
		void CrashReportsInit()
		{
			sentry_options_t* const options = sentry_options_new();
			sentry_options_set_dsn(options, CrashReportsDsn());
			sentry_options_set_release(options, (std::string("pcsx2-zdxsv@") + BuildVersion::GitRev).c_str());
			sentry_options_set_environment(options, CrashReportsEnvDsn() ? "development" : "production");
			sentry_options_set_require_user_consent(options, 1);
			sentry_options_set_auto_session_tracking(options, 0);

			const std::string database = Path::Combine(EmuFolders::DataRoot, "sentry");
#ifdef _WIN32
			// Wide paths: a Windows user name can be outside the system code page.
			const std::string handler = Path::Combine(EmuFolders::AppRoot, "crashpad_handler.exe");
			sentry_options_set_handler_pathw(options, StringUtil::UTF8StringToWideString(handler).c_str());
			sentry_options_set_database_pathw(options, StringUtil::UTF8StringToWideString(database).c_str());
#else
			const std::string handler = Path::Combine(EmuFolders::AppRoot, "crashpad_handler");
			sentry_options_set_handler_path(options, handler.c_str());
			sentry_options_set_database_path(options, database.c_str());
#endif

			if (sentry_init(options) != 0)
			{
				Console.Error("Zdxsv: crash reports: the SDK did not start");
				return;
			}

			sentry_user_consent_give();
			s_crash_reports.running = true;
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

	void CrashReportsStart(bool can_ask)
	{
#ifdef ZDXSV_HAS_SENTRY
		if (!CrashReportsAvailable())
			return;

		const char* const test = TestEnv("ZDXSV_CRASH_REPORT_TEST");
		const bool test_crash = test && test[0] == '1';

		if (!test_crash)
		{
			if (!Host::ContainsBaseSettingValue(CRASH_REPORTS_SECTION, CRASH_REPORTS_KEY))
			{
				if (!can_ask)
					return;

				Host::SetBaseBoolSettingValue(CRASH_REPORTS_SECTION, CRASH_REPORTS_KEY, CrashReportsAskPlayer());
				Host::CommitBaseSettingChanges();
			}

			if (!Host::GetBaseBoolSettingValue(CRASH_REPORTS_SECTION, CRASH_REPORTS_KEY, false))
				return;
		}

		CrashReportsInit();

		if (s_crash_reports.running && test_crash)
			*static_cast<volatile int*>(nullptr) = 0;
#else
		(void)can_ask;
#endif
	}

	void CrashReportsStop()
	{
#ifdef ZDXSV_HAS_SENTRY
		if (!s_crash_reports.running)
			return;

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

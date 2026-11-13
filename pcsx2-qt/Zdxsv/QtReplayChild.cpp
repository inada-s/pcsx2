// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/QtHooks.h"

#include "MainWindow.h"
#include "QtHost.h"

#include "pcsx2/Config.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/Zdxsv/InputHooks.h"
#include "pcsx2/Zdxsv/UiHooks.h"
#include "pcsx2/Zdxsv/ReplayList.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QMessageBox>

#include <cerrno>
#include <cstdlib>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#else
#include <signal.h>
#endif

namespace Zdxsv
{
	namespace
	{
		constexpr int PARENT_POLL_MS = 1000;

		struct ReplayChildState
		{
			bool child = false;
			std::string src;
			int pov = 0;
			qint64 parent_pid = 0;
			QPointer<QProcess> running;
		};
		ReplayChildState s_replay_child;

		bool ProcessAlive(qint64 pid)
		{
#ifdef _WIN32
			const HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
			if (!h)
				return false;
			const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
			CloseHandle(h);
			return alive;
#else
			return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
		}

		std::string ChildRoot()
		{
			return Path::Combine(EmuFolders::DataRoot, "replay-child");
		}
	} // namespace

	bool QtReplayChildParseArg(const char* arg_cstr)
	{
		const std::string arg(arg_cstr);
		const size_t a = arg.find(':');
		const size_t b = a == std::string::npos ? a : arg.find(':', a + 1);
		if (b == std::string::npos || b + 1 >= arg.size())
			return false;
		char* end = nullptr;
		const long pov = std::strtol(arg.c_str(), &end, 10);
		if (end != arg.c_str() + a || pov < -1 || pov > 3)
			return false;
		const long long pid = std::strtoll(arg.c_str() + a + 1, &end, 10);
		if (end != arg.c_str() + b || pid <= 0)
			return false;
		s_replay_child = {true, arg.substr(b + 1), static_cast<int>(pov), pid, nullptr};
		g_replay_child = true;
		return true;
	}

	bool QtReplayChild()
	{
		return s_replay_child.child;
	}

	void QtReplayChildSettings(INISettingsInterface& si)
	{
		if (!s_replay_child.child)
			return;
		const std::string root = ChildRoot();
		FileSystem::EnsureDirectoryExists(root.c_str(), false);
		// Every save of the parent's settings, read above, goes to the child's own copy.
		si.SetFileName(Path::Combine(root, "PCSX2.ini"));
		si.SetStringValue("Folders", "Cache", Path::Combine(root, "cache").c_str());
		si.SetStringValue("Folders", "Logs", Path::Combine(root, "logs").c_str());
		si.SetStringValue("Folders", "Savestates", Path::Combine(root, "sstates").c_str());
		si.SetStringValue("Folders", "MemoryCards", Path::Combine(root, "memcards").c_str());
		si.SetBoolValue("MemoryCards", "Slot1_Enable", false);
		si.SetBoolValue("MemoryCards", "Slot2_Enable", false);
		si.SetBoolValue("EmuCore", "SaveStateOnShutdown", false);
		si.SetBoolValue("EmuCore", "EnableDiscordPresence", false);
		si.SetBoolValue("Achievements", "Enabled", false);
		si.SetBoolValue("AutoUpdater", "CheckAtStartup", false);
		si.SetBoolValue("UI", "ConfirmShutdown", false);
		si.SetBoolValue("UI", "SetupWizardIncomplete", false);
		si.SetBoolValue("DEV9/Eth", "EthEnable", false);
		si.SetBoolValue("DEV9/Hdd", "HddEnable", false);
		si.SetBoolValue("DEV9/Eth", "ZdxsvFastBoot", false);
		si.SetBoolValue("DEV9/Eth", "ZdxsvFastLogin", false);
		si.SetBoolValue("DEV9/Eth", "ZdxsvSaveReplay", false);
		si.SetBoolValue("DEV9/Eth", "ZdxsvReplayUpload", false);
		si.SetBoolValue("DEV9/Eth", "ZdxsvCrashReports", false);
		SetNextReplay(s_replay_child.src, s_replay_child.pov);
		// Armed with no state: the replay's end stops the VM, and -nogui exits with it.
		SetReplayReturn({});
		Console.WriteLn("Zdxsv: replay child of %lld, settings saved to %s", s_replay_child.parent_pid, si.GetFileName().c_str());
	}

	void QtReplayChildWatchParent(QObject* parent)
	{
		if (!s_replay_child.child)
			return;
		g_replay_child_unfocused = qApp->applicationState() != Qt::ApplicationActive;
		QObject::connect(qApp, &QGuiApplication::applicationStateChanged, parent,
			[](Qt::ApplicationState state) { g_replay_child_unfocused = state != Qt::ApplicationActive; });
		QTimer* timer = new QTimer(parent);
		QObject::connect(timer, &QTimer::timeout, parent, [timer]() {
			if (ProcessAlive(s_replay_child.parent_pid))
				return;
			timer->stop();
			Console.WriteLn("Zdxsv: replay child: parent %lld is gone, exiting", s_replay_child.parent_pid);
			g_main_window->requestExit(false);
		});
		timer->start(PARENT_POLL_MS);
	}

	bool QtReplayChildLaunch(QWidget* dialog_parent, const char* disc, const char* src, int pov)
	{
		if (g_ggpo_active)
		{
			QMessageBox::information(dialog_parent, QObject::tr("zdxsv Replays"),
				QObject::tr("A replay cannot start during a battle."));
			return false;
		}
		if (s_replay_child.running && s_replay_child.running->state() != QProcess::NotRunning)
		{
			s_replay_child.running->kill();
			s_replay_child.running->waitForFinished(5000);
		}
		QProcess* p = new QProcess(qApp);
		QObject::connect(p, &QProcess::finished, p, &QObject::deleteLater);
		const QString arg = QStringLiteral("%1:%2:%3").arg(pov).arg(QCoreApplication::applicationPid()).arg(QString::fromUtf8(src));
		p->setProgram(QCoreApplication::applicationFilePath());
		p->setArguments({QStringLiteral("-nogui"), QStringLiteral("-datapath"), QString::fromStdString(EmuFolders::DataRoot),
			QStringLiteral("-zdxsvreplay"), arg, QStringLiteral("--"), QString::fromUtf8(disc)});
		p->start();
		if (!p->waitForStarted(5000))
		{
			Console.Error("Zdxsv: replay child: %s", p->errorString().toUtf8().constData());
			p->deleteLater();
			return false;
		}
#ifdef _WIN32
		if (const HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(p->processId())))
		{
			SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
			CloseHandle(h);
		}
#endif
		s_replay_child.running = p;
		Console.WriteLn("Zdxsv: replay child %lld started", static_cast<long long>(p->processId()));
		return true;
	}
} // namespace Zdxsv

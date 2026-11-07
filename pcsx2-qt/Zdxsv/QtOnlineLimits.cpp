// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// The debugger window closes while the Z game is online: it can write guest memory and registers.

#include "Zdxsv/QtHooks.h"

#include "Debugger/DebuggerWindow.h"

#include "pcsx2/Zdxsv/UiHooks.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtWidgets/QMessageBox>

#include <utility>

namespace Zdxsv
{
	namespace
	{
		bool s_exit_allowed = false;
	} // namespace

	void QtOnlineLimitsWatch(QObject* parent)
	{
		QTimer* timer = new QTimer(parent);
		QObject::connect(timer, &QTimer::timeout, [] {
			if (g_debugger_window && g_debugger_window->isVisible() && OnlineNowAnyThread())
			{
				DebuggerWindow::destroyInstance();
				OnlineRefuses();
			}
		});
		timer->start(500);
	}

	bool QtOnlineShutdownRefuses()
	{
		if (std::exchange(s_exit_allowed, false))
			return false;
		return OnlineRefuses();
	}

	bool QtOnlineExitAllows(QWidget* parent, bool ask)
	{
		if (ask && OnlineBattleNowAnyThread() &&
			QMessageBox::question(parent, QCoreApplication::translate("Zdxsv", "Online Battle"),
				QCoreApplication::translate("Zdxsv", "You are in an online battle. Quit and leave the battle?"),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			return false;
		s_exit_allowed = true;
		return true;
	}
} // namespace Zdxsv

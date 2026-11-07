// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// The debugger window closes while the Z game is online: it can write guest memory and registers.

#include "Zdxsv/QtHooks.h"

#include "Debugger/DebuggerWindow.h"

#include "pcsx2/Zdxsv/UiHooks.h"

#include <QtCore/QTimer>

namespace Zdxsv
{
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
} // namespace Zdxsv

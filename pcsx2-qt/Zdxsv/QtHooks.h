// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

class QGridLayout;
class QObject;
class QWidget;
class SettingsWindow;

namespace Zdxsv
{
	// Player and pilot names on the OSD are Japanese whatever the UI language. UI thread, before the fonts load.
	void QtFontsDownloadJapanese(QWidget* dialog_parent);
	// After the main window exists.
	void QtOnlineLimitsWatch(QObject* parent);
	bool QtOnlineShutdownRefuses();
	// Quitting stays allowed online: in a battle after a question, the way out of a frozen battle.
	bool QtOnlineExitAllows(QWidget* parent, bool ask);
	void QtSetupWizardCustomize(QWidget* wizard);
	bool QtSetupWizardControllerConfirm(QWidget* wizard);
	void QtEmulationSettingsAdd(SettingsWindow* dialog, QGridLayout* checkboxes);
} // namespace Zdxsv

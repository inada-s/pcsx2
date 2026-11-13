// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

class INISettingsInterface;
class QObject;
class QWidget;

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
	// UI thread. True: the wheel event changed the volume and is not passed on as input.
	bool QtWheelVolumeTakes(int angle_delta_y);
	// Command line `-zdxsvreplay <pov>:<parent pid>:<source>`: this process only plays that replay or live battle.
	bool QtReplayChildParseArg(const char* arg);
	bool QtReplayChild();
	// After the settings load: overrides for the child, saves redirected to its own folder.
	void QtReplayChildSettings(INISettingsInterface& si);
	// After the main window exists: the child exits when its parent is gone.
	void QtReplayChildWatchParent(QObject* parent);
	bool QtReplayChildLaunch(QWidget* dialog_parent, const char* disc, const char* src, int pov);
} // namespace Zdxsv

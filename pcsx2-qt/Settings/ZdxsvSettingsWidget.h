// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_ZdxsvSettingsWidget.h"
#include "SettingsWidget.h"

class ZdxsvSettingsWidget : public SettingsWidget
{
	Q_OBJECT

public:
	ZdxsvSettingsWidget(SettingsWindow* settings_dialog, QWidget* parent);
	~ZdxsvSettingsWidget();

	// SettingsWindow: the widgets of settings the Z game forces (pcsx2/Zdxsv/SyncSettings.cpp) are greyed out with
	// a tooltip while forced; checked once a second.
	static void markSyncForced(QWidget* window);

private:
	Ui::ZdxsvSettingsWidget m_ui;
};

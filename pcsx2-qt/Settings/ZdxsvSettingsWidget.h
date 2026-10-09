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

private:
	Ui::ZdxsvSettingsWidget m_ui;
};

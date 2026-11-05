// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_ZdxsvSettingsWidget.h"
#include "SettingsWidget.h"

#include <atomic>
#include <memory>

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
	void applyPreset(float upscale_multiplier, int min_delay);
	void mapUpnpNow();
	void updateAccount();
	void signIn();
	void signOut();

	Ui::ZdxsvSettingsWidget m_ui;
	std::shared_ptr<std::atomic<bool>> m_sign_in_cancel;
};

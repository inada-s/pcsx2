// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/QtHooks.h"

#include "SettingWidgetBinder.h"
#include "Settings/SettingsWindow.h"

#include <QtCore/QCoreApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QGridLayout>

namespace Zdxsv
{
	void QtEmulationSettingsAdd(SettingsWindow* dialog, QGridLayout* checkboxes)
	{
		const auto tr = [](const char* s) { return QCoreApplication::translate("ZdxsvSettingsWidget", s); };
		QCheckBox* box = new QCheckBox(tr("Low-Latency Present"));
		checkboxes->addWidget(box, 2, 1);
		SettingWidgetBinder::BindWidgetToBoolSetting(
			dialog->getSettingsInterface(), box, "EmuCore/GS", "ZdxsvLowLatencyPresent", true);
		dialog->registerWidgetHelp(box, tr("Low-Latency Present"), tr("Checked"),
			tr("Shows each frame sooner after your input. Downside: frame pacing can be less even."));
	}
} // namespace Zdxsv

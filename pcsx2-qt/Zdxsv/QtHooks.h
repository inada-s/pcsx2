// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

class QWidget;

namespace Zdxsv
{
	// Player and pilot names on the OSD are Japanese whatever the UI language. UI thread, before the fonts load.
	void QtFontsDownloadJapanese(QWidget* dialog_parent);
} // namespace Zdxsv

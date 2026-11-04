// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/QtHooks.h"
#include "QtHost.h"

#include "pcsx2/Config.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <QtCore/QCoreApplication>

namespace Zdxsv
{
	void QtFontsDownloadJapanese(QWidget* dialog_parent)
	{
		static constexpr const char* name = "NotoSansJP-Regular.ttf";
		const std::string path = Path::Combine("fonts", name);
		if (FileSystem::FileExists(EmuFolders::GetOverridableResourcePath(path).c_str()))
			return;
		QtHost::DownloadFile(dialog_parent, qApp->translate("MainWindow", "Downloading Files"),
			QtHost::GetRuntimeDownloadedResourceURL(name), Path::Combine(EmuFolders::UserResources, path));
	}
} // namespace Zdxsv

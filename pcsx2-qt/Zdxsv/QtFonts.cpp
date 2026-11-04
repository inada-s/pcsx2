// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/QtHooks.h"
#include "QtHost.h"

#include "pcsx2/Config.h"

#include "common/FileSystem.h"
#include "common/Path.h"

namespace Zdxsv
{
	void QtFontsDownloadJapanese(QWidget* dialog_parent)
	{
		static constexpr const char* name = "NotoSansJP-Regular.ttf";
		const std::string path = Path::Combine("fonts", name);
		if (FileSystem::FileExists(EmuFolders::GetOverridableResourcePath(path).c_str()))
			return;
		QtHost::DownloadMissingFont(dialog_parent, name, Path::Combine(EmuFolders::UserResources, path));
	}
} // namespace Zdxsv

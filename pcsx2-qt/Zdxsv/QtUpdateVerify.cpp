// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/QtHooks.h"

#include "pcsx2/Zdxsv/TestOptions.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QUrl>

namespace Zdxsv
{
	QString QtUpdateAssetUrl(const QString& tag, const QString& name)
	{
		// ZDXSV_UPDATE_URL (tests) serves the release files from its own host.
		const char* list_url = TestEnv("ZDXSV_UPDATE_URL");
		const QString origin = list_url ?
			QUrl(QString::fromUtf8(list_url)).adjusted(QUrl::RemoveUserInfo | QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toString() :
			QStringLiteral("https://github.com");
		return origin + QStringLiteral("/inada-s/pcsx2/releases/download/") + tag + QLatin1Char('/') + name;
	}

	bool QtUpdateSumsMatch(const QByteArray& sums, const QString& name, const u8* data, size_t size, QString* error)
	{
		const QByteArray hash = QCryptographicHash::hash(QByteArrayView(data, static_cast<qsizetype>(size)), QCryptographicHash::Sha256).toHex();
		const QByteArray name_utf8 = name.toUtf8();
		for (const QByteArray& line : sums.split('\n'))
		{
			// sha256sum: "<hex>  <name>", or "<hex> *<name>" in binary mode.
			const QList<QByteArray> fields = line.simplified().split(' ');
			if (fields.size() != 2)
				continue;
			const QByteArray file = fields[1].startsWith('*') ? fields[1].mid(1) : fields[1];
			if (file != name_utf8)
				continue;
			if (fields[0].toLower() == hash)
				return true;
			*error = QStringLiteral("SHA-256 of %1 is %2, SHA256SUMS.txt has %3").arg(name, QString::fromLatin1(hash), QString::fromLatin1(fields[0]));
			return false;
		}
		*error = QStringLiteral("SHA256SUMS.txt has no line for %1").arg(name);
		return false;
	}
} // namespace Zdxsv

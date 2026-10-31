// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Tools/ZdxsvReplayDialog.h"
#include "MainWindow.h"
#include "QtHost.h"
#include "QtUtils.h"

#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/Zdxsv/Lobby.h"
#include "pcsx2/Zdxsv/ReplayList.h"
#include "pcsx2/Zdxsv/UiHooks.h"

#include "common/Console.h"

#include "common/FileSystem.h"
#include "common/HTTPDownloader.h"
#include "common/Path.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <thread>

namespace
{
	constexpr int REMOTE_PAGE_SIZE = 100; // /lbs/replay's page
	constexpr int LIVE_REFRESH_MS = 10000;
	constexpr double FRAMES_PER_SECOND = 59.94;
	constexpr int SOURCE_ROLE = Qt::UserRole;
	constexpr int START_ROLE = Qt::UserRole + 1;
	constexpr int TAB_LIVE = 2;
	constexpr int TAB_FAVORITES = 3;
	constexpr int MAX_FAVORITES = 1000;
	constexpr const char* FAVORITES_SECTION = "DEV9/Eth";
	constexpr const char* FAVORITES_KEY = "ZdxsvReplayFavorites";

	QPointer<ZdxsvReplayDialog> s_dialog;

	// a favorite: battle code, start time, players and rounds, tab-separated
	struct Favorite
	{
		QString code;
		qint64 start_unix = 0;
		QString players;
		int rounds = 0;
	};

	std::vector<Favorite> LoadFavorites()
	{
		std::vector<Favorite> list;
		for (const std::string& value : Host::GetBaseStringListSetting(FAVORITES_SECTION, FAVORITES_KEY))
		{
			const QStringList f = QString::fromStdString(value).split(QLatin1Char('\t'));
			if (f[0].isEmpty())
				continue;
			list.push_back({f[0], f.size() > 1 ? f[1].toLongLong() : 0, f.size() > 2 ? f[2] : QString(), f.size() > 3 ? f[3].toInt() : 0});
		}
		return list;
	}

	void SaveFavorites(const std::vector<Favorite>& list)
	{
		std::vector<std::string> values;
		for (const Favorite& f : list)
		{
			QString players = f.players;
			players.replace(QLatin1Char('\t'), QLatin1Char(' '));
			values.push_back(QStringLiteral("%1\t%2\t%3\t%4").arg(f.code).arg(f.start_unix).arg(players).arg(f.rounds).toStdString());
		}
		Host::SetBaseStringListSettingValue(FAVORITES_SECTION, FAVORITES_KEY, values);
		Host::CommitBaseSettingChanges();
	}

	QString FormatDate(qint64 unix_seconds)
	{
		return unix_seconds > 0 ? QDateTime::fromSecsSinceEpoch(unix_seconds).toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
	}

	QString FormatLength(int frames)
	{
		const int s = static_cast<int>(frames / FRAMES_PER_SECOND);
		return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
	}

	QString PlayerName(const QString& name, const QString& pilot)
	{
		return pilot.isEmpty() || pilot == name ? name : QStringLiteral("%1 (%2)").arg(name, pilot);
	}

	// /lbs/replay and /lbs/live users: "A, B vs C, D" by team (the lobby's entry), in battle position order.
	QString JsonPlayers(const QJsonArray& users)
	{
		std::vector<QJsonObject> us;
		for (const QJsonValue& v : users)
			us.push_back(v.toObject());
		std::stable_sort(us.begin(), us.end(), [](const QJsonObject& a, const QJsonObject& b) { return a["pos"].toInt() < b["pos"].toInt(); });
		QStringList teams[3];
		for (const QJsonObject& u : us)
		{
			const int team = u["team"].toInt();
			const QString name = u["user_name"].toString();
			teams[team == 1 || team == 2 ? team : 0].append(PlayerName(name.isEmpty() ? u["user_id"].toString() : name, u["pilot_name"].toString()));
		}
		QStringList sides;
		for (const QStringList& t : teams)
			if (!t.isEmpty())
				sides.append(t.join(QStringLiteral(", ")));
		return sides.join(QStringLiteral(" vs "));
	}

	QTreeWidget* CreateTree(const QStringList& headers)
	{
		QTreeWidget* tree = new QTreeWidget();
		tree->setHeaderLabels(headers);
		tree->setRootIsDecorated(false);
		tree->setUniformRowHeights(true);
		tree->setAlternatingRowColors(true);
		tree->header()->setStretchLastSection(false);
		tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
		return tree;
	}

	void ReturnFromReplay(const std::string& disc)
	{
		std::string state;
		if (!Zdxsv::TakeReplayReturn(state))
			return;
		std::shared_ptr<VMBootParameters> params = std::make_shared<VMBootParameters>();
		params->filename = disc;
		if (!state.empty() && FileSystem::FileExists(state.c_str()))
			params->save_state = state;
		Console.WriteLn("Zdxsv: replay return: %s", params->save_state.empty() ? "boot" : params->save_state.c_str());
		g_emu_thread->startVM(std::move(params));
		ZdxsvReplayDialog::openDialog(g_main_window);
	}

	void BootReplay(const std::string& disc, const QString& src, int pov, bool back, const std::string& state)
	{
		Zdxsv::SetNextReplay(src.toStdString(), pov);
		if (back)
		{
			Zdxsv::SetReplayReturn(state);
			// connected once the replay VM runs: the stop of the game shut down for it must not count
			QObject::connect(g_emu_thread, &EmuThread::onVMStarted, g_main_window, [disc]() {
				QObject::connect(g_emu_thread, &EmuThread::onVMStopped, g_main_window,
					[disc]() { ReturnFromReplay(disc); }, Qt::SingleShotConnection);
			}, Qt::SingleShotConnection);
		}
		std::shared_ptr<VMBootParameters> params = std::make_shared<VMBootParameters>();
		params->filename = disc;
		g_emu_thread->startVM(std::move(params));
	}
} // namespace

ZdxsvReplayDialog::ZdxsvReplayDialog(QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("zdxsv Replays"));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(900, 560);

	QVBoxLayout* layout = new QVBoxLayout(this);

	QFormLayout* api = new QFormLayout();
	m_api_url = new QLineEdit(QString::fromStdString(Zdxsv::LobbyApiUrl()), this);
	m_api_url->setPlaceholderText(tr("http://host:port of the lobby's public API (remote and live lists)"));
	connect(m_api_url, &QLineEdit::editingFinished, this, &ZdxsvReplayDialog::onApiUrlEdited);
	api->addRow(tr("Lobby API:"), m_api_url);
	layout->addLayout(api);

	m_tabs = new QTabWidget(this);
	m_tabs->addTab(createLocalTab(), tr("Local"));
	m_tabs->addTab(createRemoteTab(), tr("Remote"));
	m_tabs->addTab(createLiveTab(), tr("Live"));
	m_tabs->addTab(createFavoritesTab(), tr("Favorites"));
	layout->addWidget(m_tabs, 1);

	QHBoxLayout* bottom = new QHBoxLayout();
	bottom->addWidget(new QLabel(tr("Point of view:"), this));
	m_pov = new QComboBox(this);
	m_pov->addItem(tr("Recorder"), -1);
	for (int p = 0; p < 4; p++)
		m_pov->addItem(tr("%1P").arg(p + 1), p);
	bottom->addWidget(m_pov);
	bottom->addStretch(1);
	m_play = new QPushButton(tr("Play"), this);
	m_play->setDefault(true);
	connect(m_play, &QPushButton::clicked, this, [this]() {
		if (QTreeWidget* tree = currentTree())
			playItem(tree->currentItem());
	});
	bottom->addWidget(m_play);
	QPushButton* close = new QPushButton(tr("Close"), this);
	connect(close, &QPushButton::clicked, this, &QDialog::close);
	bottom->addWidget(close);
	layout->addLayout(bottom);

	connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
		if (index == TAB_LIVE)
		{
			refreshLive();
			m_live_timer.start();
		}
		else
			m_live_timer.stop();
		updatePlayButton();
	});
	m_live_timer.setInterval(LIVE_REFRESH_MS);
	connect(&m_live_timer, &QTimer::timeout, this, &ZdxsvReplayDialog::refreshLive);

	refreshLocal();
	refreshFavorites();
	updatePlayButton();
}

ZdxsvReplayDialog::~ZdxsvReplayDialog() = default;

void ZdxsvReplayDialog::openDialog(QWidget* parent)
{
	if (!s_dialog)
		s_dialog = new ZdxsvReplayDialog(parent);
	s_dialog->show();
	s_dialog->raise();
	s_dialog->activateWindow();
}

QWidget* ZdxsvReplayDialog::createLocalTab()
{
	QWidget* tab = new QWidget(this);
	QVBoxLayout* layout = new QVBoxLayout(tab);
	m_local = CreateTree({tr("Date"), tr("Battle Code"), tr("Players"), tr("Rounds"), tr("Length"), tr("File")});
	layout->addWidget(m_local, 1);
	QHBoxLayout* buttons = new QHBoxLayout();
	m_local_status = new QLabel(tab);
	buttons->addWidget(m_local_status, 1);
	QPushButton* refresh = new QPushButton(tr("Refresh"), tab);
	connect(refresh, &QPushButton::clicked, this, &ZdxsvReplayDialog::refreshLocal);
	buttons->addWidget(refresh);
	QPushButton* folder = new QPushButton(tr("Open Folder"), tab);
	connect(folder, &QPushButton::clicked, this, [this]() {
		const std::string dir = Zdxsv::ReplayDir();
		FileSystem::EnsureDirectoryExists(dir.c_str(), false);
		QtUtils::OpenURL(this, QUrl::fromLocalFile(QString::fromStdString(dir)));
	});
	buttons->addWidget(folder);
	QPushButton* browse = new QPushButton(tr("Browse..."), tab);
	connect(browse, &QPushButton::clicked, this, &ZdxsvReplayDialog::browseFile);
	buttons->addWidget(browse);
	layout->addLayout(buttons);
	connect(m_local, &QTreeWidget::itemActivated, this, &ZdxsvReplayDialog::playItem);
	connect(m_local, &QTreeWidget::currentItemChanged, this, &ZdxsvReplayDialog::updatePlayButton);
	return tab;
}

QWidget* ZdxsvReplayDialog::createRemoteTab()
{
	QWidget* tab = new QWidget(this);
	QVBoxLayout* layout = new QVBoxLayout(tab);
	QHBoxLayout* filters = new QHBoxLayout();
	m_remote_code = new QLineEdit(tab);
	m_remote_code->setPlaceholderText(tr("Battle code"));
	m_remote_name = new QLineEdit(tab);
	m_remote_name->setPlaceholderText(tr("Player name"));
	m_remote_pilot = new QLineEdit(tab);
	m_remote_pilot->setPlaceholderText(tr("Pilot name"));
	QPushButton* search = new QPushButton(tr("Search"), tab);
	for (QLineEdit* e : {m_remote_code, m_remote_name, m_remote_pilot})
	{
		e->setAccessibleName(e->placeholderText());
		filters->addWidget(e);
		connect(e, &QLineEdit::returnPressed, this, [this]() { searchRemote(0); });
	}
	connect(search, &QPushButton::clicked, this, [this]() { searchRemote(0); });
	filters->addWidget(search);
	layout->addLayout(filters);
	QHBoxLayout* filters2 = new QHBoxLayout();
	m_remote_user = new QLineEdit(tab);
	m_remote_user->setPlaceholderText(tr("User ID"));
	m_remote_user->setAccessibleName(m_remote_user->placeholderText());
	connect(m_remote_user, &QLineEdit::returnPressed, this, [this]() { searchRemote(0); });
	filters2->addWidget(m_remote_user);
	m_remote_players = new QComboBox(tab);
	m_remote_players->setAccessibleName(tr("Players"));
	m_remote_players->addItem(tr("Any players"), -1);
	m_remote_players->addItem(tr("2 players"), 2);
	m_remote_players->addItem(tr("4 players"), 4);
	filters2->addWidget(m_remote_players);
	m_remote_ms = new QComboBox(tab);
	m_remote_ms->setAccessibleName(tr("Mobile suit"));
	m_remote_ms->setToolTip(tr("Battles where a player matching the name filters (any player without them) used this mobile suit."));
	m_remote_ms->addItem(tr("Any mobile suit"), 0);
	for (u32 id = 0; const char* name = Zdxsv::MsName(id); id++)
		m_remote_ms->addItem(QString::fromUtf8(name), id + 1);
	filters2->addWidget(m_remote_ms);
	m_remote_rated = new QCheckBox(tr("Rated only"), tab);
	m_remote_rated->setToolTip(tr("Only battles counted in the players' records."));
	filters2->addWidget(m_remote_rated);
	m_remote_reverse = new QCheckBox(tr("Oldest first"), tab);
	filters2->addWidget(m_remote_reverse);
	filters2->addStretch(1);
	layout->addLayout(filters2);
	m_remote = CreateTree({tr("Date"), tr("Battle Code"), tr("Players"), tr("Rounds")});
	layout->addWidget(m_remote, 1);
	QHBoxLayout* pages = new QHBoxLayout();
	m_remote_status = new QLabel(tab);
	pages->addWidget(m_remote_status, 1);
	m_remote_prev = new QPushButton(tr("Newer"), tab);
	m_remote_next = new QPushButton(tr("Older"), tab);
	m_remote_prev->setEnabled(false);
	m_remote_next->setEnabled(false);
	connect(m_remote_prev, &QPushButton::clicked, this, [this]() { searchRemote(m_remote_page - 1); });
	connect(m_remote_next, &QPushButton::clicked, this, [this]() { searchRemote(m_remote_page + 1); });
	pages->addWidget(m_remote_prev);
	pages->addWidget(m_remote_next);
	QPushButton* favorite = new QPushButton(tr("Add to Favorites"), tab);
	connect(favorite, &QPushButton::clicked, this, &ZdxsvReplayDialog::addFavorite);
	pages->addWidget(favorite);
	layout->addLayout(pages);
	connect(m_remote, &QTreeWidget::itemActivated, this, &ZdxsvReplayDialog::playItem);
	connect(m_remote, &QTreeWidget::currentItemChanged, this, &ZdxsvReplayDialog::updatePlayButton);
	return tab;
}

QWidget* ZdxsvReplayDialog::createLiveTab()
{
	QWidget* tab = new QWidget(this);
	QVBoxLayout* layout = new QVBoxLayout(tab);
	m_live = CreateTree({tr("Started"), tr("Battle Code"), tr("Players"), tr("Elapsed"), tr("State"), tr("Spectators")});
	layout->addWidget(m_live, 1);
	QHBoxLayout* buttons = new QHBoxLayout();
	m_live_status = new QLabel(tab);
	buttons->addWidget(m_live_status, 1);
	QPushButton* refresh = new QPushButton(tr("Refresh"), tab);
	connect(refresh, &QPushButton::clicked, this, &ZdxsvReplayDialog::refreshLive);
	buttons->addWidget(refresh);
	layout->addLayout(buttons);
	connect(m_live, &QTreeWidget::itemActivated, this, &ZdxsvReplayDialog::playItem);
	connect(m_live, &QTreeWidget::currentItemChanged, this, &ZdxsvReplayDialog::updatePlayButton);
	return tab;
}

QWidget* ZdxsvReplayDialog::createFavoritesTab()
{
	QWidget* tab = new QWidget(this);
	QVBoxLayout* layout = new QVBoxLayout(tab);
	m_favorites = CreateTree({tr("Date"), tr("Battle Code"), tr("Players"), tr("Rounds")});
	layout->addWidget(m_favorites, 1);
	QHBoxLayout* buttons = new QHBoxLayout();
	m_favorites_status = new QLabel(tab);
	buttons->addWidget(m_favorites_status, 1);
	QPushButton* remove = new QPushButton(tr("Remove"), tab);
	connect(remove, &QPushButton::clicked, this, &ZdxsvReplayDialog::removeFavorite);
	buttons->addWidget(remove);
	layout->addLayout(buttons);
	connect(m_favorites, &QTreeWidget::itemActivated, this, &ZdxsvReplayDialog::playItem);
	connect(m_favorites, &QTreeWidget::currentItemChanged, this, &ZdxsvReplayDialog::updatePlayButton);
	return tab;
}

void ZdxsvReplayDialog::refreshFavorites()
{
	m_favorites->clear();
	const std::string api = Zdxsv::LobbyApiUrl();
	const std::vector<Favorite> list = LoadFavorites();
	for (const Favorite& f : list)
	{
		QTreeWidgetItem* item = new QTreeWidgetItem(m_favorites);
		item->setText(0, FormatDate(f.start_unix));
		item->setText(1, f.code);
		item->setText(2, f.players);
		item->setText(3, f.rounds > 0 ? QString::number(f.rounds) : QString());
		if (!api.empty())
			item->setData(0, SOURCE_ROLE, QString::fromStdString(api) + QStringLiteral("/lbs/replay?battle_code=") + QString::fromLatin1(QUrl::toPercentEncoding(f.code)));
	}
	m_favorites_status->setText(tr("%n favorite(s)", nullptr, static_cast<int>(list.size())));
}

void ZdxsvReplayDialog::addFavorite()
{
	const QTreeWidgetItem* item = m_remote->currentItem();
	if (!item)
		return;
	std::vector<Favorite> list = LoadFavorites();
	const QString code = item->text(1);
	if (std::any_of(list.begin(), list.end(), [&code](const Favorite& f) { return f.code == code; }))
	{
		m_remote_status->setText(tr("%1 is already a favorite.").arg(code));
		return;
	}
	if (list.size() >= MAX_FAVORITES)
	{
		m_remote_status->setText(tr("Favorites are full (%1): remove some first.").arg(MAX_FAVORITES));
		return;
	}
	list.push_back({code, item->data(0, START_ROLE).toLongLong(), item->text(2), item->text(3).toInt()});
	SaveFavorites(list);
	refreshFavorites();
	m_remote_status->setText(tr("Added %1 to Favorites.").arg(code));
}

void ZdxsvReplayDialog::removeFavorite()
{
	const QTreeWidgetItem* item = m_favorites->currentItem();
	if (!item)
		return;
	std::vector<Favorite> list = LoadFavorites();
	const QString code = item->text(1);
	list.erase(std::remove_if(list.begin(), list.end(), [&code](const Favorite& f) { return f.code == code; }), list.end());
	SaveFavorites(list);
	refreshFavorites();
}

void ZdxsvReplayDialog::refreshLocal()
{
	m_local->clear();
	const std::string dir = Zdxsv::ReplayDir();
	FileSystem::FindResultsArray files;
	FileSystem::FindFiles(dir.c_str(), "*.pb", FILESYSTEM_FIND_FILES, &files);
	std::sort(files.begin(), files.end(), [](const FILESYSTEM_FIND_DATA& a, const FILESYSTEM_FIND_DATA& b) {
		return a.ModificationTime > b.ModificationTime;
	});
	for (const FILESYSTEM_FIND_DATA& f : files)
	{
		Zdxsv::ReplayFileInfo info;
		const bool ok = Zdxsv::ReadReplayInfo(f.FileName, &info);
		QStringList players;
		std::sort(info.users.begin(), info.users.end(), [](const auto& a, const auto& b) { return a.pos < b.pos; });
		for (const Zdxsv::ReplayFileUser& u : info.users)
			players.append(PlayerName(QString::fromStdString(u.name), QString::fromStdString(u.pilot)));
		QTreeWidgetItem* item = new QTreeWidgetItem(m_local);
		item->setText(0, FormatDate(info.start_at > 0 ? info.start_at : static_cast<qint64>(f.ModificationTime)));
		item->setText(1, QString::fromStdString(info.battle_code));
		item->setText(2, ok ? players.join(QStringLiteral(", ")) : tr("(unreadable)"));
		item->setText(3, ok ? QString::number(info.rounds) : QString());
		item->setText(4, ok ? FormatLength(info.frames) : QString());
		item->setText(5, QString::fromStdString(std::string(Path::GetFileName(f.FileName))));
		item->setData(0, SOURCE_ROLE, QString::fromStdString(f.FileName));
	}
	m_local_status->setText(tr("%n replay(s) in %1", nullptr, static_cast<int>(files.size())).arg(QString::fromStdString(dir)));
}

void ZdxsvReplayDialog::searchRemote(int page)
{
	const std::string api = Zdxsv::LobbyApiUrl();
	if (api.empty())
	{
		m_remote_status->setText(tr("Set the lobby API URL above."));
		return;
	}
	QStringList query;
	auto add = [&query](const char* key, QString value, bool like) {
		value = value.trimmed();
		if (value.isEmpty())
			return;
		if (like && !value.contains(QLatin1Char('%')))
			value = QStringLiteral("%%1%").arg(value); // names are SQL LIKE patterns: a part of the name matches
		query.append(QStringLiteral("%1=%2").arg(QLatin1String(key), QString::fromLatin1(QUrl::toPercentEncoding(value))));
	};
	add("battle_code", m_remote_code->text(), false);
	add("user_name", m_remote_name->text(), true);
	add("pilot_name", m_remote_pilot->text(), true);
	add("user_id", m_remote_user->text(), false);
	if (const int players = m_remote_players->currentData().toInt(); players > 0)
		query.append(QStringLiteral("players=%1").arg(players));
	if (const int ms = m_remote_ms->currentData().toInt(); ms > 0)
		query.append(QStringLiteral("used_ms=%1").arg(ms));
	if (m_remote_rated->isChecked())
		query.append(QStringLiteral("aggregate=1"));
	const bool reverse = m_remote_reverse->isChecked();
	if (reverse)
		query.append(QStringLiteral("reverse=1"));
	m_remote_prev->setText(reverse ? tr("Older") : tr("Newer"));
	m_remote_next->setText(reverse ? tr("Newer") : tr("Older"));
	query.append(QStringLiteral("page=%1").arg(page));
	const std::string url = api + "/lbs/replay?" + query.join(QLatin1Char('&')).toStdString();
	const int req = ++m_remote_req;
	m_remote_status->setText(tr("Searching..."));
	m_remote_prev->setEnabled(false);
	m_remote_next->setEnabled(false);
	fetch(url, [this, req, page, api](int status, const QByteArray& body) {
		if (req != m_remote_req)
			return;
		m_remote->clear();
		m_remote_page = page;
		if (status != HTTPDownloader::HTTP_STATUS_OK && status != 204)
		{
			m_remote_status->setText(tr("The lobby API did not answer (status %1).").arg(status));
			return;
		}
		const QJsonArray list = QJsonDocument::fromJson(body).array();
		for (const QJsonValue& v : list)
		{
			const QJsonObject o = v.toObject();
			const QString code = o["battle_code"].toString();
			QString src = o["replay_url"].toString();
			if (src.isEmpty()) // the replay loader asks the API for the replay_url itself
				src = QString::fromStdString(api) + QStringLiteral("/lbs/replay?battle_code=") + code;
			QTreeWidgetItem* item = new QTreeWidgetItem(m_remote);
			const qint64 start = static_cast<qint64>(o["start_unix"].toDouble());
			item->setText(0, FormatDate(start));
			item->setText(1, code);
			item->setText(2, JsonPlayers(o["users"].toArray()));
			item->setText(3, o["round"].toInt() > 0 ? QString::number(o["round"].toInt()) : QString());
			item->setData(0, SOURCE_ROLE, src);
			item->setData(0, START_ROLE, start);
		}
		m_remote_status->setText(list.isEmpty() ? tr("No replays found.") : tr("Page %1: %n replay(s)", nullptr, list.size()).arg(page + 1));
		m_remote_prev->setEnabled(page > 0);
		m_remote_next->setEnabled(list.size() >= REMOTE_PAGE_SIZE);
	});
}

void ZdxsvReplayDialog::refreshLive()
{
	const std::string api = Zdxsv::LobbyApiUrl();
	if (api.empty())
	{
		m_live_status->setText(tr("Set the lobby API URL above."));
		return;
	}
	const int req = ++m_live_req;
	fetch(api + "/lbs/live", [this, req](int status, const QByteArray& body) {
		if (req != m_live_req)
			return;
		if (status != HTTPDownloader::HTTP_STATUS_OK)
		{
			m_live->clear();
			m_live_status->setText(tr("The lobby API did not answer (status %1).").arg(status));
			return;
		}
		const QTreeWidgetItem* cur = m_live->currentItem();
		const QString selected = cur ? cur->text(1) : QString();
		m_live->clear();
		const QJsonArray list = QJsonDocument::fromJson(body).array();
		int running = 0;
		for (const QJsonValue& v : list)
		{
			const QJsonObject o = v.toObject();
			const QString code = o["battle_code"].toString();
			const bool closed = o["closed"].toBool();
			running += !closed;
			QTreeWidgetItem* item = new QTreeWidgetItem(m_live);
			item->setText(0, FormatDate(static_cast<qint64>(o["start_unix"].toDouble())));
			item->setText(1, code);
			item->setText(2, JsonPlayers(o["users"].toArray()));
			item->setText(3, FormatLength(o["frames"].toInt()));
			item->setText(4, closed ? tr("Ended") : tr("Live"));
			item->setText(5, QString::number(o["spectators"].toInt()));
			item->setData(0, SOURCE_ROLE, QString::fromStdString(Zdxsv::LiveReplaySource(code.toStdString())));
			if (code == selected)
				m_live->setCurrentItem(item);
		}
		m_live_status->setText(list.isEmpty() ? tr("No live battles.") : tr("%1 live, %2 ended").arg(running).arg(list.size() - running));
	});
}

void ZdxsvReplayDialog::browseFile()
{
	const QString path = QFileDialog::getOpenFileName(this, tr("Select Replay"), QString::fromStdString(Zdxsv::ReplayDir()),
		tr("zdxsv Replays (*.pb);;All Files (*.*)"));
	if (!path.isEmpty())
		play(QDir::toNativeSeparators(path));
}

void ZdxsvReplayDialog::onApiUrlEdited()
{
	const std::string url = m_api_url->text().trimmed().toStdString();
	if (url == Zdxsv::LobbyApiUrl())
		return;
	Host::SetBaseStringSettingValue("DEV9/Eth", "ZdxsvLobbyApiUrl", url.c_str());
	Host::CommitBaseSettingChanges();
	m_remote->clear();
	m_live->clear();
	refreshFavorites();
	if (m_tabs->currentIndex() == TAB_LIVE)
		refreshLive();
}

void ZdxsvReplayDialog::updatePlayButton()
{
	const QTreeWidget* tree = currentTree();
	m_play->setEnabled(tree && tree->currentItem());
}

void ZdxsvReplayDialog::fetch(const std::string& url, std::function<void(int, const QByteArray&)> done)
{
	std::thread([dialog = QPointer<ZdxsvReplayDialog>(this), url, done = std::move(done)]() {
		int status = -1;
		QByteArray body;
		if (std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent()))
		{
			http->SetTimeout(10.0f);
			http->CreateRequest(url, [&](s32 st, const std::string&, HTTPDownloader::Request::Data data) {
				status = st;
				body = QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size()));
			});
			http->WaitForAllRequests();
		}
		QtHost::RunOnUIThread([dialog, done, status, body]() {
			if (dialog)
				done(status, body);
		});
	}).detach();
}

QTreeWidget* ZdxsvReplayDialog::currentTree() const
{
	switch (m_tabs->currentIndex())
	{
		case 0:
			return m_local;
		case 1:
			return m_remote;
		case TAB_LIVE:
			return m_live;
		case TAB_FAVORITES:
			return m_favorites;
		default:
			return nullptr;
	}
}

void ZdxsvReplayDialog::playItem(QTreeWidgetItem* item)
{
	if (!item)
		return;
	const QString src = item->data(0, SOURCE_ROLE).toString();
	if (src.isEmpty())
	{
		QMessageBox::critical(this, tr("zdxsv Replays"), tr("Set the lobby API URL above."));
		return;
	}
	play(src);
}

void ZdxsvReplayDialog::play(const QString& src)
{
	std::string disc;
	{
		const auto lock = GameList::GetLock();
		if (const GameList::Entry* e = GameList::GetEntryBySerialAndCRC(Zdxsv::GAME_SERIAL, Zdxsv::GAME_CRC))
			disc = e->path;
	}
	if (disc.empty())
	{
		QMessageBox::critical(this, tr("zdxsv Replays"),
			tr("Mobile Suit Gundam: Gundam vs. Zeta Gundam (%1) is not in the game list. Add its folder in Settings → Game List.")
				.arg(QLatin1String(Zdxsv::GAME_SERIAL)));
		return;
	}
	const int pov = m_pov->currentData().toInt();
	const bool back = Host::GetBaseBoolSettingValue("DEV9/Eth", "ZdxsvReplayReturn", true);
	if (QtHost::IsVMValid())
	{
		if (QMessageBox::question(this, tr("zdxsv Replays"), tr("Shut down the running game and play the replay?")) != QMessageBox::Yes)
			return;
		std::string state;
		if (back && VMManager::GetDiscSerial() == Zdxsv::GAME_SERIAL && !Zdxsv::LobbyConnectionOpen() && !Zdxsv::g_ggpo_active)
		{
			state = Path::Combine(Zdxsv::ReplayDir(), "return.p2s");
			FileSystem::DeleteFilePath(state.c_str());
			Host::RunOnCPUThread([state]() { VMManager::SaveState(state.c_str(), false, false, [](const std::string& error) {
				Console.Error("Zdxsv: replay return state: %s", error.c_str());
			}); });
		}
		// the boot waits for the VM to stop; the dialog may be gone by then
		const QMetaObject::Connection boot = connect(g_emu_thread, &EmuThread::onVMStopped, g_main_window,
			[disc, src, pov, back, state]() { BootReplay(disc, src, pov, back, state); }, Qt::SingleShotConnection);
		if (!g_main_window->requestShutdown(false, false, false))
		{
			disconnect(boot);
			return;
		}
	}
	else
		BootReplay(disc, src, pov, back, {});
	close();
}

#include "moc_ZdxsvReplayDialog.cpp"

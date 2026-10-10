// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <QtCore/QTimer>
#include <QtWidgets/QDialog>

#include <functional>
#include <string>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

// Tools → zdxsv Replays (as flycast's gdxsv replay menu): local .pb files, the lobby's uploaded replays
// (/lbs/replay) and its live battles (/lbs/live). Play boots the Z game with the pick (Zdxsv::SetNextReplay).
class ZdxsvReplayDialog final : public QDialog
{
	Q_OBJECT

public:
	explicit ZdxsvReplayDialog(QWidget* parent = nullptr);
	~ZdxsvReplayDialog() override;

	static void openDialog(QWidget* parent);

private:
	QWidget* createLocalTab();
	QWidget* createRemoteTab();
	QWidget* createLiveTab();

	void refreshLocal();
	void searchRemote(int page);
	void refreshLive();
	void browseFile();
	void onApiUrlEdited();
	void updatePlayButton();

	// GET url on a worker thread; done(status, body) on the UI thread while the dialog lives. -1 = no response.
	void fetch(const std::string& url, std::function<void(int, const QByteArray&)> done);

	QTreeWidget* currentTree() const;
	void playItem(QTreeWidgetItem* item);
	void play(const QString& src);

	QLineEdit* m_api_url = nullptr;
	QTabWidget* m_tabs = nullptr;
	QComboBox* m_pov = nullptr;
	QPushButton* m_play = nullptr;

	QTreeWidget* m_local = nullptr;
	QLabel* m_local_status = nullptr;

	QLineEdit* m_remote_code = nullptr;
	QLineEdit* m_remote_name = nullptr;
	QLineEdit* m_remote_pilot = nullptr;
	QPushButton* m_remote_prev = nullptr;
	QPushButton* m_remote_next = nullptr;
	QTreeWidget* m_remote = nullptr;
	QLabel* m_remote_status = nullptr;
	int m_remote_page = 0;
	int m_remote_req = 0;

	QTreeWidget* m_live = nullptr;
	QLabel* m_live_status = nullptr;
	QTimer m_live_timer;
	int m_live_req = 0;
};

// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ZdxsvSettingsWidget.h"
#include "SettingWidgetBinder.h"
#include "GameFixSettingsWidget.h"
#include "SettingsWindow.h"
#include "QtHost.h"

#include "pcsx2/Zdxsv/AuthSignIn.h"
#include "pcsx2/Zdxsv/CrashReportHooks.h"
#include "pcsx2/Zdxsv/SyncSettings.h"
#include "pcsx2/Zdxsv/Upnp.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>

#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// The environment wins over a setting (pcsx2/Zdxsv/Settings.h): a widget whose setting is overridden is disabled
// and says by what.
static void MarkEnvOverride(QWidget* widget, const char* env, const char* key = nullptr)
{
	const char* value = std::getenv(env);
	if (!value || (key && !std::strstr(value, key)))
		return;
	widget->setEnabled(false);
	widget->setToolTip(QObject::tr("Overridden by the environment variable %1=%2").arg(env).arg(value));
}

ZdxsvSettingsWidget::ZdxsvSettingsWidget(SettingsWindow* settings_dialog, QWidget* parent)
	: SettingsWidget(settings_dialog, parent)
{
	SettingsInterface* sif = dialog()->getSettingsInterface();

	setupTab(m_ui, tr("zdxsv"));

	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvFastBoot, "DEV9/Eth", "ZdxsvFastBoot", true);
	SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.zdxsvGgpoMinDelay, "DEV9/Eth", "ZdxsvGgpoMinDelay", 2);
	SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.zdxsvGgpoPort, "DEV9/Eth", "ZdxsvGgpoPort", 0);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvNetOsd, "DEV9/Eth", "ZdxsvNetOsd", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvUPnP, "DEV9/Eth", "ZdxsvUPnP", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvLowLatencyVsync, "EmuCore/GS", "ZdxsvLowLatencyVsync", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvSingleDrawList, "DEV9/Eth", "ZdxsvSingleDrawList", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvLiveAutoNext, "DEV9/Eth", "ZdxsvLiveAutoNext", false);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvReplaySkipMs, "DEV9/Eth", "ZdxsvReplaySkipMs", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvReplayKeyDisplay, "DEV9/Eth", "ZdxsvReplayKeyDisplay", false);
	SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.zdxsvReplayBar, "DEV9/Eth", "ZdxsvReplayBar", 0);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvReplayUpload, "DEV9/Eth", "ZdxsvReplayUpload", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvSaveReplay, "DEV9/Eth", "ZdxsvSaveReplay", true);
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.zdxsvReplayPath, m_ui.zdxsvReplayPathBrowse, m_ui.zdxsvReplayPathOpen,
		m_ui.zdxsvReplayPathReset, "DEV9/Eth", "ZdxsvReplayPath", Path::Combine(EmuFolders::DataRoot, "replays"));

	if (Zdxsv::CrashReportsAvailable() && !dialog()->isPerGameSettings())
	{
		SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvCrashReports, "DEV9/Eth", "ZdxsvCrashReports", true);
		connect(m_ui.zdxsvCrashReports, &QCheckBox::checkStateChanged, this,
			[](Qt::CheckState state) { Zdxsv::CrashReportsSetEnabled(state == Qt::Checked); });
		dialog()->registerWidgetHelp(m_ui.zdxsvCrashReports, tr("Crash Reports"), tr("Checked"),
			tr("When PCSX2 crashes, a report is sent to the zdxsv developers. It contains part of the memory of PCSX2 at the moment of the crash, your version of PCSX2, your operating system and a random ID of this installation. Turning this on takes effect at the next start."));
	}
	else
	{
		m_ui.zdxsvCrashReportsGroup->hide();
	}

	MarkEnvOverride(m_ui.zdxsvFastBoot, "ZDXSV_FAST_BOOT");
	MarkEnvOverride(m_ui.zdxsvSingleDrawList, "ZDXSV_SINGLE_DRAW_LIST");
	// The mindelay=, port=, osd=, upload= and replay= keys of ZDXSV_GGPO override those settings.
	MarkEnvOverride(m_ui.zdxsvGgpoMinDelay, "ZDXSV_GGPO", "mindelay=");
	MarkEnvOverride(m_ui.zdxsvGgpoPort, "ZDXSV_GGPO", "port=");
	MarkEnvOverride(m_ui.zdxsvNetOsd, "ZDXSV_GGPO", "osd=");
	MarkEnvOverride(m_ui.zdxsvLiveAutoNext, "ZDXSV_LIVE_NEXT");
	MarkEnvOverride(m_ui.zdxsvReplaySkipMs, "ZDXSV_REPLAY_SKIP_MS");
	MarkEnvOverride(m_ui.zdxsvReplayKeyDisplay, "ZDXSV_REPLAY_KEY_DISPLAY");
	MarkEnvOverride(m_ui.zdxsvReplayBar, "ZDXSV_REPLAY_BAR");
	MarkEnvOverride(m_ui.zdxsvReplayUpload, "ZDXSV_GGPO", "upload=");
	MarkEnvOverride(m_ui.zdxsvSaveReplay, "ZDXSV_GGPO", "replay=");
	MarkEnvOverride(m_ui.zdxsvReplayPath, "ZDXSV_GGPO", "replay=");

	dialog()->registerWidgetHelp(m_ui.zdxsvFastBoot, tr("Fast Boot"), tr("Checked"),
		tr("From game start to the lobby ID selection with no input: runs at unlimited speed and presses through the title, main menu and network menus (broadband, network setting 1), then returns to normal speed. Stops at once when the route cannot go on, for example with an unformatted memory card or no NET file on it. Uncheck it to play offline."));
	dialog()->registerWidgetHelp(m_ui.zdxsvGgpoMinDelay, tr("Minimum Input Delay"), tr("2"),
		tr("Lowest GGPO input delay (frames, 2 to 6) a battle picks from the ping test. Also the delay of a replay takeover."));
	dialog()->registerWidgetHelp(m_ui.zdxsvGgpoPort, tr("Battle UDP Port"), tr("0"),
		tr("UDP port the emulator uses for online battles. 0: a random port (29700 to 29800) is picked at the next game start and saved here."));
	dialog()->registerWidgetHelp(m_ui.zdxsvNetOsd, tr("Network Status"), tr("Checked"),
		tr("Shows the network status overlay (one line per opponent) during online battles."));
	dialog()->registerWidgetHelp(m_ui.zdxsvUPnP, tr("UPnP"), tr("Checked"),
		tr("When the connectivity test at the first lobby connection finds the battle UDP port closed, asks the router (UPnP) to forward it to this PC, then tests again. The mapping is removed when PCSX2 exits."));
	dialog()->registerWidgetHelp(m_ui.zdxsvLowLatencyVsync, tr("Low-Latency Vsync"), tr("Checked"),
		tr("Presents the finished frame before the frame limiter sleeps, and polls input right before the next frame runs. Other games are never affected."));
	dialog()->registerWidgetHelp(m_ui.zdxsvSingleDrawList, tr("Single Draw List"), tr("Checked"),
		tr("The game draws each frame's picture in that frame instead of the next: one frame less from input to picture. Only this PC's picture changes; battles stay in sync with players who have it off. Other games are never affected."));
	dialog()->registerWidgetHelp(m_ui.zdxsvLiveAutoNext, tr("Live Auto-Next"), tr("Unchecked"),
		tr("Live spectating: when a stream ends, watch the lobby's next live battle."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplaySkipMs, tr("Skip Mobile Suit Selection"), tr("Checked"),
		tr("A replay runs at full speed up to the briefing."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplayKeyDisplay, tr("Key Display"), tr("Unchecked"),
		tr("Replays start with the key display on. The Toggle Key Display hotkey switches it."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplayBar, tr("Control Bar"), tr("When paused or the mouse moves"),
		tr("When the replay control bar is shown."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplayUpload, tr("Upload Replays"), tr("Checked"),
		tr("After each lobby battle, its replay is posted to the zdxsv server, where anyone can watch it (Tools → zdxsv Replays, Remote)."));
	dialog()->registerWidgetHelp(m_ui.zdxsvSaveReplay, tr("Save Replays"), tr("Checked"),
		tr("After each lobby battle, its replay is saved in the replay folder (Tools → zdxsv Replays, Local)."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplayPath, tr("Replay Folder"), tr("replays in the data folder"),
		tr("Where replays are saved and the Local replay list reads from. A folder that cannot be created falls back to the default."));

	connect(m_ui.zdxsvPresetLow, &QPushButton::clicked, this, [this]() { applyPreset(1.0f, 3); });
	connect(m_ui.zdxsvPresetMid, &QPushButton::clicked, this, [this]() { applyPreset(2.0f, 2); });
	connect(m_ui.zdxsvPresetHigh, &QPushButton::clicked, this, [this]() { applyPreset(3.0f, 2); });
	connect(m_ui.zdxsvUPnPNow, &QPushButton::clicked, this, &ZdxsvSettingsWidget::mapUpnpNow);
	connect(m_ui.zdxsvSignIn, &QPushButton::clicked, this, &ZdxsvSettingsWidget::signIn);
	connect(m_ui.zdxsvSignOut, &QPushButton::clicked, this, &ZdxsvSettingsWidget::signOut);
	updateAccount();
	dialog()->registerWidgetHelp(m_ui.zdxsvSignIn, tr("Sign in with Discord"), tr("None"),
		tr("Opens the zdxsv sign-in page in your browser. The first sign-in creates your account; a carry-over code from "
		   "the lobby on a PS2 links that account instead. PCSX2 keeps a sign-in token for this PC."));
	dialog()->registerWidgetHelp(m_ui.zdxsvSignOut, tr("Sign Out"), tr("None"),
		tr("Revokes this PC's sign-in token on the server and deletes it."));
	dialog()->registerWidgetHelp(m_ui.zdxsvUPnPNow, tr("UPnP Now"), tr("None"),
		tr("Asks the router (UPnP) now to forward the battle UDP port to this PC, and shows the result. The mapping is removed when PCSX2 exits."));
	dialog()->registerWidgetHelp(m_ui.zdxsvPresetLow, tr("Recommended Settings for Low-Spec PC"), tr("None"),
		tr("Sets Internal Resolution to Native (PS2) and Minimum Input Delay to 3."));
	dialog()->registerWidgetHelp(m_ui.zdxsvPresetMid, tr("Recommended Settings for Mid-Spec PC"), tr("None"),
		tr("Sets Internal Resolution to 2x Native (~720p) and Minimum Input Delay to 2."));
	dialog()->registerWidgetHelp(m_ui.zdxsvPresetHigh, tr("Recommended Settings for High-Spec PC"), tr("None"),
		tr("Sets Internal Resolution to 3x Native (~1080p) and Minimum Input Delay to 2."));
}

ZdxsvSettingsWidget::~ZdxsvSettingsWidget()
{
	if (m_sign_in_cancel)
		m_sign_in_cancel->store(true);
}

void ZdxsvSettingsWidget::updateAccount()
{
	const bool signed_in = Zdxsv::AuthTokenRead().has_value();
	const bool busy = m_sign_in_cancel != nullptr;
	if (!busy)
		m_ui.zdxsvAccountStatus->setText(signed_in ? tr("Signed in on this PC.") : tr("Not signed in. Online play needs a sign-in."));
	m_ui.zdxsvSignIn->setEnabled(!busy);
	m_ui.zdxsvCarryCode->setEnabled(!busy);
	m_ui.zdxsvSignOut->setEnabled(!busy && signed_in);
}

void ZdxsvSettingsWidget::signIn()
{
	m_sign_in_cancel = std::make_shared<std::atomic<bool>>(false);
	m_ui.zdxsvAccountStatus->setText(tr("Finish the sign-in in your browser..."));
	updateAccount();
	std::thread([widget = QPointer<ZdxsvSettingsWidget>(this), cancel = m_sign_in_cancel,
					carry = m_ui.zdxsvCarryCode->text().toStdString()]() {
		const std::string error = Zdxsv::AuthSignIn(carry, [](const std::string& url) { Host::OpenURL(url); }, *cancel);
		QtHost::RunOnUIThread([widget, cancel, error]() {
			if (!widget || widget->m_sign_in_cancel != cancel)
				return;
			widget->m_sign_in_cancel.reset();
			widget->updateAccount();
			if (!error.empty())
				widget->m_ui.zdxsvAccountStatus->setText(tr("Sign-in failed: %1").arg(QString::fromStdString(error)));
			else
				widget->m_ui.zdxsvCarryCode->clear();
		});
	}).detach();
}

void ZdxsvSettingsWidget::signOut()
{
	m_ui.zdxsvSignOut->setEnabled(false);
	std::thread([widget = QPointer<ZdxsvSettingsWidget>(this)]() {
		const std::string error = Zdxsv::AuthSignOut();
		QtHost::RunOnUIThread([widget, error]() {
			if (!widget)
				return;
			widget->updateAccount();
			if (!error.empty())
				widget->m_ui.zdxsvAccountStatus->setText(tr("Signed out on this PC; %1").arg(QString::fromStdString(error)));
		});
	}).detach();
}

void ZdxsvSettingsWidget::applyPreset(float upscale_multiplier, int min_delay)
{
	if (m_ui.zdxsvGgpoMinDelay->isEnabled())
		m_ui.zdxsvGgpoMinDelay->setValue(min_delay);
	// The Graphics page shows the value from its combo box and saves on a change: set it through the combo box.
	QComboBox* upscale = dialog()->findChild<QComboBox*>(QStringLiteral("upscaleMultiplier"));
	const int index = upscale ? upscale->findData(QVariant(upscale_multiplier)) : -1;
	if (index >= 0)
		upscale->setCurrentIndex(index);
	else
		dialog()->setFloatSettingValue("EmuCore/GS", "upscale_multiplier", upscale_multiplier);
}

void ZdxsvSettingsWidget::mapUpnpNow()
{
	const int port = m_ui.zdxsvGgpoPort->value();
	if (port <= 0 || port > 0xFFFF)
	{
		m_ui.zdxsvUPnPResult->setText(tr("Set a battle UDP port first (0 picks one at the next game start)."));
		return;
	}
	m_ui.zdxsvUPnPNow->setEnabled(false);
	m_ui.zdxsvUPnPResult->setText(tr("Please wait..."));
	std::thread([widget = QPointer<ZdxsvSettingsWidget>(this), port]() {
		std::string error;
		const bool mapped = Zdxsv::UpnpMapUdp(static_cast<uint16_t>(port), error);
		const QString result = mapped ? tr("UDP port %1 opened (until PCSX2 exits).").arg(port) : QString::fromStdString(error);
		QtHost::RunOnUIThread([widget, result]() {
			if (!widget)
				return;
			widget->m_ui.zdxsvUPnPNow->setEnabled(true);
			widget->m_ui.zdxsvUPnPResult->setText(result);
		});
	}).detach();
}

void ZdxsvSettingsWidget::markSyncForced(QWidget* window)
{
	// object names of the upstream pages' widgets; battle: forced only during battles, replays and live spectating
	static constexpr struct
	{
		const char* name;
		bool battle;
	} widgets[] = {{"cheats", false}, {"enableCheats", false}, {"eeRecompiler", false},
		{"iopRecompiler", false}, {"vu0Recompiler", false}, {"vu1Recompiler", false}, {"eeCache", false}, {"eeFastmem", false},
		{"pauseOnTLBMiss", false}, {"eeRoundingMode", false}, {"eeDivRoundingMode", false}, {"vu0RoundingMode", false},
		{"vu1RoundingMode", false}, {"eeClampMode", false}, {"vu0ClampMode", false}, {"vu1ClampMode", false},
		{"extraMemory", false}, {"gameFixes", false}, {"gsDownloadMode", false}, {"eeCycleRate", true},
		{"eeCycleSkipping", true}, {"MTVU", true}, {"fastCDVD", true}, {"eeINTCSpinDetection", true},
		{"eeWaitLoopDetection", true}, {"vuFlagHack", true}, {"instantVU1", true}, {"normalSpeed", true},
		{"syncToHostRefreshRate", true}};
	std::vector<std::pair<QWidget*, bool>> found;
	for (const auto& w : widgets)
		// findChildren: the Folders page has a QLineEdit "cheats" (a path, never forced)
		for (QWidget* widget : window->findChildren<QWidget*>(QString::fromUtf8(w.name)))
			if (!qobject_cast<QLineEdit*>(widget))
				found.emplace_back(widget, w.battle);
	if (GameFixSettingsWidget* fixes = window->findChild<GameFixSettingsWidget*>())
		found.emplace_back(fixes, false);
	const auto update = [found]() {
		for (const auto& [widget, battle] : found)
		{
			const bool forced = Zdxsv::SyncSettingsForced(battle);
			if (forced == widget->property("zdxsvSyncForced").toBool())
				continue;
			widget->setProperty("zdxsvSyncForced", forced);
			widget->setEnabled(!forced);
			widget->setToolTip(!forced ? QString() :
			                   battle  ? tr("Set to the default during online battles, replays and live spectating "
			                                "(the same on every player's emulator).") :
			                             tr("Set to the default while Gundam vs. Z Gundam runs (the same on every "
			                                "player's emulator)."));
		}
	};
	update();
	QTimer* timer = new QTimer(window);
	connect(timer, &QTimer::timeout, window, update);
	timer->start(1000);
}

#include "moc_ZdxsvSettingsWidget.cpp"

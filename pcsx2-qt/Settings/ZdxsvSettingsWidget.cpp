// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ZdxsvSettingsWidget.h"
#include "SettingWidgetBinder.h"
#include "GameFixSettingsWidget.h"
#include "SettingsWindow.h"

#include "pcsx2/Zdxsv/Settings.h"
#include "pcsx2/Zdxsv/SyncSettings.h"

#include <QtCore/QTimer>

#include <cstdlib>
#include <cstring>
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

	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvGgpo, "DEV9/Eth", "ZdxsvGgpo", true);
	SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.zdxsvGgpoMinDelay, "DEV9/Eth", "ZdxsvGgpoMinDelay", 2);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvNetOsd, "DEV9/Eth", "ZdxsvNetOsd", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvLowLatencyVsync, "EmuCore/GS", "ZdxsvLowLatencyVsync", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvLiveAutoNext, "DEV9/Eth", "ZdxsvLiveAutoNext", false);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvReplaySkipMs, "DEV9/Eth", "ZdxsvReplaySkipMs", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvReplayKeyDisplay, "DEV9/Eth", "ZdxsvReplayKeyDisplay", false);
	SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.zdxsvReplayBar, "DEV9/Eth", "ZdxsvReplayBar", 0);
	SettingWidgetBinder::BindWidgetToStringSetting(
		sif, m_ui.zdxsvReplayStateUrl, "DEV9/Eth", "ZdxsvReplayStateUrl", Zdxsv::REPLAY_STATE_URL);

	// ZDXSV_GGPO replaces the options of the GGPO setting; its mindelay= and osd= keys override those settings.
	MarkEnvOverride(m_ui.zdxsvGgpo, "ZDXSV_GGPO");
	MarkEnvOverride(m_ui.zdxsvGgpoMinDelay, "ZDXSV_GGPO", "mindelay=");
	MarkEnvOverride(m_ui.zdxsvNetOsd, "ZDXSV_GGPO", "osd=");
	MarkEnvOverride(m_ui.zdxsvLiveAutoNext, "ZDXSV_LIVE_NEXT");
	MarkEnvOverride(m_ui.zdxsvReplaySkipMs, "ZDXSV_REPLAY_SKIP_MS");
	MarkEnvOverride(m_ui.zdxsvReplayKeyDisplay, "ZDXSV_REPLAY_KEY_DISPLAY");
	MarkEnvOverride(m_ui.zdxsvReplayBar, "ZDXSV_REPLAY_BAR");
	MarkEnvOverride(m_ui.zdxsvReplayStateUrl, "ZDXSV_REPLAY_STATE");

	dialog()->registerWidgetHelp(m_ui.zdxsvGgpo, tr("Rollback Netcode (GGPO)"), tr("Checked"),
		tr("Online battles of the game run over GGPO rollback netcode. Takes effect at the next game start. Other games are never affected."));
	dialog()->registerWidgetHelp(m_ui.zdxsvGgpoMinDelay, tr("Minimum Input Delay"), tr("2"),
		tr("Lowest GGPO input delay (frames, 2 to 6) a battle picks from the ping test. Also the delay of a replay takeover."));
	dialog()->registerWidgetHelp(m_ui.zdxsvNetOsd, tr("Network Status"), tr("Checked"),
		tr("Shows the network status overlay (one line per opponent) during online battles."));
	dialog()->registerWidgetHelp(m_ui.zdxsvLowLatencyVsync, tr("Low-Latency Vsync"), tr("Checked"),
		tr("Presents the finished frame before the frame limiter sleeps, and polls input right before the next frame runs. Other games are never affected."));
	dialog()->registerWidgetHelp(m_ui.zdxsvLiveAutoNext, tr("Live Auto-Next"), tr("Unchecked"),
		tr("Live spectating: when a stream ends, watch the lobby's next live battle."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplaySkipMs, tr("Skip Mobile Suit Selection"), tr("Checked"),
		tr("A replay runs at full speed up to the briefing."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplayKeyDisplay, tr("Key Display"), tr("Unchecked"),
		tr("Replays start with the key display on. The Toggle Key Display hotkey switches it."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplayBar, tr("Control Bar"), tr("When paused or the mouse moves"),
		tr("When the replay control bar is shown."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplayStateUrl, tr("Start State"), QString::fromUtf8(Zdxsv::REPLAY_STATE_URL),
		tr("Save state replays and live battles start from: a URL (downloaded once into the cache folder) or a file. Empty: the state of the running game."));
}

ZdxsvSettingsWidget::~ZdxsvSettingsWidget() = default;

void ZdxsvSettingsWidget::markSyncForced(QWidget* window)
{
	// object names of the upstream pages' widgets; battle: forced only during battles, replays and live spectating
	static constexpr struct
	{
		const char* name;
		bool battle;
	} widgets[] = {{"cheats", false}, {"pineEnable", false}, {"pineSlot", false}, {"eeRecompiler", false},
		{"iopRecompiler", false}, {"vu0Recompiler", false}, {"vu1Recompiler", false}, {"eeCache", false}, {"eeFastmem", false},
		{"pauseOnTLBMiss", false}, {"eeRoundingMode", false}, {"eeDivRoundingMode", false}, {"vu0RoundingMode", false},
		{"vu1RoundingMode", false}, {"eeClampMode", false}, {"vu0ClampMode", false}, {"vu1ClampMode", false},
		{"extraMemory", false}, {"gameFixes", false}, {"gsDownloadMode", false}, {"eeCycleRate", true},
		{"eeCycleSkipping", true}, {"MTVU", true}, {"fastCDVD", true}, {"eeINTCSpinDetection", true},
		{"eeWaitLoopDetection", true}, {"vuFlagHack", true}, {"instantVU1", true}, {"normalSpeed", true}};
	std::vector<std::pair<QWidget*, bool>> found;
	for (const auto& w : widgets)
		if (QWidget* widget = window->findChild<QWidget*>(QString::fromUtf8(w.name)))
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

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
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QFontDatabase>
#include <QtGui/QRegularExpressionValidator>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// ZdxsvAccount -1: an empty account slot, made at the first login (pcsx2/Zdxsv/FastLogin.cpp).
static constexpr int kNewAccount = -1;

// The values of ZdxsvStickMessageButton (pcsx2/Zdxsv/StickMessage.cpp).
static const char* s_stick_message_values[] = {
	"None", "Start", "Select", "L1", "L2", "R1", "R2", "L3", "R3", "Triangle", "Circle", "Cross", "Square", nullptr};
static const char* s_stick_message_names[] = {
	QT_TRANSLATE_NOOP("ZdxsvSettingsWidget", "Off"),
	QT_TRANSLATE_NOOP("ZdxsvSettingsWidget", "Start"),
	QT_TRANSLATE_NOOP("ZdxsvSettingsWidget", "Select"),
	"L1",
	"L2",
	"R1",
	"R2",
	"L3",
	"R3",
	QT_TRANSLATE_NOOP("ZdxsvSettingsWidget", "Triangle"),
	QT_TRANSLATE_NOOP("ZdxsvSettingsWidget", "Circle"),
	QT_TRANSLATE_NOOP("ZdxsvSettingsWidget", "Cross"),
	QT_TRANSLATE_NOOP("ZdxsvSettingsWidget", "Square"),
	nullptr};

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
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvFrameOsd, "DEV9/Eth", "ZdxsvFrameOsd", false);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvAllyHp, "DEV9/Eth", "ZdxsvAllyHp", true);
	SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.zdxsvStickMessageButton, "DEV9/Eth", "ZdxsvStickMessageButton",
		s_stick_message_names, s_stick_message_values, "Start", "ZdxsvSettingsWidget");
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvUPnP, "DEV9/Eth", "ZdxsvUPnP", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvLiveAutoNext, "DEV9/Eth", "ZdxsvLiveAutoNext", false);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvReplaySkipMs, "DEV9/Eth", "ZdxsvReplaySkipMs", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvReplayKeyDisplay, "DEV9/Eth", "ZdxsvReplayKeyDisplay", true);
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvSaveReplay, "DEV9/Eth", "ZdxsvSaveReplay", true);
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.zdxsvReplayPath, m_ui.zdxsvReplayPathBrowse, m_ui.zdxsvReplayPathOpen,
		m_ui.zdxsvReplayPathReset, "DEV9/Eth", "ZdxsvReplayPath", Path::Combine(EmuFolders::DataRoot, "replays"));

	if (Zdxsv::CrashReportsAvailable() && !dialog()->isPerGameSettings())
	{
		SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.zdxsvCrashReports, "DEV9/Eth", "ZdxsvCrashReports", true);
		connect(m_ui.zdxsvCrashReports, &QCheckBox::checkStateChanged, this,
			[](Qt::CheckState state) { Zdxsv::CrashReportsSetEnabled(state == Qt::Checked); });
		dialog()->registerWidgetHelp(m_ui.zdxsvCrashReports, tr("Crash Reports"), tr("Checked"),
			tr("Sends a report to the zdxsv developers when PCSX2 crashes."));
	}
	else
	{
		m_ui.zdxsvCrashReports->hide();
	}

	MarkEnvOverride(m_ui.zdxsvFastBoot, "ZDXSV_FAST_BOOT");
	// The mindelay=, port=, osd=, upload= and replay= keys of ZDXSV_GGPO override those settings.
	MarkEnvOverride(m_ui.zdxsvGgpoMinDelay, "ZDXSV_GGPO", "mindelay=");
	MarkEnvOverride(m_ui.zdxsvGgpoPort, "ZDXSV_GGPO", "port=");
	MarkEnvOverride(m_ui.zdxsvNetOsd, "ZDXSV_GGPO", "osd=");
	MarkEnvOverride(m_ui.zdxsvFrameOsd, "ZDXSV_FRAME_OSD");
	MarkEnvOverride(m_ui.zdxsvAllyHp, "ZDXSV_ALLY_HP");
	MarkEnvOverride(m_ui.zdxsvLiveAutoNext, "ZDXSV_LIVE_NEXT");
	MarkEnvOverride(m_ui.zdxsvReplaySkipMs, "ZDXSV_REPLAY_SKIP_MS");
	MarkEnvOverride(m_ui.zdxsvReplayKeyDisplay, "ZDXSV_REPLAY_KEY_DISPLAY");
	MarkEnvOverride(m_ui.zdxsvSaveReplay, "ZDXSV_GGPO", "replay=");
	MarkEnvOverride(m_ui.zdxsvReplayPath, "ZDXSV_GGPO", "replay=");

	dialog()->registerWidgetHelp(m_ui.zdxsvFastBoot, tr("Fast Boot"), tr("Checked"),
		tr("Goes from game start to the lobby ID selection with no input. Uncheck it to play offline."));
	dialog()->registerWidgetHelp(m_ui.zdxsvGgpoMinDelay, tr("Minimum Input Delay"), tr("2"),
		tr("Lowest input delay of a battle (frames). Higher: fewer rollbacks, but slower controls."));
	dialog()->registerWidgetHelp(m_ui.zdxsvGgpoPort, tr("Battle UDP Port"), tr("0"),
		tr("UDP port for online battles. 0: picked at random. Forward it on your router if you cannot connect."));
	dialog()->registerWidgetHelp(m_ui.zdxsvNetOsd, tr("Network Status"), tr("Checked"),
		tr("Shows the connection status during online battles."));
	dialog()->registerWidgetHelp(m_ui.zdxsvFrameOsd, tr("Frame Time"), tr("Unchecked"),
		tr("Shows during online battles and replays where each frame's time goes: emulation, rollback, save/load, network wait, sleep; and the GS thread, VU thread and GPU time per frame."));
	dialog()->registerWidgetHelp(m_ui.zdxsvAllyHp, tr("Teammate HP"), tr("Checked"),
		tr("In battle, your HP display shows your teammate's current HP where it shows your max HP."));
	dialog()->registerWidgetHelp(m_ui.zdxsvStickMessageButton, tr("Team Messages Without a Right Stick"), tr("Start"),
		tr("While this button is held, the D-pad or the left stick also tilts the right stick, which sends the team "
		   "messages. The button and the D-pad still work as usual in the game. Off: only the right stick sends them."));
	dialog()->registerWidgetHelp(m_ui.zdxsvUPnP, tr("UPnP"), tr("Checked"),
		tr("When the battle port is closed, automatically changes the router's settings."));
	dialog()->registerWidgetHelp(m_ui.zdxsvLiveAutoNext, tr("Live Auto-Next"), tr("Unchecked"),
		tr("When a live battle ends, watches the next one."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplaySkipMs, tr("Skip Mobile Suit Selection"), tr("Checked"),
		tr("Replays skip the mobile suit selection and rematch screens."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplayKeyDisplay, tr("Key Display"), tr("Unchecked"),
		tr("Replays start with the key display on."));
	dialog()->registerWidgetHelp(m_ui.zdxsvSaveReplay, tr("Save Replays"), tr("Checked"),
		tr("Saves each lobby battle's replay on this PC."));
	dialog()->registerWidgetHelp(m_ui.zdxsvReplayPath, tr("Replay Folder"), tr("replays in the data folder"),
		tr("Where replays are saved."));

	connect(m_ui.zdxsvPresetLow, &QPushButton::clicked, this, [this]() { applyPreset(1.0f, 3); });
	connect(m_ui.zdxsvPresetMid, &QPushButton::clicked, this, [this]() { applyPreset(2.0f, 2); });
	connect(m_ui.zdxsvPresetHigh, &QPushButton::clicked, this, [this]() { applyPreset(3.0f, 2); });
	connect(m_ui.zdxsvUPnPNow, &QPushButton::clicked, this, &ZdxsvSettingsWidget::mapUpnpNow);
	connect(m_ui.zdxsvSignIn, &QPushButton::clicked, this, &ZdxsvSettingsWidget::signIn);
	connect(m_ui.zdxsvSignOut, &QPushButton::clicked, this, &ZdxsvSettingsWidget::signOut);
	connect(m_ui.zdxsvAccountRefresh, &QPushButton::clicked, this, [this]() { loadAccounts(AccountRequest::List); });
	connect(m_ui.zdxsvCarry, &QPushButton::clicked, this, &ZdxsvSettingsWidget::carryOver);
	connect(m_ui.zdxsvAccountPick, &QComboBox::activated, this, &ZdxsvSettingsWidget::pickAccount);
	updateAccount();
	if (Zdxsv::AuthTokenRead())
		loadAccounts(AccountRequest::List);
	dialog()->registerWidgetHelp(m_ui.zdxsvAccountPick, tr("Account"), tr("The first one"),
		tr("The account used online."));
	dialog()->registerWidgetHelp(m_ui.zdxsvSignIn, tr("Sign in with Discord"), tr("None"),
		tr("Signs in to zdxsv in your browser."));
	dialog()->registerWidgetHelp(m_ui.zdxsvCarry, tr("Carry Over from PS2..."), tr("None"),
		tr("Uses your PS2 account on this PC too. Get the code by typing ｄｃ in the Kennedy Port lobby chat on the PS2."));
	dialog()->registerWidgetHelp(m_ui.zdxsvSignOut, tr("Sign Out"), tr("None"),
		tr("Revokes this PC's sign-in token on the server and deletes it."));
	dialog()->registerWidgetHelp(m_ui.zdxsvUPnPNow, tr("UPnP Now"), tr("None"),
		tr("Opens the battle port on the router now."));
	dialog()->registerWidgetHelp(m_ui.zdxsvPresetLow, tr("Recommended Settings for Low-Spec PC"), tr("None"),
		tr("Native resolution, input delay 3."));
	dialog()->registerWidgetHelp(m_ui.zdxsvPresetMid, tr("Recommended Settings for Mid-Spec PC"), tr("None"),
		tr("2x resolution, input delay 2."));
	dialog()->registerWidgetHelp(m_ui.zdxsvPresetHigh, tr("Recommended Settings for High-Spec PC"), tr("None"),
		tr("3x resolution, input delay 2."));
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
	m_ui.zdxsvSignIn->setText(busy ? tr("Cancel Sign-in") : tr("Sign in with Discord"));
	m_ui.zdxsvSignOut->setEnabled(!busy && signed_in);
	m_ui.zdxsvAccountPick->setEnabled(!busy && signed_in && m_ui.zdxsvAccountPick->count() > 0);
	m_ui.zdxsvAccountRefresh->setEnabled(!busy && signed_in);
	m_ui.zdxsvCarry->setEnabled(!busy && signed_in && m_carry_room);
	if (!signed_in)
		m_ui.zdxsvAccountPick->clear();
}

static void SetPickedAccount(int account)
{
	Host::SetBaseIntSettingValue("DEV9/Eth", "ZdxsvAccount", account);
	Host::CommitBaseSettingChanges();
}

void ZdxsvSettingsWidget::carryOver()
{
	QDialog dialog(this);
	dialog.setWindowTitle(tr("Carry Over from PS2"));
	QLabel* text = new QLabel(tr("Use the account you play on a PS2 on this PC too. The PS2 keeps playing with the same "
								 "account.\n\n"
								 "Steps:\n"
								 "1. On the PS2, enter the Kennedy Port lobby.\n"
								 "2. Type ｄｃ in the chat: a carry-over code is shown.\n"
								 "3. Within 3 minutes, enter the code below."),
		&dialog);
	text->setWordWrap(true);
	QLineEdit* code = new QLineEdit(&dialog);
	code->setMaxLength(8);
	code->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z0-9]{0,8}")), code));
	code->setAlignment(Qt::AlignCenter);
	code->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	code->setPlaceholderText(QStringLiteral("XXXXXXXX"));
	QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QVBoxLayout* layout = new QVBoxLayout(&dialog);
	layout->addWidget(text);
	layout->addWidget(code);
	layout->addWidget(buttons);
	dialog.setFixedWidth(360);
	if (dialog.exec() == QDialog::Accepted && !code->text().isEmpty())
		loadAccounts(AccountRequest::Carry, code->text().toUpper().toStdString());
}

void ZdxsvSettingsWidget::loadAccounts(AccountRequest request, const std::string& carry_code)
{
	m_ui.zdxsvAccountRefresh->setEnabled(false);
	m_ui.zdxsvCarry->setEnabled(false);
	std::thread([widget = QPointer<ZdxsvSettingsWidget>(this), request, code = carry_code]() {
		Zdxsv::AuthAccountList list;
		const std::string error =
			request == AccountRequest::Carry ? Zdxsv::AuthCarry(code, &list) : Zdxsv::AuthAccounts(&list);
		QtHost::RunOnUIThread([widget, request, error, list]() {
			if (!widget)
				return;
			if (!error.empty())
			{
				widget->updateAccount();
				widget->m_ui.zdxsvAccountStatus->setText(tr("Accounts: %1").arg(QString::fromStdString(error)));
				return;
			}
			if (list.added > 0)
				SetPickedAccount(list.added);
			int picked = Host::GetBaseIntSettingValue("DEV9/Eth", "ZdxsvAccount", 0);
			int made = 0, carried = 0;
			for (bool c : list.carried)
				(c ? carried : made)++;
			const bool room = made < list.max_new;
			widget->m_carry_room = carried < list.max_carried;
			if ((picked == kNewAccount && !room) || (picked != kNewAccount && (picked < 1 || picked > static_cast<int>(list.users.size()))))
				picked = list.default_account;
			if (list.users.empty() && room)
				picked = kNewAccount;
			QComboBox* combo = widget->m_ui.zdxsvAccountPick;
			combo->clear();
			for (size_t i = 0; i < list.users.size(); i++)
			{
				QStringList names;
				for (const std::string& name : list.users[i])
					names.append(QString::fromStdString(name));
				QString text = tr("Account %1").arg(i + 1);
				if (!names.isEmpty())
					text += QStringLiteral(": ") + names.join(QStringLiteral(", "));
				combo->addItem(text, static_cast<int>(i + 1));
			}
			for (int i = made; i < list.max_new; i++)
				combo->addItem(tr("Account %1: not made yet (made at the first connection)").arg(combo->count() + 1), kNewAccount);
			combo->setCurrentIndex(combo->findData(picked));
			widget->updateAccount();
			if (list.users.empty())
				widget->m_ui.zdxsvAccountStatus->setText(
					tr("Signed in on this PC. Your zdxsv account is made when you first connect; to use your PS2 account, "
					   "carry it over first."));
		});
	}).detach();
}

void ZdxsvSettingsWidget::pickAccount(int index)
{
	const QVariant account = m_ui.zdxsvAccountPick->itemData(index);
	if (!account.isValid())
		return;
	SetPickedAccount(account.toInt());
	loadAccounts(AccountRequest::List);
}

void ZdxsvSettingsWidget::signIn()
{
	if (m_sign_in_cancel)
	{
		// The browser may never come back (its page closed): the wait would hold the buttons for minutes.
		m_sign_in_cancel->store(true);
		m_sign_in_cancel.reset();
		updateAccount();
		m_ui.zdxsvAccountStatus->setText(tr("Sign-in cancelled."));
		return;
	}
	m_sign_in_cancel = std::make_shared<std::atomic<bool>>(false);
	m_ui.zdxsvAccountStatus->setText(tr("Finish the sign-in in your browser..."));
	updateAccount();
	std::thread([widget = QPointer<ZdxsvSettingsWidget>(this), cancel = m_sign_in_cancel]() {
		const std::string error = Zdxsv::AuthSignIn(Zdxsv::AuthOpenBrowser, *cancel);
		QtHost::RunOnUIThread([widget, cancel, error]() {
			if (!widget || widget->m_sign_in_cancel != cancel)
				return;
			widget->m_sign_in_cancel.reset();
			widget->updateAccount();
			if (!error.empty())
				widget->m_ui.zdxsvAccountStatus->setText(tr("Sign-in failed: %1").arg(QString::fromStdString(error)));
			else
			{
				SetPickedAccount(0);
				widget->loadAccounts(AccountRequest::List);
			}
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
			SetPickedAccount(0);
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

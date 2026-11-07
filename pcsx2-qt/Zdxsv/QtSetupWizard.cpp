// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/QtHooks.h"

#include "QtHost.h"

#include "pcsx2/Host.h"
#include "pcsx2/Zdxsv/AuthSignIn.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>

#include <atomic>
#include <memory>
#include <thread>

namespace Zdxsv
{
	namespace
	{
		QString Tr(const char* s)
		{
			return QCoreApplication::translate("ZdxsvSetupWizard", s);
		}

		// Shared with the sign-in on Settings > zdxsv.
		QString TrSettings(const char* s)
		{
			return QCoreApplication::translate("ZdxsvSettingsWidget", s);
		}

		QString Heading(const QString& text)
		{
			return QStringLiteral("<h1 style=\"margin-top:18px; margin-bottom:12px;\"><span style=\"font-size:xx-large; "
								  "font-weight:700;\">%1</span></h1>")
				.arg(text);
		}

		void SetText(QWidget* wizard, const char* name, const QString& text)
		{
			if (QLabel* label = wizard->findChild<QLabel*>(QString::fromLatin1(name)))
				label->setText(text);
		}

		void Hide(QWidget* wizard, const char* name)
		{
			if (QWidget* widget = wizard->findChild<QWidget*>(QString::fromLatin1(name)))
				widget->hide();
		}

		class SignInPanel : public QWidget
		{
		public:
			explicit SignInPanel(QWidget* parent)
				: QWidget(parent)
			{
				QVBoxLayout* layout = new QVBoxLayout(this);
				layout->setContentsMargins(0, 0, 0, 0);
				m_status = new QLabel(this);
				m_status->setWordWrap(true);
				m_button = new QPushButton(this);
				layout->addWidget(m_status);
				layout->addWidget(m_button, 0, Qt::AlignLeft);
				connect(m_button, &QPushButton::clicked, this, [this]() { signIn(); });
				update(QString());
			}

			~SignInPanel() override
			{
				if (m_cancel)
					m_cancel->store(true);
			}

		private:
			void update(const QString& status)
			{
				const bool busy = m_cancel != nullptr;
				const bool signed_in = AuthTokenRead().has_value();
				m_button->setText(busy ? TrSettings("Cancel Sign-in") : TrSettings("Sign in with Discord"));
				m_button->setEnabled(busy || !signed_in);
				if (!status.isEmpty())
					m_status->setText(status);
				else
					m_status->setText(signed_in ? TrSettings("Signed in on this PC.") :
												  TrSettings("Not signed in. Online play needs a sign-in."));
			}

			void signIn()
			{
				if (m_cancel)
				{
					m_cancel->store(true);
					m_cancel.reset();
					update(TrSettings("Sign-in cancelled."));
					return;
				}
				m_cancel = std::make_shared<std::atomic<bool>>(false);
				update(TrSettings("Finish the sign-in in your browser..."));
				std::thread([panel = QPointer<SignInPanel>(this), cancel = m_cancel]() {
					const std::string error = AuthSignIn(AuthOpenBrowser, *cancel);
					QtHost::RunOnUIThread([panel, cancel, error]() {
						if (!panel || panel->m_cancel != cancel)
							return;
						panel->m_cancel.reset();
						if (error.empty())
						{
							Host::SetBaseIntSettingValue("DEV9/Eth", "ZdxsvAccount", 0);
							Host::CommitBaseSettingChanges();
						}
						panel->update(error.empty() ? QString() :
													  TrSettings("Sign-in failed: %1").arg(QString::fromStdString(error)));
					});
				}).detach();
			}

			QLabel* m_status;
			QPushButton* m_button;
			std::shared_ptr<std::atomic<bool>> m_cancel;
		};
	} // namespace

	void QtSetupWizardCustomize(QWidget* wizard)
	{
		SetText(wizard, "IntroLabel",
			Heading(Tr("Welcome to PCSX2 for zdxsv!")) +
				Tr("<p>This PCSX2 plays Mobile Suit Gundam: Gundam vs. Zeta Gundam online on zdxsv. This wizard sets up the "
				   "BIOS, the game, the controller and your zdxsv account.</p><p>At each start, PCSX2 checks "
				   "github.com/inada-s/pcsx2 for a new release. To stop it, uncheck Enable Automatic Updates below.</p>"
				   "<p>Choose a language and a theme to begin.</p>"));
		SetText(wizard, "dumpsIntro",
			Tr("<p>Add the folder that holds the game's disc image (ISO, CHD and others), made from your own disc. "
			   "PCSX2 lists the games it finds there.</p>"));

		SetText(wizard, "controllerIntro",
			Tr("<p>By default, the keyboard is mapped to the PS2 controller.</p><p><b>To use a gamepad, connect it, "
			   "press Automatic Mapping and choose your controller.</b></p><p>To change the bindings in detail, open "
			   "Settings → Controllers after the setup.</p>"));

		SetText(wizard, "labelRetroAchievements", Tr("zdxsv Account"));
		SetText(wizard, "raSectionLabel", Tr("zdxsv Account"));
		SetText(wizard, "raIntroLabel",
			Tr("<p>Online play on zdxsv needs a sign-in with your Discord account. Sign in now, or later on "
			   "Settings → zdxsv.</p>"));
		for (const char* name : {"raLogo", "raIntegrationLabel", "raContentWidget"})
			Hide(wizard, name);
		QLabel* intro = wizard->findChild<QLabel*>(QStringLiteral("raIntroLabel"));
		QBoxLayout* layout = intro ? qobject_cast<QBoxLayout*>(intro->parentWidget()->layout()) : nullptr;
		if (layout)
			layout->insertWidget(layout->indexOf(intro) + 1, new SignInPanel(intro->parentWidget()));

		SetText(wizard, "completeIntro",
			Heading(Tr("Setup Complete!")) +
				Tr("<p>Start the game from the game list. The online settings are on Settings → zdxsv.</p>"
				   "<p><b>Warning:</b> Do not run untrusted programs in PCSX2. It is not a sandbox and cannot protect "
				   "your computer from malicious software.</p>"));
	}

	bool QtSetupWizardControllerConfirm(QWidget* wizard)
	{
		const std::string cross = Host::GetBaseStringSettingValue("Pad1", "Cross");
		if (!cross.empty() && !cross.starts_with("Keyboard/"))
			return true;
		return QMessageBox::question(wizard, Tr("Keyboard"),
				   Tr("Controller Port 1 is on the keyboard. To use a gamepad, press Automatic Mapping and choose your "
					  "controller.\n\nContinue with the keyboard?")) == QMessageBox::Yes;
	}
} // namespace Zdxsv

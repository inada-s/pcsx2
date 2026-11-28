// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/QtHooks.h"

#include "pcsx2/Host.h"

#include <QtCore/QEvent>
#include <QtCore/QTimer>
#include <QtGui/QCursor>
#include <QtGui/QWindow>

namespace Zdxsv
{
	namespace
	{
		constexpr int CursorMoveShowMs = 3000;
		constexpr const char* CursorMoveWatcherName = "ZdxsvCursorMoveWatcher";

		class CursorMoveWatcher final : public QObject
		{
		public:
			explicit CursorMoveWatcher(QWindow* surface)
				: QObject(surface)
				, m_surface(surface)
			{
				setObjectName(QLatin1StringView(CursorMoveWatcherName));
				m_timer.setSingleShot(true);
				m_timer.setInterval(CursorMoveShowMs);
				connect(&m_timer, &QTimer::timeout, this, &CursorMoveWatcher::Hide);
			}

			void SetActive(bool active)
			{
				if (m_active == active)
					return;
				m_active = active;
				if (active)
				{
					m_last_pos = QCursor::pos();
					m_surface->installEventFilter(this);
				}
				else
				{
					m_surface->removeEventFilter(this);
					m_timer.stop();
				}
			}

			void HideIfShown()
			{
				if (m_shown)
					Hide();
			}

			void ForgetShown()
			{
				m_shown = false;
			}

		protected:
			bool eventFilter(QObject* object, QEvent* event) override
			{
				if (object == m_surface && event->type() == QEvent::MouseMove)
				{
					// Changing the cursor can send a move event without a move.
					const QPoint pos = QCursor::pos();
					if (pos != m_last_pos)
					{
						m_last_pos = pos;
						if (!m_shown)
						{
							m_shown = true;
							m_surface->unsetCursor();
						}
						m_timer.start();
					}
				}
				return false;
			}

		private:
			void Hide()
			{
				m_timer.stop();
				m_shown = false;
				m_surface->setCursor(Qt::BlankCursor);
			}

			QWindow* m_surface;
			QTimer m_timer;
			QPoint m_last_pos;
			bool m_active = false;
			bool m_shown = false;
		};
	} // namespace

	void QtCursorMoveUpdate(QWindow* surface, bool hidden, bool fullscreen_option_only)
	{
		// No Q_OBJECT: findChild by type would match any QObject child.
		CursorMoveWatcher* watcher = static_cast<CursorMoveWatcher*>(
			surface->findChild<QObject*>(QLatin1StringView(CursorMoveWatcherName), Qt::FindDirectChildrenOnly));
		const bool active = hidden && fullscreen_option_only && Host::GetBoolSettingValue("UI", "ZdxsvCursorShowOnMove", true);
		if (!watcher)
		{
			if (!active)
				return;
			watcher = new CursorMoveWatcher(surface);
		}

		watcher->SetActive(active);
		if (active)
			return;
		if (hidden)
			watcher->HideIfShown();
		else
			watcher->ForgetShown();
	}
} // namespace Zdxsv

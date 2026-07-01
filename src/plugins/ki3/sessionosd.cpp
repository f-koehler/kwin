/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "sessionosd.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QToolButton>

namespace KWin
{

namespace
{
struct ActionInfo
{
    SessionOsd::Action action;
    const char *label;
    const char *iconName;
};

constexpr ActionInfo s_actions[] = {
    {SessionOsd::Action::Lock, "Lock", "system-lock-screen"},
    {SessionOsd::Action::Logout, "Logout", "system-log-out"},
    {SessionOsd::Action::Suspend, "Suspend", "system-suspend"},
    {SessionOsd::Action::Restart, "Restart", "system-reboot"},
    {SessionOsd::Action::Shutdown, "Shutdown", "system-shutdown"},
};
}

SessionOsd::SessionOsd(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setFocusPolicy(Qt::StrongFocus);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    for (const ActionInfo &info : s_actions) {
        auto *button = new QToolButton(this);
        button->setText(QString::fromUtf8(info.label));
        button->setFocusPolicy(Qt::NoFocus);
        button->setMinimumSize(96, 72);

        const QIcon icon = QIcon::fromTheme(QString::fromUtf8(info.iconName));
        if (icon.isNull()) {
            button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        } else {
            button->setIcon(icon);
            button->setIconSize(QSize(32, 32));
            button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        }

        const SessionOsd::Action action = info.action;
        connect(button, &QToolButton::clicked, this, [this, action]() {
            Q_EMIT triggered(action);
            close();
        });

        layout->addWidget(button);
        m_buttons << button;
    }

    setSelected(0);
}

void SessionOsd::popup(const Rect &outputGeometry)
{
    setSelected(0);
    adjustSize();

    const QRect screen = outputGeometry;
    move(screen.center() - QPoint(width() / 2, height() / 2));
    show();
    setFocus();
}

void SessionOsd::setSelected(int index)
{
    if (m_buttons.isEmpty()) {
        return;
    }
    m_selected = (index + m_buttons.size()) % m_buttons.size();
    for (int i = 0; i < m_buttons.size(); ++i) {
        m_buttons[i]->setStyleSheet(i == m_selected
                ? QStringLiteral("QToolButton { border: 2px solid palette(highlight); border-radius: 4px; }")
                : QStringLiteral("QToolButton { border: 2px solid transparent; }"));
    }
}

void SessionOsd::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_H:
        setSelected(m_selected - 1);
        return;
    case Qt::Key_Right:
    case Qt::Key_L:
        setSelected(m_selected + 1);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        Q_EMIT triggered(s_actions[m_selected].action);
        close();
        return;
    case Qt::Key_Escape:
        close();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

}

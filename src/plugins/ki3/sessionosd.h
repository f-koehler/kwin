/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/rect.h"

#include <QList>
#include <QWidget>

class QToolButton;

namespace KWin
{

/**
 * A single combined "what do you want to do with the session" OSD (i3/sway
 * Meta+Shift+E): Lock / Logout / Suspend / Restart / Shutdown, navigated with
 * Left/Right (or h/l), confirmed with Enter, cancelled with Escape or a click
 * outside.
 *
 * Shown as a KWin "internal window" with the Qt::Popup flag, which KWin's
 * always-installed PopupInputFilter (src/popup_input_filter.cpp) picks up
 * automatically: it grants real keyboard focus, forwards all key events here,
 * and closes the popup on an outside click -- no custom input handling needed
 * on ki3's side.
 */
class SessionOsd : public QWidget
{
    Q_OBJECT

public:
    enum class Action {
        Lock,
        Logout,
        Suspend,
        Restart,
        Shutdown,
    };
    Q_ENUM(Action)

    explicit SessionOsd(QWidget *parent = nullptr);

    /** Show, centered on @p outputGeometry, with the selection reset to Lock. */
    void popup(const Rect &outputGeometry);

Q_SIGNALS:
    void triggered(KWin::SessionOsd::Action action);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void setSelected(int index);

    QList<QToolButton *> m_buttons;
    int m_selected = 0;
};

}

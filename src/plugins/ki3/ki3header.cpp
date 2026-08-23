/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3header.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>

namespace KWin
{

Ki3SolidOverlay::Ki3SolidOverlay(bool acceptsInput)
    : m_acceptsInput(acceptsInput)
{
    Qt::WindowFlags flags = Qt::BypassWindowManagerHint | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus;
    if (!acceptsInput) {
        flags |= Qt::WindowTransparentForInput;
    }
    setFlags(flags);
    // Placed in AboveLayer by Window::belongsToLayer() (see internalwindow.cpp).
    setProperty("__ki3_overlay", true);
}

void Ki3SolidOverlay::setColor(const QColor &color)
{
    m_color = color;
    update();
}

void Ki3SolidOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(QRectF(0, 0, width(), height()), m_color);
}

void Ki3SolidOverlay::mousePressEvent(QMouseEvent *event)
{
    // Only the primary button starts a resize; a right/middle click here
    // would otherwise be silently hijacked into one (review finding L1).
    if (m_acceptsInput && event->button() == Qt::LeftButton) {
        Q_EMIT pressed(event->globalPosition());
    }
}

Ki3FloatTitleBar::Ki3FloatTitleBar()
{
    // Borderless, unmanaged; input arrives so the bar can be dragged. Tagged
    // __ki3_overlay so belongsToLayer() puts it in AboveLayer (see Ki3Header).
    setFlags(Qt::BypassWindowManagerHint | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    setProperty("__ki3_overlay", true);
}

qreal Ki3FloatTitleBar::barHeight()
{
    return 24.0;
}

void Ki3FloatTitleBar::setTitle(const QString &title)
{
    m_title = title;
    update();
}

void Ki3FloatTitleBar::setPalette(const Ki3HeaderPalette &palette)
{
    m_palette = palette;
    update();
}

void Ki3FloatTitleBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QFont font = QGuiApplication::font();
    p.setFont(font);
    const QRectF area(0, 0, width(), height());
    p.fillRect(area, m_palette.activeBg);
    p.setPen(m_palette.border);
    p.drawRect(area.adjusted(0, 0, -1, -1));

    const QFontMetrics fm(font);
    const qreal pad = 6.0;
    p.setPen(m_palette.activeText);
    const QRectF textRect = area.adjusted(pad, 0, -pad, 0);
    const QString elided = fm.elidedText(m_title, Qt::ElideRight, int(textRect.width()));
    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
}

void Ki3FloatTitleBar::mousePressEvent(QMouseEvent *event)
{
    // Only the primary button starts a drag (review finding L1); a
    // right/middle click on the bar should do nothing, not move the window.
    if (event->button() == Qt::LeftButton) {
        Q_EMIT dragRequested(event->globalPosition());
    }
}

Ki3Header::Ki3Header()
{
    // Borderless, unmanaged; input arrives so tabs can be clicked. Tagged
    // __ki3_overlay so belongsToLayer() puts it in AboveLayer — above ordinary
    // windows but below menus/notifications/OSD (OverlayLayer would cover those).
    setFlags(Qt::BypassWindowManagerHint | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    setProperty("__ki3_overlay", true);
}

qreal Ki3Header::rowHeight()
{
    return 24.0;
}

qreal Ki3Header::heightForTabs(int count, bool stacked)
{
    if (count <= 0) {
        return 0.0;
    }
    return stacked ? rowHeight() * count : rowHeight();
}

void Ki3Header::setTabs(const QStringList &titles, int active, bool stacked, bool focused)
{
    m_titles = titles;
    m_active = active;
    m_stacked = stacked;
    m_focused = focused;
    // update() (QPaintDeviceWindow) marks the surface dirty and schedules a
    // paintEvent; requestUpdate() alone only posts an UpdateRequest without
    // dirtying, so the header would never repaint after its first expose.
    update();
}

void Ki3Header::setPalette(const Ki3HeaderPalette &palette)
{
    m_palette = palette;
    update();
}

void Ki3Header::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QFont font = QGuiApplication::font();
    p.setFont(font);
    const QRectF area(0, 0, width(), height());
    p.fillRect(area, m_palette.inactiveBg);

    const int n = m_titles.size();
    if (n == 0) {
        return;
    }

    const QFontMetrics fm(font);
    const qreal pad = 6.0;

    for (int i = 0; i < n; ++i) {
        QRectF cell;
        if (m_stacked) {
            cell = QRectF(0, i * rowHeight(), width(), rowHeight());
        } else {
            const qreal w = qreal(width()) / n;
            cell = QRectF(i * w, 0, w, height());
        }

        const bool isActive = (i == m_active) && m_focused;
        p.fillRect(cell, isActive ? m_palette.activeBg : m_palette.inactiveBg);
        p.setPen(m_palette.border);
        p.drawRect(cell.adjusted(0, 0, -1, -1));

        p.setPen(isActive ? m_palette.activeText : m_palette.inactiveText);
        const QRectF textRect = cell.adjusted(pad, 0, -pad, 0);
        const QString elided = fm.elidedText(m_titles[i], Qt::ElideRight, int(textRect.width()));
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
    }
}

void Ki3Header::mousePressEvent(QMouseEvent *event)
{
    // Only the primary button activates a tab (review finding L1); a
    // right/middle click shouldn't change the active tab.
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const int index = indexAt(event->position());
    if (index >= 0) {
        Q_EMIT tabActivated(index);
    }
}

int Ki3Header::indexAt(const QPointF &pos) const
{
    const int n = m_titles.size();
    if (n == 0) {
        return -1;
    }
    int index;
    if (m_stacked) {
        index = int(pos.y() / rowHeight());
    } else {
        index = int(pos.x() / (qreal(width()) / n));
    }
    return (index >= 0 && index < n) ? index : -1;
}

} // namespace KWin

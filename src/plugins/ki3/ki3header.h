/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QColor>
#include <QRasterWindow>
#include <QStringList>

namespace KWin
{

/**
 * A plain solid-colour internal overlay window (used for the split-direction
 * indicator). A QRasterWindow rather than a QQuickWindow so it renders through
 * KWin's internal QPA backing store with no OpenGL/Qt-Quick RHI — the Quick
 * path crashes on the headless virtual backend (no GL context). Tagged
 * `__ki3_overlay` so Window::belongsToLayer() puts it in AboveLayer (above
 * ordinary windows, below menus/notifications).
 */
class Ki3SolidOverlay : public QRasterWindow
{
    Q_OBJECT

public:
    explicit Ki3SolidOverlay();
    void setColor(const QColor &color);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QColor m_color;
};

/**
 * The title bar ki3 draws for a tabbed/stacked container. A borderless internal
 * QRasterWindow (KWin composites its QPainter backing store via the internal QPA
 * backing store, and gives untagged internal windows OverlayLayer, so it sits
 * above ordinary windows). ki3 owns one per tab/stack group, positions it in the
 * header strip reserved by Tile::setHeaderReserve(), and feeds it the tab titles.
 *
 * Tabbed: one row of side-by-side tabs. Stacked: one full-width title row per
 * window, stacked vertically. Clicking a tab emits tabActivated(index).
 */
class Ki3Header : public QRasterWindow
{
    Q_OBJECT

public:
    explicit Ki3Header();

    /** Height of a single title row, in logical pixels. */
    static qreal rowHeight();

    /**
     * The header height for @p count tabs in the given mode: one row for tabbed,
     * one row per tab for stacked.
     */
    static qreal heightForTabs(int count, bool stacked);

    /** Update the drawn tabs and repaint. @p active is highlighted. */
    void setTabs(const QStringList &titles, int active, bool stacked);

Q_SIGNALS:
    /** A tab was clicked; @p index is its position in the titles list. */
    void tabActivated(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    /** Which tab index sits under @p pos, or -1. */
    int indexAt(const QPointF &pos) const;

    QStringList m_titles;
    int m_active = 0;
    bool m_stacked = false;
};

} // namespace KWin

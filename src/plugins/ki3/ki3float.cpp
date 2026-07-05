/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3_logging.h"
#include "ki3header.h"
#include "ki3tiler.h"

#include "window.h"
#include "workspace.h"

namespace KWin
{

void Ki3Tiler::toggleFloating()
{
    Window *window = workspace()->activeWindow();
    if (!window) {
        return;
    }
    if (m_floatingWindows.contains(window)) {
        // Re-tile it.
        m_floatingWindows.remove(window);
        destroyFloatChrome(window);
        window->setKeepAbove(false);
        qCDebug(KWIN_KI3) << "unfloat" << window->caption();
        insertWindow(window);
    } else if (m_leafForWindow.contains(window)) {
        // Detach from the tree; it keeps its current geometry and floats.
        m_floatingWindows.insert(window);
        qCDebug(KWIN_KI3) << "float" << window->caption();
        forgetWindow(window); // restores noBorder(false); createFloatChrome() below re-hides it
        window->requestTile(nullptr);
        window->setNoBorder(true); // ki3 draws its own title bar instead of the native SSD
        // i3/sway: floating windows always stay above tiled ones, even across
        // focus changes -- KWin's default focus-follows-raise would otherwise
        // sink this behind a tiled window the user clicks on next.
        window->setKeepAbove(true);
        createFloatChrome(window);
    }
}

void Ki3Tiler::createFloatChrome(Window *window)
{
    if (!window || m_floatChrome.contains(window)) {
        return;
    }

    // Grow the window's footprint upward by the title bar height (like a real
    // decoration adding chrome outside the client area) rather than shrinking
    // its content; the vacated strip above is where the bar gets drawn.
    const qreal barHeight = Ki3FloatTitleBar::barHeight();
    const RectF geom = window->frameGeometry();
    window->moveResize(RectF(geom.left(), geom.top() + barHeight, geom.width(), geom.height()));

    FloatChrome chrome;
    chrome.titleBar = std::make_shared<Ki3FloatTitleBar>();
    chrome.titleBar->setPalette(m_headerPalette);
    connect(chrome.titleBar.get(), &Ki3FloatTitleBar::dragRequested, this,
            [window](const QPointF &globalPos) {
        window->performMousePressCommand(Options::MouseMove, globalPos);
    });

    for (auto &strip : chrome.resizeStrips) {
        strip = std::make_shared<Ki3SolidOverlay>(/*acceptsInput=*/true);
        connect(strip.get(), &Ki3SolidOverlay::pressed, this,
                [window](const QPointF &globalPos) {
            window->performMousePressCommand(Options::MouseResize, globalPos);
        });
    }

    // Keep the chrome glued to the window as it moves/resizes/retitles. Plain
    // lambdas (not Ki3Tiler methods), so Qt::UniqueConnection can't dedupe them
    // — fine, since the m_floatChrome.contains() guard above already ensures
    // this only runs once per window.
    chrome.geometryConn = connect(window, &Window::frameGeometryChanged, this,
                                  [this, window] {
        repositionFloatChrome(window);
    });
    chrome.captionConn = connect(window, &Window::captionChanged, this,
                                 [this, window] {
        repositionFloatChrome(window);
    });

    m_floatChrome.insert(window, chrome);
    repositionFloatChrome(window); // also sets the border's initial focus state
    qCDebug(KWIN_KI3) << "float chrome created for" << window->caption();
}

void Ki3Tiler::destroyFloatChrome(Window *window)
{
    auto it = m_floatChrome.find(window);
    if (it == m_floatChrome.end()) {
        return;
    }
    disconnect(it->geometryConn);
    disconnect(it->captionConn);
    m_floatChrome.erase(it); // shared_ptrs drop refcount and free the overlay windows
}

void Ki3Tiler::repositionFloatChrome(Window *window)
{
    auto it = m_floatChrome.find(window);
    if (it == m_floatChrome.end()) {
        return;
    }
    FloatChrome &chrome = it.value();

    if (!window->isShown() || !window->isOnCurrentDesktop()) {
        chrome.titleBar->hide();
        for (auto &strip : chrome.resizeStrips) {
            strip->hide();
        }
        return;
    }

    static constexpr qreal thickness = 4.0;
    const qreal barHeight = Ki3FloatTitleBar::barHeight();
    const RectF geom = window->frameGeometry();

    // Extend by `thickness` on each side to match the left/right resize
    // strips' outer edges below -- otherwise the bar's top corners fall short
    // of them, leaving a small notch where the strips stick out past it.
    const QRectF titleRect(geom.left() - thickness, geom.top() - barHeight,
                           geom.width() + 2 * thickness, barHeight);
    chrome.titleBar->setGeometry(titleRect.toRect());
    chrome.titleBar->setTitle(window->caption());
    chrome.titleBar->show();

    // Order matches the FloatChrome doc comment: left, right, bottom. Sit just
    // outside the window's own edges so the strips never occlude its content.
    const QRectF strips[3] = {
        QRectF(geom.left() - thickness, geom.top(), thickness, geom.height()),
        QRectF(geom.right(), geom.top(), thickness, geom.height()),
        QRectF(geom.left(), geom.bottom(), geom.width(), thickness),
    };
    for (int i = 0; i < 3; ++i) {
        chrome.resizeStrips[i]->setGeometry(strips[i].toRect());
    }
    // Visibility (not just geometry) depends on focus -- see
    // updateFloatChromeBorder() -- so it decides show()/hide() for the strips
    // instead of this unconditionally showing them.
    updateFloatChromeBorder(window);
}

void Ki3Tiler::updateFloatChromeBorder(Window *window)
{
    auto it = m_floatChrome.find(window);
    if (it == m_floatChrome.end()) {
        return;
    }
    FloatChrome &chrome = it.value();

    // repositionFloatChrome() already hides everything while the window isn't
    // visible on the current desktop; don't fight that here.
    if (!window->isShown() || !window->isOnCurrentDesktop()) {
        return;
    }

    // i3/sway: an unfocused floating window shows no border at all -- unlike
    // a tiled leaf's border (updateTileBorders()), which stays visible (just
    // muted) even unfocused so the tile grid's boundaries don't disappear.
    const bool focused = window == workspace()->activeWindow();
    for (auto &strip : chrome.resizeStrips) {
        strip->setColor(m_focusBorderColor);
        strip->setVisible(focused);
    }
}

void Ki3Tiler::updateAllFloatChromeBorders()
{
    for (auto it = m_floatChrome.constBegin(); it != m_floatChrome.constEnd(); ++it) {
        updateFloatChromeBorder(it.key());
    }
}

} // namespace KWin

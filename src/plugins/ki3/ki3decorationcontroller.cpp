/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3decorationcontroller.h"
#include "ki3_logging.h"

#include "tiles/customtile.h"
#include "window.h"
#include "workspace.h"

#include <KColorScheme>
#include <KConfigGroup>
#include <KSharedConfig>

namespace KWin
{

// Linear-RGB blend of @p a and @p b, @p t of the way from a to b.
static QColor mixColors(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

// The four kIndicatorThickness-wide edge strips outlining @p geom, in the order
// the resize/focus border arrays expect: top, bottom, left, right.
static std::array<RectF, 4> borderStrips(const RectF &geom)
{
    return {
        RectF(geom.left(), geom.top(), geom.width(), kIndicatorThickness),
        RectF(geom.left(), geom.bottom() - kIndicatorThickness, geom.width(), kIndicatorThickness),
        RectF(geom.left(), geom.top(), kIndicatorThickness, geom.height()),
        RectF(geom.right() - kIndicatorThickness, geom.top(), kIndicatorThickness, geom.height()),
    };
}

// Same as borderStrips(), but the four strips sit just *outside* @p geom (in
// the tile's own padding/gap, like a floating window's chrome border — see
// DecorationController::repositionFloatChrome()) instead of overlapping its
// content. Top/bottom extend the full outward width (including both side
// thicknesses) so all four strips still meet cleanly at the corners, same as
// borderStrips()'s inward overlap does.
static std::array<RectF, 4> outwardBorderStrips(const RectF &geom)
{
    return {
        RectF(geom.left() - kIndicatorThickness, geom.top() - kIndicatorThickness,
              geom.width() + 2 * kIndicatorThickness, kIndicatorThickness),
        RectF(geom.left() - kIndicatorThickness, geom.bottom(),
              geom.width() + 2 * kIndicatorThickness, kIndicatorThickness),
        RectF(geom.left() - kIndicatorThickness, geom.top(), kIndicatorThickness, geom.height()),
        RectF(geom.right(), geom.top(), kIndicatorThickness, geom.height()),
    };
}

DecorationController::DecorationController(TileTreeController *tileTree, QObject *parent)
    : QObject(parent)
    , m_tileTree(tileTree)
    , m_splitIndicatorWindow(std::make_unique<Ki3SolidOverlay>())
{
    for (auto &strip : m_resizeBorder) {
        strip = std::make_unique<Ki3SolidOverlay>();
    }

    // Replaces what used to be a direct updateSplitIndicator()/
    // updateTileBorders() call from inside every tile-tree-mutating method --
    // see TileTreeController::layoutChanged()'s doc comment.
    connect(m_tileTree, &TileTreeController::layoutChanged, this, &DecorationController::updateSplitIndicator);

    // Colour every overlay from the active colour scheme now, and re-read it
    // whenever the user switches scheme (kdeglobals changes) so the accents
    // stay in step with ki3-pager's Kirigami colours instead of freezing at load.
    m_colorSchemeWatcher = KConfigWatcher::create(KSharedConfig::openConfig());
    connect(m_colorSchemeWatcher.get(), &KConfigWatcher::configChanged, this,
            [this](const KConfigGroup &group, const QByteArrayList &) {
        if (group.name().startsWith(QLatin1String("Colors:"))
            || group.name() == QLatin1String("General")) {
            applyIndicatorColors();
        }
    });
    applyIndicatorColors();
}

void DecorationController::watchWindow(Window *window)
{
    // Any window's real geometry settling can flip another leaf's occlusion
    // state (leafWindowOccluded() walks the whole stacking order, not just
    // managed windows), and window->frameGeometry() often still reflects the
    // *old* rect for one more round-trip after a fresh split/insert already
    // reassigned its tile -- see the 2026-07-05 PLAN entry "tile borders stuck
    // invisible after a fresh split". Recheck borders whenever any window's
    // geometry actually commits, not only on ki3's own explicit resync calls;
    // deferred (see scheduleBorderRecheck()) since this signal can fire from
    // inside a Wayland surface-commit transaction, where synchronously
    // touching ki3's own overlay windows crashes. Qt::UniqueConnection guards
    // the harmless case of this firing twice for the same window.
    connect(window, &Window::frameGeometryChanged, this, &DecorationController::scheduleBorderRecheck,
            Qt::UniqueConnection);
}

void DecorationController::applyIndicatorColors()
{
    // Split indicator: the scheme's selection background — the same value
    // Kirigami exposes as Theme.highlightColor, which ki3-pager uses for the
    // current/active desktop cell.
    const QColor highlight = KColorScheme(QPalette::Active, KColorScheme::Selection)
                                 .background(KColorScheme::NormalBackground)
                                 .color();
    m_splitIndicatorWindow->setColor(highlight);

    // Resize border: the scheme's neutral ("warning") foreground — Kirigami's
    // Theme.neutralTextColor, which ki3-pager switches to while resize mode is
    // active, and distinct from the split indicator's selection accent.
    const QColor resizeColor = KColorScheme(QPalette::Active, KColorScheme::View)
                                   .foreground(KColorScheme::NeutralText)
                                   .color();
    for (auto &strip : m_resizeBorder) {
        strip->setColor(resizeColor);
    }

    // Window chrome (tile borders, tab/stack headers, floating title bar):
    // border/text/unfocused-background come from KColorScheme's Header set —
    // the same role KWin's own window decorations use for their titlebar —
    // but the *focused* background reuses the split indicator's own
    // highlight, mixed halfway toward the view background so it reads as a
    // muted accent rather than the raw selection colour. The Header set's own
    // Active background turned out to have poor/inverted contrast against its
    // Inactive background in some colour schemes -- barely distinguishable
    // from "unfocused", the opposite of the point -- so it isn't used for the
    // focused background itself (see the 2026-07-05 PLAN entry).
    const KColorScheme activeHeader(QPalette::Active, KColorScheme::Header);
    const KColorScheme inactiveHeader(QPalette::Inactive, KColorScheme::Header);
    const QColor windowBg = KColorScheme(QPalette::Active, KColorScheme::View)
                                .background(KColorScheme::NormalBackground)
                                .color();
    m_focusBorderColor = mixColors(highlight, windowBg, 0.5);
    m_unfocusedBorderColor = inactiveHeader.background().color();
    m_headerPalette.activeBg = m_focusBorderColor;
    m_headerPalette.inactiveBg = m_unfocusedBorderColor;
    m_headerPalette.border = activeHeader.background(KColorScheme::AlternateBackground).color();
    m_headerPalette.activeText = activeHeader.foreground().color();
    m_headerPalette.inactiveText = inactiveHeader.foreground().color();

    // Reused for a focused floating window's chrome border and every tab/
    // stack header + floating title bar, so all of ki3's own chrome tracks a
    // colour-scheme switch together.
    updateTileBorders();
    updateAllFloatChromeBorders();
    updateHeaderPalette();
}

void DecorationController::updateHeaderPalette()
{
    m_tileTree->setHeaderPalette(m_headerPalette);
    for (auto it = m_floatChrome.begin(); it != m_floatChrome.end(); ++it) {
        it->titleBar->setPalette(m_headerPalette);
    }
}

void DecorationController::scheduleBorderRecheck()
{
    if (m_borderRecheckPending) {
        return;
    }
    m_borderRecheckPending = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_borderRecheckPending = false;
        updateSplitIndicator();
    }, Qt::QueuedConnection);
}

void DecorationController::updateSplitIndicator()
{
    // Refreshed unconditionally (not just when the split indicator itself has
    // something to show below) so it also runs -- and hides the borders -- when
    // the current leaf disappears, e.g. the last window on a desktop closing
    // while resize mode is active.
    //
    // Order matters for stacking: tile borders must be (re)shown *before* the
    // resize border and the split indicator so those two internal windows are
    // created after them and therefore stack above them in AboveLayer (see
    // updateTileBorders()).
    updateTileBorders();
    updateResizeIndicator();

    CustomTile *leaf = m_tileTree->currentLeaf();

    if (m_splitIndicatorLeaf != leaf) {
        if (m_splitIndicatorLeaf) {
            disconnect(m_splitIndicatorLeaf, &Tile::windowGeometryChanged, this, &DecorationController::updateSplitIndicator);
        }
        m_splitIndicatorLeaf = leaf;
        if (leaf) {
            connect(leaf, &Tile::windowGeometryChanged, this, &DecorationController::updateSplitIndicator);
        }
    }

    // Only meaningful on an actual leaf holding a window that is presently
    // visible; a bare root, a tile mid-restructure (childCount() > 0), or a
    // leaf left over on a desktop we've since switched away from has nothing
    // to highlight.
    Window *window = (leaf && leaf->childCount() == 0 && !leaf->windows().isEmpty())
        ? leaf->windows().constFirst()
        : nullptr;
    if (!window || !window->isShown() || !window->isOnCurrentDesktop() || m_tileTree->leafWindowOccluded(window)) {
        m_splitIndicatorWindow->hide();
        return;
    }

    const RectF geom = leaf->windowGeometry();
    const RectF strip = (m_tileTree->splitDirection() == Tile::LayoutDirection::Horizontal)
        ? RectF(geom.right() - kIndicatorThickness, geom.top(), kIndicatorThickness, geom.height())
        : RectF(geom.left(), geom.bottom() - kIndicatorThickness, geom.width(), kIndicatorThickness);
    m_splitIndicatorWindow->setGeometry(strip.toRect());
    m_splitIndicatorWindow->show();
}

void DecorationController::setResizeModeActive(bool active)
{
    if (m_resizeModeActive == active) {
        return;
    }
    m_resizeModeActive = active;
    updateResizeIndicator();
}

void DecorationController::updateResizeIndicator()
{
    CustomTile *leaf = m_resizeModeActive ? m_tileTree->currentLeaf() : nullptr;

    if (m_resizeIndicatorLeaf != leaf) {
        if (m_resizeIndicatorLeaf) {
            disconnect(m_resizeIndicatorLeaf, &Tile::windowGeometryChanged, this, &DecorationController::updateResizeIndicator);
        }
        m_resizeIndicatorLeaf = leaf;
        if (leaf) {
            connect(leaf, &Tile::windowGeometryChanged, this, &DecorationController::updateResizeIndicator);
        }
    }

    // Same visibility rule as the split indicator: only a leaf with a
    // presently-shown window on the current desktop has anything to outline.
    Window *window = (leaf && leaf->childCount() == 0 && !leaf->windows().isEmpty())
        ? leaf->windows().constFirst()
        : nullptr;
    if (!window || !window->isShown() || !window->isOnCurrentDesktop() || m_tileTree->leafWindowOccluded(window)) {
        for (auto &strip : m_resizeBorder) {
            strip->hide();
        }
        return;
    }

    const auto strips = borderStrips(leaf->windowGeometry());
    for (int i = 0; i < 4; ++i) {
        m_resizeBorder[i]->setGeometry(strips[i].toRect());
        m_resizeBorder[i]->show();
    }
}

void DecorationController::updateTileBorders()
{
    // Every leaf currently showing a real tiled window gets a border -- unlike
    // a single "current leaf only" indicator, so the boundary line between two
    // tiles stays put and only *changes colour* as focus moves between them,
    // instead of popping in/out on alternating sides of the gap.
    const QSet<CustomTile *> liveLeaves = m_tileTree->liveLeaves();

    // i3-style "smart borders": a lone tile filling its whole output/desktop
    // has no neighbour to delimit, so only draw borders where more than one
    // leaf shares a root. Counted across every live leaf regardless of its
    // own shown/occlusion state below -- a root's leaves are always all on
    // the same (single) desktop, so this is just "how many tiles does this
    // output/desktop have", independent of transient per-window occlusion.
    QHash<Tile *, int> leafCountByRoot;
    for (CustomTile *leaf : liveLeaves) {
        ++leafCountByRoot[leaf->rootTile()];
    }

    for (CustomTile *leaf : liveLeaves) {
        // Same visibility rule as the split/resize indicators: only a leaf
        // with a presently-shown, unoccluded window on the current desktop
        // has anything to outline.
        Window *window = (leaf->childCount() == 0 && !leaf->windows().isEmpty())
            ? leaf->windows().constFirst()
            : nullptr;
        const bool visible = window && window->isShown() && window->isOnCurrentDesktop()
            && !m_tileTree->leafWindowOccluded(window) && leafCountByRoot.value(leaf->rootTile()) > 1;
        if (!visible) {
            if (auto it = m_tileBorders.find(leaf); it != m_tileBorders.end()) {
                disconnect(it->geometryConn);
                m_tileBorders.erase(it);
            }
            continue;
        }

        if (!m_tileBorders.contains(leaf)) {
            TileBorder border;
            for (auto &strip : border.strips) {
                strip = std::make_shared<Ki3SolidOverlay>();
            }
            // A resize/redistribute that doesn't itself route through
            // updateSplitIndicator() (e.g. a neighbour tile being pushed by
            // another one resizing) still needs to keep the border glued to
            // its leaf.
            border.geometryConn = connect(leaf, &Tile::windowGeometryChanged, this,
                                          [this, leaf] {
                repositionTileBorder(leaf);
            });
            m_tileBorders.insert(leaf, std::move(border));
            // Drop the entry the instant the tile is destroyed (e.g. an
            // output unplug tears down its tile tree) so the raw-pointer key
            // never dangles; see TileTreeController::onGroupTileDestroyed()
            // for the same pattern.
            connect(leaf, &QObject::destroyed, this, &DecorationController::onTileBorderDestroyed,
                    Qt::UniqueConnection);
        }
        repositionTileBorder(leaf);
    }

    // Drop borders for leaves no longer live at all (window removed, floated
    // away, or the tile's own tree torn down); everything else not currently
    // visible was already handled inside the loop above.
    for (auto it = m_tileBorders.begin(); it != m_tileBorders.end();) {
        if (!liveLeaves.contains(it.key())) {
            disconnect(it->geometryConn);
            it = m_tileBorders.erase(it);
        } else {
            ++it;
        }
    }
}

void DecorationController::repositionTileBorder(CustomTile *leaf)
{
    auto it = m_tileBorders.find(leaf);
    if (it == m_tileBorders.end()) {
        return;
    }
    TileBorder &border = it.value();

    const auto strips = outwardBorderStrips(leaf->windowGeometry());
    // A tab/stack group already has its own header showing the title right
    // above windowGeometry() (see TileTreeController::refreshGroup()); the top
    // strip there would just be redundant wasted space, so skip it for
    // grouped leaves only.
    const bool skipTop = m_tileTree->isGroup(leaf);
    const QColor &color = (leaf == m_tileTree->currentLeaf()) ? m_focusBorderColor : m_unfocusedBorderColor;
    for (int i = 0; i < 4; ++i) {
        if (i == 0 && skipTop) {
            border.strips[i]->hide();
            continue;
        }
        border.strips[i]->setColor(color);
        border.strips[i]->setGeometry(strips[i].toRect());
        border.strips[i]->show();
    }
}

void DecorationController::onTileBorderDestroyed(QObject *tile)
{
    // The tile is mid-destruction; use it as a bare key only (no dereference).
    auto it = m_tileBorders.find(static_cast<CustomTile *>(tile));
    if (it == m_tileBorders.end()) {
        return;
    }
    // This slot runs reentrantly: synchronously nested inside the *dying
    // tile's own* QObject destructor (Tile::~Tile() -> ~QObject() ->
    // destroyed() -> here). Actually tearing down the border's
    // Ki3SolidOverlay windows right now is unsafe -- destroying an internal
    // QWindow cascades through Workspace::removeInternalWindow() ->
    // windowRemoved(), which every live Tile relays into unmanage(); if
    // that reaches back into a tile whose own destructor is still
    // unwinding on this very call stack, it's a real crash (ki3-PLAN.md has
    // the trace -- this is exactly the follow-on crash the Workspace-scoped
    // disconnect() in Tile::~Tile() alone didn't cover). Drop the map entry
    // now (m_tileBorders itself must never keep the stale key), but let the
    // TileBorder's shared_ptrs -- and hence the overlay windows -- actually
    // die on the next event loop turn instead, safely outside this cascade.
    TileBorder border = std::move(it.value());
    m_tileBorders.erase(it);
    QMetaObject::invokeMethod(this, [border = std::move(border)]() {
        qCDebug(KWIN_KI3) << "tile border: deferred teardown of stale entry running";
    }, Qt::QueuedConnection);
    qCDebug(KWIN_KI3) << "tile border: owning tile destroyed; dropped stale entry";
}

} // namespace KWin

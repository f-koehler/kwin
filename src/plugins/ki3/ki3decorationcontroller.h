/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ki3header.h"
#include "ki3tiletreecontroller.h"

#include <KConfigWatcher>

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPointer>

#include <array>
#include <memory>

namespace KWin
{

class CustomTile;
class Window;

/**
 * Owns every pixel ki3 draws that isn't a real client window: the split-
 * direction hint, the resize-mode border, tile borders, tab/stack header
 * colours, and a floating window's title bar + resize strips. Second
 * controller extracted from the `Ki3Tiler` god object (M1 refactor,
 * Phase 3) -- see `~/.claude/plans/toasty-fluttering-kitten.md` and the
 * matching ki3-PLAN.md entry.
 *
 * Named for KWin's own established term (`KDecoration`, ki3's
 * `DecorationPolicy` enum) rather than the plugin-internal "chrome" wording
 * used in older comments -- note the scope here is broader than that term's
 * narrow KWin sense (server-side titlebar/border policy): this also owns the
 * tile-border/split/resize indicators and (eventually) tab/stack headers.
 *
 * Depends one-directionally on TileTreeController (a non-owning pointer,
 * given at construction): every query into it is read-only
 * (currentLeaf()/liveLeaves()/leafWindowOccluded()/isGroup()/splitDirection()),
 * plus one push in the safe direction (setHeaderPalette(), mirroring how
 * Ki3Tiler used to push colours into it before this controller existed).
 * TileTreeController has no knowledge of this class in return.
 *
 * Tab/stack header rendering itself still lives on TileTreeController (it
 * was merged in during Phase 2, since header state is tab-model state) --
 * moving it here is an explicitly optional future Phase 6, not part of this
 * extraction.
 */
class DecorationController : public QObject
{
    Q_OBJECT

public:
    explicit DecorationController(TileTreeController *tileTree, QObject *parent = nullptr);

    /**
     * Start recomputing @p window's borders whenever its geometry commits.
     * Any window's real geometry settling can flip another leaf's occlusion
     * state (leafWindowOccluded() walks the whole stacking order, not just
     * managed windows), so every window ki3 sees needs this, not only ones
     * it tiles. Deferred internally (see scheduleBorderRecheck()) since this
     * can fire from inside a Wayland surface-commit transaction, where
     * synchronously touching ki3's own overlay windows crashes.
     */
    void watchWindow(Window *window);

    /**
     * Refresh the on-screen hint showing where the next tiled window will
     * land, the resize-mode border, and every tile border -- see the three
     * private update*() methods this delegates to, and their doc comments
     * for the stacking-order rationale. Connected to TileTreeController's
     * layoutChanged() signal internally (constructor), so callers normally
     * never need this directly; still public for the one explicit call
     * Ki3Tiler's constructor makes after adopting pre-existing windows.
     */
    void updateSplitIndicator();

    /** updateFloatChromeBorder() for every floating window with chrome. */
    void updateAllFloatChromeBorders();

    /**
     * Push a new resize-mode-active state in (see
     * ShortcutController::setResizeMode()) and immediately redraw the
     * resize border to match.
     */
    void setResizeModeActive(bool active);

    /**
     * Build (or, if already present, just leave alone) the floating chrome
     * for @p window: reflows its geometry to carve out title-bar space, then
     * creates the title bar + resize strips, wires their input signals to
     * Window::performMousePressCommand(), and hooks its geometry/caption
     * changes to keep the chrome in sync.
     */
    void createFloatChrome(Window *window);

    /** Tear down @p window's floating chrome (un-float or window destroy). */
    void destroyFloatChrome(Window *window);

    /** Reposition and repaint @p window's floating chrome to match its current geometry/title. */
    void repositionFloatChrome(Window *window);

    /**
     * Plugin teardown: destroy every floating window's chrome. Self-
     * contained -- iterates this controller's own m_floatChrome, not
     * TileTreeController's floating-window set. Call before
     * TileTreeController::detachAllManagedWindows() (see
     * Ki3Tiler::teardownManagedState()).
     */
    void teardownFloatChrome();

private:
    /**
     * ki3's own chrome for a floating window, replacing its native SSD: a
     * title bar above it (drag to move) and thin resize strips along its
     * left/right/bottom edges (drag to resize). See createFloatChrome().
     */
    struct FloatChrome
    {
        std::shared_ptr<Ki3FloatTitleBar> titleBar;
        // Left, right, bottom (top is covered by the title bar itself).
        std::array<std::shared_ptr<Ki3SolidOverlay>, 3> resizeStrips;
        // The window's geometry/caption connections driving repositionFloatChrome();
        // stored so destroyFloatChrome() can disconnect the exact lambda connections
        // (Qt::UniqueConnection can't dedupe lambdas, so we guard by construction
        // instead and just need precise teardown here).
        QMetaObject::Connection geometryConn;
        QMetaObject::Connection captionConn;
    };

    /**
     * ki3's own border chrome for one tiled leaf: outward top/bottom/left/
     * right strips around its windowGeometry(), kept below the split/resize
     * indicators (see updateTileBorders()).
     */
    struct TileBorder
    {
        std::array<std::shared_ptr<Ki3SolidOverlay>, 4> strips;
        // Tracks the leaf's own geometry; see repositionTileBorder().
        QMetaObject::Connection geometryConn;
    };

    /**
     * (Re)apply the colour-scheme-derived colours to the overlay windows: the
     * split indicator uses the scheme's selection background (Kirigami's
     * Theme.highlightColor, matching ki3-pager's active-desktop accent), the
     * resize border its neutral/negative accent, and the tile borders/tab-
     * stack headers/floating title bar all share KColorScheme's Header set
     * (see m_headerPalette). Called at construction and whenever kdeglobals
     * changes (see m_colorSchemeWatcher) so all of these track a live colour-
     * scheme switch the way ki3-pager's Kirigami colours do.
     */
    void applyIndicatorColors();

    /**
     * Push m_headerPalette to every existing floating title bar, and to
     * m_tileTree so it can do the same for every tab/stack header (see
     * TileTreeController::setHeaderPalette()).
     */
    void updateHeaderPalette();

    /**
     * Queue a single (coalesced) updateSplitIndicator() (which includes
     * updateTileBorders()) for the next event loop turn. See watchWindow()'s
     * doc comment for why this must be deferred.
     */
    void scheduleBorderRecheck();

    /**
     * Refresh the on-screen border drawn around the resize-mode target: four
     * thin strips around the current leaf's window while resize mode is
     * active (see setResizeModeActive()), hidden otherwise. Called from
     * updateSplitIndicator() so it stays in sync with every event that can
     * change the current leaf or its geometry; also re-tracks the leaf's
     * windowGeometryChanged the same way updateSplitIndicator() does, so the
     * border follows the leaf through a resize.
     */
    void updateResizeIndicator();

    /**
     * Refresh the border drawn around *every* currently visible tiled leaf:
     * four thin strips sitting just outside its windowGeometry() (in the
     * tile's own padding/gap, like a floating window's chrome border — see
     * repositionTileBorder()) rather than overlapping its content. Unlike a
     * single "current leaf" indicator, every visible leaf keeps a border all
     * the time; only its colour changes (accent for the current leaf, muted
     * otherwise) — this way the boundary line between two tiles never pops
     * in/out on alternating sides of the gap as focus moves between them, it
     * only recolours in place. Creates/destroys per-leaf entries in
     * m_tileBorders as leaves gain/lose a visible window; called from
     * updateSplitIndicator() so it stays in sync with every event that can
     * change which leaves are visible or who is focused. Deliberately run
     * *before* the split/resize indicators each cycle so those draw on top —
     * ki3's overlays share KWin's AboveLayer, where the most-recently-shown
     * internal window stacks highest.
     */
    void updateTileBorders();

    /**
     * Recompute @p leaf's border geometry/colour/visibility (see
     * updateTileBorders()): outward strips around its current
     * windowGeometry(), coloured for focus, top strip skipped for a
     * tab/stack leaf (its header already occupies that space — see
     * TileTreeController::refreshGroup()). No-op if @p leaf has no entry in
     * m_tileBorders. Connected to the leaf's own windowGeometryChanged so a
     * resize/redistribute that doesn't itself call updateSplitIndicator()
     * (e.g. a neighbour tile being pushed by another one resizing) still
     * keeps the border glued to its leaf.
     */
    void repositionTileBorder(CustomTile *leaf);

    /**
     * Slot: a bordered tile was destroyed out from under us (e.g. an output
     * unplug tore down its tile tree). m_tileBorders is keyed by raw
     * CustomTile*, so without this its entry would dangle. @p tile is only
     * used as a hash key here (never dereferenced), so it is safe
     * mid-destruction.
     */
    void onTileBorderDestroyed(QObject *tile);

    /**
     * Recolour @p window's resize-strip border to match focus, like a tiled
     * leaf's border (updateTileBorders()): m_focusBorderColor when @p window
     * is the active window, hidden otherwise (a floating window has no muted
     * unfocused border — i3/sway show none at all until it is focused).
     * Called on activation changes and colour-scheme updates; see
     * applyIndicatorColors().
     */
    void updateFloatChromeBorder(Window *window);

    // Non-owning; TileTreeController outlives this (constructed first in
    // Ki3Tiler's constructor, destroyed last). Every call into it is a
    // read-only query -- see the class doc comment above.
    TileTreeController *m_tileTree;

    // Whether Meta+R resize mode is active; pushed in by
    // ShortcutController::setResizeMode() via setResizeModeActive().
    bool m_resizeModeActive = false;

    // Coalesces scheduleBorderRecheck() calls after any window's geometry commits.
    bool m_borderRecheckPending = false;

    // Compositor-drawn hint on the trailing edge of the current leaf, showing
    // where the next tiled window will land. A plain internal QRasterWindow
    // (Ki3SolidOverlay): renders through KWin's internal QPA backing store with
    // no OpenGL/Qt-Quick RHI (a QQuickWindow crashes on the headless virtual
    // backend, which has no GL context — see ki3-PLAN.md 2026-07-02). Tagged
    // __ki3_overlay so belongsToLayer() places it in AboveLayer.
    std::unique_ptr<Ki3SolidOverlay> m_splitIndicatorWindow;

    // The leaf m_splitIndicatorWindow is currently tracking (connected to for
    // geometry updates), so updateSplitIndicator() can (re)subscribe only
    // when it actually changes.
    QPointer<CustomTile> m_splitIndicatorLeaf;

    // Border drawn around the resize-mode target (see updateResizeIndicator()):
    // top, bottom, left, right strips, same Ki3SolidOverlay mechanism as
    // m_splitIndicatorWindow above.
    std::array<std::unique_ptr<Ki3SolidOverlay>, 4> m_resizeBorder;

    // The leaf m_resizeBorder is currently tracking; see m_splitIndicatorLeaf.
    QPointer<CustomTile> m_resizeIndicatorLeaf;

    // Border chrome for every currently visible tiled leaf (see
    // updateTileBorders()). Keyed by raw CustomTile*; onTileBorderDestroyed()
    // drops the entry the instant its tile is destroyed so the key never
    // dangles.
    QHash<CustomTile *, TileBorder> m_tileBorders;

    // Cached copy of the colour applyIndicatorColors() computes for the
    // *focused* leaf's border, reused for a focused floating window's chrome
    // border (see updateFloatChromeBorder()) so both borders always match.
    QColor m_focusBorderColor;

    // Cached copy of the colour applyIndicatorColors() computes for every
    // *unfocused* leaf's border (see m_tileBorders) — a muted sibling of
    // m_focusBorderColor so the boundary between tiles stays visible even
    // when neither side has focus, preventing the accent border from simply
    // popping in/out as focus moves (see updateTileBorders()).
    QColor m_unfocusedBorderColor;

    // Cached KColorScheme::Header-derived palette applyIndicatorColors()
    // computes, pushed to every floating title bar (see updateHeaderPalette())
    // and to m_tileTree for its tab/stack headers, and to freshly-created
    // ones in createFloatChrome(). m_focusBorderColor/m_unfocusedBorderColor
    // are its activeBg/inactiveBg, so tile borders and headers always match.
    Ki3HeaderPalette m_headerPalette;

    // Watches kdeglobals so applyIndicatorColors() re-reads the scheme when the
    // user switches colour scheme, keeping the overlays' accents live (matching
    // ki3-pager's Kirigami colours) instead of frozen at plugin-load time.
    KConfigWatcher::Ptr m_colorSchemeWatcher;

    // ki3's own title bar + resize strips for each floating window (replacing
    // its native SSD). Entries created in createFloatChrome(), torn down in
    // destroyFloatChrome(); keyed by the same windows as TileTreeController's
    // floating-window set (kept separately here since chrome lifecycle is a
    // decoration concern, not a tiling-membership one).
    QHash<Window *, FloatChrome> m_floatChrome;
};

} // namespace KWin

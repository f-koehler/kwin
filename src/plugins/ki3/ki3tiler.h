/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ki3rules.h"
#include "plugin.h"
#include "tiles/tile.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <memory>

namespace KWin
{

class CustomTile;
class Ki3Header;
class Ki3SolidOverlay;
class LogicalOutput;
class RootTile;
class VirtualDesktop;
class Window;

/**
 * Exported on D-Bus as KWin's own /Ki3 object (service "org.kde.KWin", which
 * KWin's DBusInterface already owns) so external UI — e.g. the ki3-pager
 * plasmoid, since Plasma's stock Pager assumes one global desktop and can't
 * represent ki3's per-output model — can show/drive per-output desktops
 * without linking against KWin internals.
 */
class Ki3Tiler : public Plugin
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.ki3")

public:
    explicit Ki3Tiler();
    ~Ki3Tiler() override;

public Q_SLOTS:
    /** Output names (matches Qt/QScreen::name(), e.g. "DP-1") known to KWin. */
    Q_SCRIPTABLE QStringList outputNames() const;

    /** 1-based current desktop number shown on @p outputName, or 0 if unknown. */
    Q_SCRIPTABLE int currentDesktopNumber(const QString &outputName) const;

    /** Number of currently live desktops. */
    Q_SCRIPTABLE int desktopCount() const;

    /** Workspace numbers (parsed from desktop names) of all currently live desktops, in order. */
    Q_SCRIPTABLE QList<int> liveDesktopNumbers() const;

    /** D-Bus wrapper for switchToWorkspace(), for the ki3-pager plasmoid. */
    Q_SCRIPTABLE void dbusSwitchToWorkspace(int number);

    /** D-Bus wrapper for moveActiveToWorkspace(), for the ki3-pager plasmoid. */
    Q_SCRIPTABLE void dbusMoveActiveToWorkspace(int number);

Q_SIGNALS:
    /** Emitted whenever any output's current desktop changes. */
    Q_SCRIPTABLE void desktopsChanged();

private:
    void handleWindowAdded(Window *window);
    void handleWindowRemoved(Window *window);
    void handleWindowActivated(Window *window);

    /** Register global shortcuts. */
    void registerShortcuts();

    /**
     * i3/sway container layouts beyond plain splits. A tab/stack "container" is
     * represented as a *single leaf tile owning several windows* (KWin gives
     * every window in a tile the same rect — tile.cpp:141-143 — i.e. they
     * overlap, which is exactly a tab/stack group); ki3 layers on top which one
     * is visible and (later) draws the header. Only tabbed/stacked leaves have a
     * TabState; a plain split leaf has none.
     */
    enum class ContainerMode {
        Tabbed, // one header row of tabs; cycle with focus left/right
        Stacked, // stacked title rows; cycle with focus up/down
    };
    struct TabState
    {
        QList<QPointer<Window>> windows; // tab order
        int active = 0; // index into windows of the visible one
        ContainerMode mode = ContainerMode::Tabbed;
        // The container's split direction before it was collapsed, restored on
        // untab (i3/sway prev_split_layout).
        Tile::LayoutDirection prevSplit = Tile::LayoutDirection::Horizontal;
        std::shared_ptr<Ki3Header> header; // the title bar (created lazily)
    };

    /** The leaf tile of the currently active or last focused window. */
    CustomTile *currentLeaf() const;

    /**
     * Collapse the focused container into a single tab/stack group leaf (i3/sway
     * "layout tabbed"/"layout stacked", Meta+T/Meta+S). Re-invoking with the same
     * mode splits it back out (untabContainer); invoking the other mode flips it
     * in place. T0 spike: model + visibility only, no header UI yet.
     */
    void setContainerMode(ContainerMode mode);

    /** Fan a tab/stack group leaf back out into an even split of its windows. */
    void untabContainer(CustomTile *tile);

    /** Advance the visible tab of @p tile by @p delta (wraps), and focus it. */
    void cycleTab(CustomTile *tile, int delta);

    /** Raise the active tab of @p tile above the others; prune dead windows. */
    void updateTabVisibility(CustomTile *tile);

    /**
     * Recompute a tab/stack group's header: reserve the right header height on
     * its tile, reposition + repaint the header window over the reserved strip,
     * and hide it when the group is off the current desktop. Creates the header
     * on first use and (re)connects the tile's geometry signal.
     */
    void refreshGroup(CustomTile *tile);

    /** Refresh every tab/stack group (e.g. after a desktop switch). */
    void refreshAllGroups();

    /** Tear down a group's header and clear its tile's header reserve. */
    void destroyGroupHeader(CustomTile *tile);

    /** Slot: a group tile's geometry changed — reposition its header. */
    void onGroupGeometryChanged();

    /** Activate the tab at @p index within @p tile (from a header click). */
    void activateTab(CustomTile *tile, int index);

    /**
     * Refresh the on-screen hint showing where the next tiled window will
     * land: a thin strip on the trailing edge of currentLeaf() (right edge
     * for a Horizontal m_splitDirection, bottom edge for Vertical), or hidden
     * when there is no current leaf. Re-tracks currentLeaf()'s
     * windowGeometryChanged so the strip follows resizes/redistributes
     * without every call site having to remember to refresh it.
     */
    void updateSplitIndicator();

    /** Move keyboard focus to the neighbouring leaf in @p edge direction. */
    void moveFocus(Qt::Edge edge);

    /** Move focus from @p leaf to the adjacent output in @p edge direction. */
    void moveFocusAcrossOutput(CustomTile *leaf, Qt::Edge edge);

    /** Swap the active window with its neighbour in @p edge direction. */
    void moveWindow(Qt::Edge edge);

    /** Grow/shrink the active leaf along @p orientation by @p deltaPixels. */
    void resizeActive(Qt::Orientation orientation, qreal deltaPixels);

    /**
     * Set the split direction applied to the next inserted window (i3/sway
     * "split h"/"split v", `Meta+G`/`Meta+V` here since `Meta+H` is taken by
     * vim-style focus-left).
     */
    void setSplitDirection(Tile::LayoutDirection direction);

    /**
     * Toggle the *current container's* layout direction between horizontal and
     * vertical in place (i3/sway "layout toggle split", `Meta+E`). Distinct
     * from setSplitDirection(): this rearranges the focused window's existing
     * siblings rather than steering where the next new window lands.
     */
    void toggleContainerLayout();

    /** Toggle the active window between tiled and floating. */
    void toggleFloating();

    /** Switch to workspace (virtual desktop) @p number (1-based), i3/sway-style. */
    void switchToWorkspace(int number);

    /** Move the active window to workspace @p number (1-based), following it to its output. */
    void moveActiveToWorkspace(int number);

    /** Find the live desktop with user-facing number @p n (by name), or nullptr. */
    VirtualDesktop *desktopByNumber(int n) const;

    /**
     * Return the live desktop for number @p n, creating it (with name == n) if
     * it does not yet exist. Inserts in sorted order so the pager stays ordered.
     */
    VirtualDesktop *getOrCreateDesktop(int n);

    /**
     * Create a new desktop whose name is the smallest positive integer not
     * already used as a desktop name. Used when a new output needs a workspace.
     */
    VirtualDesktop *createFreshDesktop();

    /**
     * Remove every desktop that is not currently shown on any output and has no
     * windows. Called deferred (via schedulePrune) so KWin finishes window
     * destruction before we check occupancy.
     */
    void pruneEmptyDesktops();

    /** Queue a single (coalesced) pruneEmptyDesktops after the current event. */
    void schedulePrune();

    /**
     * Give each output a distinct starting desktop (output i -> desktop i+1) so
     * no desktop number is shown on two screens at once — the i3/sway invariant.
     */
    void assignInitialDesktops();

    /** The output currently showing @p desktop, or nullptr if it is hidden. */
    LogicalOutput *outputShowingDesktop(VirtualDesktop *desktop) const;

    /**
     * The output a (possibly hidden) @p desktop "lives" on: the output of the
     * first client window on it, excluding @p exclude. nullptr if it has none.
     */
    LogicalOutput *outputForDesktop(VirtualDesktop *desktop, Window *exclude = nullptr) const;

    /** Move keyboard focus to @p output by activating its top window there. */
    void focusOutput(LogicalOutput *output);

    /** First desktop not currently shown on any output, or nullptr if all are. */
    VirtualDesktop *firstFreeDesktop() const;

    /**
     * Queue a single (coalesced) reconcile after an output was plugged/unplugged.
     * Deferred so it runs once KWin has finished re-homing windows and tile trees.
     */
    void scheduleReconcile();

    /** Reconcile ki3 state with the current set of outputs (see scheduleReconcile). */
    void reconcileOutputs();

    /** Drop m_managedRoots entries whose TileManager/RootTile no longer exists. */
    void purgeStaleRoots();

    /** Re-assert the one-desktop-per-screen invariant, moving duplicates to free desktops. */
    void enforceUniqueDesktops();

    /** Re-tile windows whose tile vanished (unplug) or that moved to another tree. */
    void retileHomelessWindows();

    /**
     * If focus was left on a window that is now hidden (e.g. the active output
     * was unplugged and its window re-homed onto another screen's background
     * desktop), move focus to the active output's visible desktop.
     */
    void ensureSaneFocus();

    /** The output of the active window, else the globally active output. */
    LogicalOutput *focusedOutput() const;

    /** Launch a terminal emulator (i3-style Meta+Return). */
    void spawnTerminal();

    /** Close the active window (i3/sway-style Meta+Shift+Q). */
    void closeActiveWindow();

    /** Whether ki3 should tile this window at all. */
    bool shouldManage(Window *window) const;

    /** Whether a non-tileable rule (built-in or user-configured) matches @p window. */
    bool isNonTileable(const Window *window) const;

    /** Insert a manageable window into its output/desktop tile tree. */
    void insertWindow(Window *window);

    /** Detach a window and collapse the tile it leaves behind. */
    void forgetWindow(Window *window);

    /** Root tile for the window's output and (current) desktop, or nullptr. */
    RootTile *rootForWindow(Window *window) const;

    /**
     * Clear KWin's default tile layout for @p root so ki3 owns the tree.
     * Done once per root (lazily) the first time we manage it.
     */
    void ensureManaged(RootTile *root);

    /** First leaf (childless) tile under @p root, or @p root if it has none. */
    static CustomTile *firstLeaf(RootTile *root);

    /** Lay out @p parent's direct children as equal slices along its direction. */
    static void redistributeEvenly(CustomTile *parent);

    /**
     * Re-point m_leafForWindow at the leaf each managed window actually sits in.
     * KWin's CustomTile::remove() can migrate a window into a promoted parent
     * tile (customtile.cpp:326-340), leaving our mapping pointing at a deleted
     * tile; this repairs it for the whole tree under @p root.
     */
    void resyncLeafMapping(RootTile *root);

    /**
     * Set @p tile's geometry to @p geom, bypassing CustomTile's neighbour-push
     * engine, and remap its subtree proportionally so nested layouts keep their
     * internal ratios. Used by redistributeEvenly to assign exact slices.
     */
    static void setGeometryRecursive(CustomTile *tile, const RectF &geom);

    // Default split direction for new windows (i3 default: side-by-side).
    Tile::LayoutDirection m_splitDirection = Tile::LayoutDirection::Horizontal;

    // Authoritative mapping of the windows we manage to their leaf tile.
    QHash<Window *, QPointer<CustomTile>> m_leafForWindow;

    // The leaf that should receive the next window (last focused tiled leaf).
    QPointer<CustomTile> m_lastFocusedLeaf;

    // Roots whose default layout we've already cleared and taken over.
    QSet<RootTile *> m_managedRoots;

    // Windows the user has detached from tiling (floating).
    QSet<Window *> m_floatingWindows;

    // Leaf tiles that are tab/stack groups (own several windows, one visible).
    // Keyed by the group leaf; entries are dropped when the group empties.
    QHash<CustomTile *, TabState> m_tabbed;

    // Guards refreshGroup() against the re-entry triggered when setHeaderReserve
    // emits windowGeometryChanged (which we listen to). The re-entrant call is a
    // no-op; the outer call finishes positioning against the settled geometry.
    bool m_refreshingGroup = false;

    // Rules (built-in + user config) for windows ki3 must never tile.
    QList<WindowRule> m_nonTileableRules;

    // Coalesces output plug/unplug events into a single deferred reconcile.
    bool m_reconcilePending = false;

    // Coalesces pruneEmptyDesktops calls after window removal / workspace switch.
    bool m_prunePending = false;

    // Compositor-drawn hint on the trailing edge of the current leaf, showing
    // where the next tiled window will land. A plain internal QRasterWindow
    // (Ki3SolidOverlay): renders through KWin's internal QPA backing store with
    // no OpenGL/Qt-Quick RHI (a QQuickWindow crashes on the headless virtual
    // backend, which has no GL context — see ki3-PLAN.md 2026-07-02). Tagged
    // __ki3_overlay so belongsToLayer() places it in AboveLayer.
    std::unique_ptr<Ki3SolidOverlay> m_splitIndicatorWindow;

    // The leaf m_splitIndicator is currently tracking (connected to for
    // geometry updates), so updateSplitIndicator() can (re)subscribe only
    // when it actually changes.
    QPointer<CustomTile> m_splitIndicatorLeaf;
};

} // namespace KWin

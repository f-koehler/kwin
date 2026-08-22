/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/backendoutput.h"
#include "ki3header.h"
#include "ki3rules.h"
#include "plugin.h"
#include "tiles/tile.h"

#include <KConfigWatcher>

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <array>
#include <memory>

namespace KWin
{

class BackendOutput;
class CustomTile;
class Ki3SolidOverlay;
class LogicalOutput;
class RootTile;
class VirtualDesktop;
class Window;

// Shared thickness (device-independent px) of every leaf-edge overlay: the
// split-direction hint, the resize-mode border, and the tile border (see
// ki3tiler.cpp's outwardBorderStrips()). Shared with ki3groups.cpp's
// refreshGroup() so a tab/stack group's header lines up with the same
// leaf's left/right border strips instead of falling short of them.
static constexpr qreal kIndicatorThickness = 3.0;

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

    /**
     * Workspace numbers that "belong" to @p outputName, in order: the desktops
     * whose windows live on that output (their home output) plus the desktop
     * currently shown there. This is the i3/sway per-output workspace set — the
     * pager on each screen shows only these, never every output's desktops.
     */
    Q_SCRIPTABLE QList<int> desktopNumbersForOutput(const QString &outputName) const;

    /**
     * Name of the output ki3 currently considers focused (see focusedOutput()),
     * or empty if none. Lets the ki3-pager plasmoid mute its active-desktop
     * highlight on every screen except this one.
     */
    Q_SCRIPTABLE QString focusedOutputName() const;

    /** D-Bus wrapper for switchToWorkspace(), for the ki3-pager plasmoid. */
    Q_SCRIPTABLE void dbusSwitchToWorkspace(int number);

    /** D-Bus wrapper for moveActiveToWorkspace(), for the ki3-pager plasmoid. */
    Q_SCRIPTABLE void dbusMoveActiveToWorkspace(int number);

    /** Whether Meta+R resize mode (see toggleResizeMode()) is currently active. */
    Q_SCRIPTABLE bool resizeModeActive() const;

    /**
     * Test/introspection: one entry per window ki3 tiles, each
     * "<output>|<desktop>|<x>,<y>,<w>,<h>" using the window's *actual* applied
     * frame geometry (not the tile's intended rect). Lets a test assert that a
     * window really laid out where its tile says -- e.g. that a cross-output move
     * onto a hidden workspace didn't leave windows overlapping. The debug log
     * only prints intended tile geometry, so this is the only ground truth for
     * "did the geometry actually get applied".
     */
    Q_SCRIPTABLE QStringList tiledWindowGeometries() const;

    /**
     * Test-only: hot-plug a virtual output and return its name (empty on
     * failure). No-op unless the env var KI3_TEST_HOOKS is set, so it can never
     * spawn a phantom screen in a real session. Used by the regression suite to
     * exercise output plug/unplug (createVirtualOutput → Workspace::outputAdded,
     * the same path a real monitor takes). See tests/harness.py `hotplug`.
     */
    Q_SCRIPTABLE QString dbusAddTestOutput();

    /** Test-only counterpart: unplug the most recently added test output. */
    Q_SCRIPTABLE QString dbusRemoveTestOutput();

Q_SIGNALS:
    /** Emitted whenever any output's current desktop changes. */
    Q_SCRIPTABLE void desktopsChanged();

    /** Emitted whenever resize mode is toggled on or off. */
    Q_SCRIPTABLE void resizeModeChanged();

    /** Emitted whenever focusedOutputName() would return a different value. */
    Q_SCRIPTABLE void focusedOutputChanged();

private:
    void handleWindowAdded(Window *window);
    void handleWindowRemoved(Window *window);
    void handleWindowActivated(Window *window);

    /**
     * Snapshot whatever ki3 is about to overwrite in the *real* kwinrc/
     * kglobalshortcutsrc -- but only the first time, guarded by
     * `ki3rc [SessionBackup] Pending`. A leftover `Pending=true` from an
     * unclean previous exit means the real config *already* holds ki3's own
     * values, not the user's; re-snapshotting then would capture ki3's state
     * as if it were the original, corrupting the restore this is meant to
     * provide. Called first thing in the constructor, before anything that
     * mutates kwinrc/global shortcuts. See restoreSessionStateOnCleanExit()
     * and ki3session.cpp for the full design (ki3-PLAN.md has the write-up).
     */
    void backupSessionStateIfNeeded();

    /**
     * Snapshot the current owner of @p key into the SessionBackup group,
     * right before registerShortcuts()/setResizeMode() steal it -- but only
     * if this session took a fresh backup at startup (m_captureShortcutOwners;
     * false means one was already pending from an unclean exit, so the real
     * config already holds ki3's own values and there's nothing genuine left
     * to capture). Also a no-op if nobody currently owns the key, or if the
     * current owner is already ki3 itself (ki3OwnsAction()) -- e.g. a later
     * setResizeMode() activation re-stealing a key it already grabbed earlier
     * this same session.
     */
    void captureShortcutOwnerIfNeeded(const QKeySequence &key);

    /** True if @p actionUniqueName is one of ki3's own "ki3_*" action ids. */
    bool ki3OwnsAction(const QString &actionUniqueName) const;

    /**
     * Undo backupSessionStateIfNeeded()'s overwrites: restore the real
     * kwinrc groups ki3 touched and hand every captured shortcut back to its
     * original owner via KGlobalAccel's setForeignShortcut(). Called from
     * ~Ki3Tiler(), which runs on a normal logout well before Workspace/
     * VirtualDesktopManager teardown (Application::destroyPlugins() runs
     * before destroyWorkspace() in ApplicationWayland::~ApplicationWayland(),
     * see main_wayland.cpp) -- so this can still use the live APIs. A no-op
     * if nothing is pending (nothing was ever backed up, or a clean exit
     * already restored and cleared it). No effect on a hard crash/SIGKILL,
     * which skips destructors entirely -- session/ki3-restore-if-pending.sh
     * (run at the next normal Plasma login via XDG autostart) is the
     * fallback for that case.
     */
    void restoreSessionStateOnCleanExit();

    /** Register global shortcuts. */
    void registerShortcuts();

    /**
     * Register the bare (no-modifier) h/j/k/l, arrow, and Escape/Return
     * shortcuts used *only* while resize mode is active (see setResizeMode()).
     * Created once with no bound keys — KGlobalAccel would otherwise grab e.g.
     * plain "h" globally forever, breaking normal typing everywhere. Their
     * QKeySequences are (re)applied/cleared by setResizeMode() itself, which
     * is the closest approximation available to i3/sway's real keyboard grab
     * without KWin plugins having access to one.
     */
    void registerResizeModeShortcuts();

    /**
     * Disable the Overview effect's default top-left screen-edge trigger.
     * It fires where ki3's own split/resize indicators and stacked/tabbed
     * title bars live, is easy to hit by accident while tiling, and its
     * default Meta+W shortcut is taken over for the tabbed layout in
     * registerShortcuts() anyway.
     */
    void disableOverviewHotCorner();

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

    /** The leaf tile of the currently active or last focused window. */
    CustomTile *currentLeaf() const;

    /**
     * Collapse the focused container into a single tab/stack group leaf (i3/sway
     * "layout tabbed"/"layout stacked", Meta+W/Meta+S). Re-invoking with the same
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

    /**
     * Slot: a group's container tile was destroyed out from under us (e.g. an
     * output was unplugged, taking its whole TileManager/tile tree with it).
     * m_tabbed is keyed by raw CustomTile* (unlike the QPointer-guarded
     * m_leafForWindow), so without this its entry would dangle and the next
     * refreshAllGroups() would dereference freed memory. @p tile is only used
     * as a hash key here (never dereferenced), so it is safe mid-destruction.
     */
    void onGroupTileDestroyed(QObject *tile);

    /**
     * Slot: a root tile ki3 had taken over (see ensureManaged) was destroyed.
     * This fires not only on output unplug but on every desktop *prune* — each
     * TileManager deletes its per-desktop RootTile on VirtualDesktop removal
     * (tilemanager.cpp) — which purgeStaleRoots() (output-change-only) would
     * miss. m_managedRoots is keyed by raw RootTile*, so a freed address can be
     * recycled by a fresh RootTile and wrongly look "already managed", making
     * ensureManaged() skip clearing KWin's default layout. Drop the entry the
     * instant its root dies. @p tile is a bare key here (never dereferenced).
     */
    void onManagedRootDestroyed(QObject *tile);

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

    /**
     * Refresh the on-screen border drawn around the resize-mode target: four
     * thin strips around currentLeaf()'s window while m_resizeMode is active,
     * hidden otherwise. Called from updateSplitIndicator() so it stays in sync
     * with every event that can change the current leaf or its geometry
     * without duplicating that call site's bookkeeping; also re-tracks
     * currentLeaf()'s windowGeometryChanged the same way updateSplitIndicator()
     * does, so the border follows the leaf through a resize.
     */
    void updateResizeIndicator();

    /**
     * Refresh the border drawn around *every* currently visible tiled leaf:
     * four thin strips sitting just outside its windowGeometry() (in the
     * tile's own padding/gap, like a floating window's chrome border — see
     * repositionTileBorder()) rather than overlapping its content. Unlike a
     * single "current leaf" indicator, every visible leaf keeps a border all
     * the time; only its colour changes (accent for currentLeaf(), muted
     * otherwise) — this way the boundary line between two tiles never pops
     * in/out on alternating sides of the gap as focus moves between them, it
     * only recolours in place. Creates/destroys per-leaf entries in
     * m_tileBorders as leaves gain/lose a visible window; called from
     * updateSplitIndicator() so it stays in sync with every event that can
     * change which leaves are visible or who is focused. Deliberately run
     * *before* the split/resize indicators each cycle (see
     * updateSplitIndicator()) so those draw on top — ki3's overlays share
     * KWin's AboveLayer, where the most-recently-shown internal window stacks
     * highest (its InternalWindow is (re)created on show; see qpa/window.cpp
     * map()/unmap()).
     */
    void updateTileBorders();

    /**
     * Recompute @p leaf's border geometry/colour/visibility (see
     * updateTileBorders()): outward strips around its current
     * windowGeometry(), coloured for focus, top strip skipped for a
     * tab/stack leaf (its header already occupies that space — see
     * refreshGroup()). No-op if @p leaf has no entry in m_tileBorders.
     * Connected to the leaf's own windowGeometryChanged so a resize/
     * redistribute that doesn't itself call updateSplitIndicator() (e.g. a
     * neighbour tile being pushed by another one resizing) still keeps the
     * border glued to its leaf.
     */
    void repositionTileBorder(CustomTile *leaf);

    /**
     * Slot: a bordered tile was destroyed out from under us (e.g. an output
     * unplug tore down its tile tree). m_tileBorders is keyed by raw
     * CustomTile* (like m_tabbed), so without this its entry would dangle.
     * @p tile is only used as a hash key here (never dereferenced), so it is
     * safe mid-destruction.
     */
    void onTileBorderDestroyed(QObject *tile);

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

    /** Push m_headerPalette to every existing tab/stack header and floating title bar. */
    void updateHeaderPalette();

    /** Move keyboard focus to the neighbouring leaf in @p edge direction. */
    void moveFocus(Qt::Edge edge);

    /**
     * Move focus from @p leaf to a neighbouring tile (same output, then
     * adjacent output) in @p edge direction. Returns whether a target was
     * found and focused.
     */
    bool leaveLeaf(CustomTile *leaf, Qt::Edge edge);

    /** Move focus from @p leaf to the adjacent output in @p edge direction. Returns success. */
    bool moveFocusAcrossOutput(CustomTile *leaf, Qt::Edge edge);

    /** Swap the active window with its neighbour in @p edge direction. */
    void moveWindow(Qt::Edge edge);

    /**
     * Eject @p self (a member of the group at @p leaf) toward @p edge when
     * moveWindow() found no existing neighbour tile to pop it into -- splits
     * @p leaf itself to make room, mirroring i3's "move" semantics: leaving a
     * tab/stack group always succeeds, even off a group's edge that has
     * nothing next to it (typically because that very slot is what the
     * window vacated when it joined the group).
     */
    void ejectGroupMemberViaSplit(CustomTile *leaf, Window *self, Qt::Edge edge);

    /** Grow/shrink the active leaf along @p orientation by @p deltaPixels. */
    void resizeActive(Qt::Orientation orientation, qreal deltaPixels);

    /**
     * Toggle resize mode (i3/sway "mode resize", `Meta+R`): while active, bare
     * h/j/k/l/arrow keys (no modifier) resize the focused leaf, Escape/Return
     * leave the mode, and every other ki3 shortcut is inert — matching i3/sway,
     * where entering resize mode grabs the keyboard so nothing else fires until
     * the mode is explicitly left. Press `Meta+R` again (always live, see
     * registerShortcuts()) to leave the mode from outside it too.
     */
    void toggleResizeMode();

    /**
     * Enter/leave resize mode; shared by toggleResizeMode(), the bare Escape/
     * Return exit shortcuts (see registerResizeModeShortcuts()), and the
     * auto-exit in handleWindowRemoved() (closing the last window leaves
     * nothing to resize). No-op if @p active already matches the current
     * state, so the auto-exit doesn't spam the log/D-Bus signal on every
     * window close. Binds/unbinds the bare-key resize shortcuts to approximate
     * i3/sway's keyboard grab — see registerResizeModeShortcuts().
     */
    void setResizeMode(bool active);

    /** Move keyboard focus in @p edge direction (bound to Meta+h/j/k/l/arrows). */
    void handleDirectional(Qt::Edge edge);

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
     * Queue a single (coalesced) updateSplitIndicator() (which includes
     * updateTileBorders()) for the next event loop turn. Any window's real
     * geometry committing can flip another leaf's occlusion state (see
     * handleWindowAdded()), but frameGeometryChanged can fire from deep inside
     * KWin's Wayland surface-commit transaction machinery -- recreating or
     * repositioning ki3's own internal overlay windows *synchronously* from
     * there is unsafe (crashes; see the 2026-07-05 PLAN entry "tile borders
     * stuck invisible after a fresh split"), so the actual recheck is always
     * deferred to a fresh event via Qt::QueuedConnection.
     */
    void scheduleBorderRecheck();

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

    /**
     * True if some ordinary window stacked above @p window overlaps its frame
     * (e.g. a modal dialog raised over its tiled parent) -- see the definition
     * for why a not-yet-tiled window ki3 will manage momentarily is excluded.
     */
    bool leafWindowOccluded(Window *window) const;

    /**
     * Insert a manageable window into its output/desktop tile tree.
     *
     * @p outputHint, when non-null, overrides window->output() for choosing the
     * target tree. This is required right after a cross-output move
     * (Window::sendToOutput()): on Wayland the output only updates in
     * WaylandWindow::updateGeometry() once the client acks the configure, so
     * window->output() still reports the *old* output synchronously here. Using
     * the stale output would tile the window into the (old-output, target-desktop)
     * tree — hidden, since that output shows a different desktop — until an
     * unrelated re-home (e.g. an output unplug) rescues it. See moveActiveToWorkspace().
     */
    void insertWindow(Window *window, LogicalOutput *outputHint = nullptr);

    /**
     * Place @p window at @p target, following i3 join rules: joins a tab/stack
     * group as a new tab, becomes an evenly-redistributed sibling if @p target's
     * parent already runs the current split direction, or otherwise splits
     * @p target in the current split direction. Shared by insertWindow() (for
     * brand-new windows, always after target — @p insertBefore false) and
     * moveWindow() (for relocating an existing one, where @p insertBefore lets
     * the sibling case land on the correct side of target — e.g. within an
     * existing V[top,bottom] pair, moving the bottom window up must insert it
     * *before* target or it lands back in its own vacated slot and nothing
     * visibly changes).
     */
    void placeWindowAt(Window *window, CustomTile *target, bool insertBefore = false);

    /**
     * Add @p window to @p leaf and make the tile association stick even when
     * @p leaf's desktop is not the one currently shown on its output.
     * Tile::manage() only calls window->requestTile() when the tile isActive()
     * (its desktop is current) and otherwise clears an evacuated window's tile
     * (tile.cpp:444) -- so tiling onto a hidden destination (e.g. a window moved
     * across outputs onto a background workspace) would apply no geometry and
     * the window would overlap at its raw position, before *and* after that
     * desktop is next shown. Re-requesting explicitly is idempotent when
     * manage() already did it. Use this everywhere ki3 tiles a window.
     */
    void attachWindow(Window *window, CustomTile *leaf);

    /** Detach a window and collapse the tile it leaves behind. */
    void forgetWindow(Window *window);

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
     * Recolour @p window's resize-strip border to match focus, like a tiled
     * leaf's border (updateTileBorders()): m_focusBorderColor when @p window
     * is the active window, hidden otherwise (a floating window has no muted
     * unfocused border — i3/sway show none at all until it is focused).
     * Called on activation changes and colour-scheme updates; see
     * applyIndicatorColors().
     */
    void updateFloatChromeBorder(Window *window);

    /** updateFloatChromeBorder() for every floating window with chrome. */
    void updateAllFloatChromeBorders();

    /**
     * Root tile for the window's output and (current) desktop, or nullptr.
     * @p outputHint overrides window->output() when it is known to be stale
     * (see insertWindow()).
     */
    RootTile *rootForWindow(Window *window, LogicalOutput *outputHint = nullptr) const;

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

    // Whether Meta+R resize mode is active; see toggleResizeMode().
    bool m_resizeMode = false;

    // Authoritative mapping of the windows we manage to their leaf tile.
    QHash<Window *, QPointer<CustomTile>> m_leafForWindow;

    // The leaf that should receive the next window (last focused tiled leaf).
    QPointer<CustomTile> m_lastFocusedLeaf;

    // Roots whose default layout we've already cleared and taken over.
    QSet<RootTile *> m_managedRoots;

    // Windows the user has detached from tiling (floating).
    QSet<Window *> m_floatingWindows;

    // ki3's own title bar + resize strips for each floating window (replacing
    // its native SSD). Entries created in createFloatChrome(), torn down in
    // destroyFloatChrome(); keyed by the same windows as m_floatingWindows.
    QHash<Window *, FloatChrome> m_floatChrome;

    // Leaf tiles that are tab/stack groups (own several windows, one visible).
    // Keyed by the group leaf; entries are dropped when the group empties.
    QHash<CustomTile *, TabState> m_tabbed;

    // Guards refreshGroup() against the re-entry triggered when setHeaderReserve
    // emits windowGeometryChanged (which we listen to). Keyed per tile so a
    // cascade that refreshes a *different* group mid-call still runs; only the
    // re-entry for the same tile is suppressed (that outer call finishes
    // positioning against the settled geometry).
    QSet<CustomTile *> m_refreshingGroups;

    // Rules (built-in + user config) for windows ki3 must never tile.
    QList<WindowRule> m_nonTileableRules;

    // Coalesces output plug/unplug events into a single deferred reconcile.
    bool m_reconcilePending = false;

    // Test-only: virtual outputs hot-plugged via dbusAddTestOutput(), newest
    // last. QPointer so an output that vanishes another way self-clears.
    QList<QPointer<BackendOutput>> m_testOutputs;
    int m_testOutputSeq = 0;

    // Coalesces pruneEmptyDesktops calls after window removal / workspace switch.
    bool m_prunePending = false;

    // Set once by backupSessionStateIfNeeded(): true if this session took a
    // fresh SessionBackup snapshot, false if one was already Pending from an
    // unclean exit (meaning the real config already holds ki3's own values,
    // so there's nothing genuine left to capture this session). Gates every
    // captureShortcutOwnerIfNeeded() call for the rest of the session. See
    // ki3session.cpp.
    bool m_captureShortcutOwners = false;

    // Coalesces scheduleBorderRecheck() calls after any window's geometry commits.
    bool m_borderRecheckPending = false;

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

    // Border drawn around the resize-mode target (see updateResizeIndicator()):
    // top, bottom, left, right strips, same Ki3SolidOverlay mechanism as
    // m_splitIndicatorWindow above.
    std::array<std::unique_ptr<Ki3SolidOverlay>, 4> m_resizeBorder;

    // The leaf m_resizeBorder is currently tracking; see m_splitIndicatorLeaf.
    QPointer<CustomTile> m_resizeIndicatorLeaf;

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

    // Border chrome for every currently visible tiled leaf (see
    // updateTileBorders()). Keyed by raw CustomTile* like m_tabbed;
    // onTileBorderDestroyed() drops the entry the instant its tile is
    // destroyed so the key never dangles.
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
    // computes, pushed to every tab/stack header and floating title bar (see
    // updateHeaderPalette()) and to freshly-created ones in refreshGroup()/
    // createFloatChrome(). m_focusBorderColor/m_unfocusedBorderColor are its
    // activeBg/inactiveBg, so tile borders and headers always match.
    Ki3HeaderPalette m_headerPalette;

    // Watches kdeglobals so applyIndicatorColors() re-reads the scheme when the
    // user switches colour scheme, keeping the overlays' accents live (matching
    // ki3-pager's Kirigami colours) instead of frozen at plugin-load time.
    KConfigWatcher::Ptr m_colorSchemeWatcher;

    // One resize-mode-only shortcut (see registerResizeModeShortcuts()): the
    // action to trigger, and the keys setResizeMode() binds it to on entry /
    // clears on exit. Kept as data (not just QActions) since KGlobalAccel's
    // setShortcut() needs the key list re-supplied on every rebind.
    struct ResizeModeShortcut
    {
        QAction *action;
        QList<QKeySequence> keys;
    };
    QList<ResizeModeShortcut> m_resizeModeActions;
};

} // namespace KWin

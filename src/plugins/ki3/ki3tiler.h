/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/backendoutput.h"
#include "ki3decorationcontroller.h"
#include "ki3sessionguard.h"
#include "ki3tiletreecontroller.h"
#include "plugin.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <memory>

namespace KWin
{

class BackendOutput;
class LogicalOutput;
class VirtualDesktop;
class Window;

/**
 * Exported on D-Bus as KWin's own /Ki3 object (service "org.kde.KWin", which
 * KWin's DBusInterface already owns) so external UI — e.g. the ki3-pager
 * plasmoid, since Plasma's stock Pager assumes one global desktop and can't
 * represent ki3's per-output model — can show/drive per-output desktops
 * without linking against KWin internals.
 *
 * As of the M1 refactor's Phase 1-3, this class delegates the tile tree,
 * tab/stack group model, and floating-window set to TileTreeController
 * (m_tileTree), tile-border/split/resize-indicator/floating-chrome rendering
 * to DecorationController (m_decoration), and reversible-session config
 * backup/restore to Ki3SessionGuard (m_sessionGuard) -- see
 * `~/.claude/plans/toasty-fluttering-kitten.md` and the matching
 * ki3-PLAN.md entries. What remains here: plugin lifecycle, global
 * shortcuts, virtual-desktop/output reconciliation, and the D-Bus surface
 * itself.
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
     * Test/introspection: one entry per window ki3 currently floats, each
     * "<output>|<x>,<y>,<w>,<h>" using the window's *actual* applied frame
     * geometry (the client area ki3 moved down to make room for its title
     * bar -- see DecorationController::createFloatChrome()), not the chrome's
     * geometry. Lets a test
     * assert the client stayed within the output's usable area after
     * floating near an edge (review finding M3).
     */
    Q_SCRIPTABLE QStringList floatingWindowGeometries() const;

    /** Test/introspection: the active window's current decoration policy -- true if borderless. */
    Q_SCRIPTABLE bool activeWindowNoBorder() const;

    /** Test/introspection: the active window's current keep-above state. */
    Q_SCRIPTABLE bool activeWindowKeepAbove() const;

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

    /**
     * Test-only: directly set the active window's decoration policy/keep-above/
     * all-desktops state, bypassing ki3 entirely -- simulating state a
     * WindowRule or the user set *before* ki3 ever touched the window, so a
     * test can prove ki3 restores exactly that instead of a hardcoded default
     * (review finding M2) and that a sticky window is excluded from
     * auto-tiling (review finding M4). No-op (returns false) unless
     * KI3_TEST_HOOKS is set, or if there is no active window.
     */
    Q_SCRIPTABLE bool dbusSetActiveWindowNoBorder(bool noBorder);

    /** Test-only counterpart of dbusSetActiveWindowNoBorder() for keep-above. */
    Q_SCRIPTABLE bool dbusSetActiveWindowKeepAbove(bool keepAbove);

    /** Test-only counterpart of dbusSetActiveWindowNoBorder() for all-desktops (sticky). */
    Q_SCRIPTABLE bool dbusSetActiveWindowOnAllDesktops(bool onAllDesktops);

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
     * Undo everything ki3 wrote into KWin's *windows* and *tile trees* (as
     * opposed to Ki3SessionGuard::restoreOnCleanExit(), which only undoes
     * config ki3 wrote): destroy floating chrome and hand back every
     * floating/tiled window's pre-ki3 decoration/keep-above state (via
     * m_tileTree), then drop ki3's own split structure so a fresh load (or
     * KWin's default layout) starts clean. Called from ~Ki3Tiler(), on the
     * same "runs before Workspace/VirtualDesktopManager teardown" guarantee
     * documented on Ki3SessionGuard::restoreOnCleanExit().
     *
     * This makes *this destructor* safe to run to completion; it does not by
     * itself make KWin's live PluginManager UnloadPlugin/LoadPlugin D-Bus
     * round-trip safe to use for hot-reloading ki3 -- that hit a separate,
     * KWin-level bug where a `false` return from LoadPlugin didn't reliably
     * mean "not loaded", producing two simultaneous Ki3Tiler instances (see
     * ki3-PLAN.md, "Hot-reload support" and its 2026-07-07 revert). Runtime
     * hot-unload remains unsupported for that reason; this teardown exists
     * for ordinary process-exit and to leave KWin in a clean state on the
     * off chance something else does unload the plugin at runtime.
     */
    void teardownManagedState();

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
     * Give each output a starting desktop so no desktop number is shown on two
     * screens at once — the i3/sway invariant. Outputs matching a configured
     * entry in m_workspaceOutputPreference (see loadWorkspaceOutputPreferences())
     * get that desktop number; every other output falls back to the lowest
     * unreserved number, output i -> desktop i+1 in enumeration order (today's
     * behaviour, unchanged, when nothing is configured).
     */
    void assignInitialDesktops();

    /**
     * Load the user's desktop -> output priority list from `ki3rc`'s
     * `[Workspaces]` group (i3/sway `workspace <n> output <o1> <o2> ...` model:
     * key = desktop number, value = comma-separated output names, first
     * connected one wins) into m_workspaceOutputPreference. Called once at
     * construction.
     */
    void loadWorkspaceOutputPreferences();

    /**
     * Apply m_workspaceOutputPreference against the outputs currently connected:
     * for each configured desktop number (ascending, so a lower number wins a
     * conflicting claim), give it the first output in its priority list that is
     * connected and not already claimed by an earlier desktop in this same pass.
     * If that output currently shows a *different* desktop, that stale holder is
     * evicted to a free desktop first — self-contained, so the duplicate can
     * never end up resolved the wrong way by enforceUniqueDesktops()'s later,
     * order-dependent dedup pass. Every output this claims is added to
     * @p claimedOutputs so the caller's own fallback/dedup logic leaves it alone.
     * No-op (and touches nothing) for any desktop with no configured entry, or
     * whose preferred outputs are all disconnected or already claimed — that's
     * the "no matching profile" default policy, unchanged from before.
     */
    void claimConfiguredOutputs(QSet<LogicalOutput *> &claimedOutputs);

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
     * Synchronously tear down any tab/stack group living on @p output, right
     * as it's being removed -- *before* KWin destroys the output's
     * TileManager/RootTile/CustomTile tree (Workspace::outputRemoved fires
     * before that happens; workspace.cpp's output-removal loop only tears
     * the tile tree down afterwards). scheduleReconcile()'s queued
     * reconcileOutputs() runs too late for this specifically: a group's
     * CustomTile stays connected to Tile::windowGeometryChanged for its whole
     * life, and something during that destruction cascade can re-emit it on a
     * tile whose destructor has already partly run -- Qt's "class destructor
     * may have already run" safety assertion, a real reproducible crash (see
     * ki3-PLAN.md). Calling m_tileTree->dropGroup() here first removes the
     * connection proactively, so by the time the real destruction happens
     * there's nothing left of ki3's to crash regardless of the exact
     * KWin-internal re-entrancy path.
     */
    void teardownGroupsOnOutput(LogicalOutput *output);

    /**
     * Queue a single (coalesced) reconcile after an output was plugged/unplugged.
     * Deferred so it runs once KWin has finished re-homing windows and tile trees.
     */
    void scheduleReconcile();

    /** Reconcile ki3 state with the current set of outputs (see scheduleReconcile). */
    void reconcileOutputs();

    /**
     * Re-assert the one-desktop-per-screen invariant, moving duplicates to free
     * desktops. Starts with claimConfiguredOutputs() so a configured preference
     * (e.g. a monitor that was unplugged and just came back) reclaims its output
     * before the generic dedup pass below runs — this is what makes the
     * preference sway-style *live*, not just applied at startup.
     */
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

    // Reversible-session backup/restore of the real kwinrc groups + global
    // shortcuts ki3 overwrites, and their restoration on clean exit. See
    // ki3sessionguard.h / ki3session.cpp. Constructed first in Ki3Tiler's
    // constructor, before anything that mutates kwinrc/shortcuts.
    std::unique_ptr<Ki3SessionGuard> m_sessionGuard;

    // ki3's tile tree, tab/stack group model, and floating-window set -- see
    // TileTreeController's own doc comment. Constructed right after
    // m_sessionGuard (before anything that inserts a window).
    std::unique_ptr<TileTreeController> m_tileTree;

    // Tile-border/split/resize-indicator/floating-chrome rendering -- see
    // DecorationController's own doc comment. Constructed right after
    // m_tileTree (which it depends on, read-only).
    std::unique_ptr<DecorationController> m_decoration;

    // Whether Meta+R resize mode is active; see toggleResizeMode().
    bool m_resizeMode = false;

    // User's desktop -> output priority list, from ki3rc [Workspaces]. Desktop
    // number -> ordered output names (first connected one wins). Empty entries
    // (or numbers with no configured outputs) mean "no preference, use the
    // default policy" — see claimConfiguredOutputs().
    QMap<int, QStringList> m_workspaceOutputPreference;

    // Coalesces output plug/unplug events into a single deferred reconcile.
    bool m_reconcilePending = false;

    // Test-only: virtual outputs hot-plugged via dbusAddTestOutput(), newest
    // last. QPointer so an output that vanishes another way self-clears.
    QList<QPointer<BackendOutput>> m_testOutputs;
    int m_testOutputSeq = 0;

    // Coalesces pruneEmptyDesktops calls after window removal / workspace switch.
    bool m_prunePending = false;

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

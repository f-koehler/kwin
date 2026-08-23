/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ki3decorationcontroller.h"
#include "ki3sessionguard.h"
#include "ki3tiletreecontroller.h"
#include "ki3workspacecontroller.h"
#include "plugin.h"

#include <QList>
#include <QStringList>

#include <memory>

namespace KWin
{

class Window;

/**
 * Exported on D-Bus as KWin's own /Ki3 object (service "org.kde.KWin", which
 * KWin's DBusInterface already owns) so external UI — e.g. the ki3-pager
 * plasmoid, since Plasma's stock Pager assumes one global desktop and can't
 * represent ki3's per-output model — can show/drive per-output desktops
 * without linking against KWin internals.
 *
 * As of the M1 refactor's Phase 1-4, this class delegates the tile tree,
 * tab/stack group model, and floating-window set to TileTreeController
 * (m_tileTree); tile-border/split/resize-indicator/floating-chrome rendering
 * to DecorationController (m_decoration); the per-output virtual-desktop
 * model, output reconciliation, and most D-Bus-introspection query bodies to
 * WorkspaceController (m_workspace); and reversible-session config
 * backup/restore to Ki3SessionGuard (m_sessionGuard) -- see
 * `~/.claude/plans/toasty-fluttering-kitten.md` and the matching
 * ki3-PLAN.md entries. What remains here: plugin lifecycle, global
 * shortcuts, and the D-Bus surface itself (every Q_SCRIPTABLE slot backed by
 * WorkspaceController is a one-line forwarder, since the registered /Ki3
 * object must stay this class).
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
     * Name of the output ki3 currently considers focused (see
     * WorkspaceController::focusedOutput()), or empty if none. Lets the
     * ki3-pager plasmoid mute its active-desktop highlight on every screen
     * except this one.
     */
    Q_SCRIPTABLE QString focusedOutputName() const;

    /** D-Bus wrapper for WorkspaceController::switchToWorkspace(), for the ki3-pager plasmoid. */
    Q_SCRIPTABLE void dbusSwitchToWorkspace(int number);

    /** D-Bus wrapper for WorkspaceController::moveActiveToWorkspace(), for the ki3-pager plasmoid. */
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

    // Per-output virtual-desktop model, output reconciliation, and most of
    // the D-Bus introspection query bodies -- see WorkspaceController's own
    // doc comment. Constructed right after m_decoration (which it depends
    // on, read-only, alongside m_tileTree).
    std::unique_ptr<WorkspaceController> m_workspace;

    // Whether Meta+R resize mode is active; see toggleResizeMode().
    bool m_resizeMode = false;

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

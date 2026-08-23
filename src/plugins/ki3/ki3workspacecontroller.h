/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/backendoutput.h"
#include "ki3decorationcontroller.h"
#include "ki3tiletreecontroller.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

namespace KWin
{

class BackendOutput;
class LogicalOutput;
class VirtualDesktop;

/**
 * Owns ki3's per-output virtual-desktop model: the one-desktop-per-screen
 * invariant, the configured desktop -> output priority list, output plug/
 * unplug reconciliation, and the test-only output hotplug hooks. Fourth
 * controller extracted from the `Ki3Tiler` god object (M1 refactor, Phase 4)
 * -- see `~/.claude/plans/toasty-fluttering-kitten.md` and the matching
 * ki3-PLAN.md entry.
 *
 * Also holds most of the plugin's D-Bus-introspection query bodies
 * (`outputNames()`, `tiledWindowGeometries()`, the `KI3_TEST_HOOKS` setters,
 * ...) -- these lived in the same `ki3workspaces.cpp` file before this
 * extraction, so they moved here too rather than being split by finer
 * semantic categories. The actual `/Ki3` D-Bus object is (and must stay)
 * `Ki3Tiler` itself -- see its class doc comment -- so every `Q_SCRIPTABLE`
 * slot there is a one-line forwarder into this class.
 *
 * Depends one-directionally on TileTreeController and DecorationController
 * (both non-owning pointers, given at construction): reconciliation reads
 * and mutates TileTreeController's tile-tree membership
 * (`shouldManage()`/`isManaged()`/`forgetWindow()`/`insertWindow()`/
 * `rootForWindow()`/`isFloating()`/`purgeStaleRoots()`/`dropGroup()`) and
 * asks DecorationController to redraw after an output/desktop change with no
 * window to trigger it otherwise (`updateSplitIndicator()`,
 * `repositionFloatChrome()`). Neither sibling controller has any knowledge
 * of this class in return.
 */
class WorkspaceController : public QObject
{
    Q_OBJECT

public:
    explicit WorkspaceController(TileTreeController *tileTree, DecorationController *decoration,
                                 QObject *parent = nullptr);

    /** Output names (matches Qt/QScreen::name(), e.g. "DP-1") known to KWin. */
    QStringList outputNames() const;

    /** 1-based current desktop number shown on @p outputName, or 0 if unknown. */
    int currentDesktopNumber(const QString &outputName) const;

    /** Number of currently live desktops. */
    int desktopCount() const;

    /** Workspace numbers (parsed from desktop names) of all currently live desktops, in order. */
    QList<int> liveDesktopNumbers() const;

    /**
     * Workspace numbers that "belong" to @p outputName, in order: the desktops
     * whose windows live on that output (their home output) plus the desktop
     * currently shown there. This is the i3/sway per-output workspace set — the
     * pager on each screen shows only these, never every output's desktops.
     */
    QList<int> desktopNumbersForOutput(const QString &outputName) const;

    /**
     * Name of the output ki3 currently considers focused (see focusedOutput()),
     * or empty if none. Lets the ki3-pager plasmoid mute its active-desktop
     * highlight on every screen except this one.
     */
    QString focusedOutputName() const;

    /**
     * Test/introspection: one entry per window ki3 tiles, each
     * "<output>|<desktop>|<x>,<y>,<w>,<h>" using the window's *actual* applied
     * frame geometry (not the tile's intended rect). Lets a test assert that a
     * window really laid out where its tile says -- e.g. that a cross-output move
     * onto a hidden workspace didn't leave windows overlapping. The debug log
     * only prints intended tile geometry, so this is the only ground truth for
     * "did the geometry actually get applied".
     */
    QStringList tiledWindowGeometries() const;

    /**
     * Test/introspection: one entry per window ki3 currently floats, each
     * "<output>|<x>,<y>,<w>,<h>" using the window's *actual* applied frame
     * geometry (the client area ki3 moved down to make room for its title
     * bar -- see DecorationController::createFloatChrome()), not the chrome's
     * geometry. Lets a test assert the client stayed within the output's
     * usable area after floating near an edge (review finding M3).
     */
    QStringList floatingWindowGeometries() const;

    /** Test/introspection: the active window's current decoration policy -- true if borderless. */
    bool activeWindowNoBorder() const;

    /** Test/introspection: the active window's current keep-above state. */
    bool activeWindowKeepAbove() const;

    /**
     * Test-only: hot-plug a virtual output and return its name (empty on
     * failure). No-op unless the env var KI3_TEST_HOOKS is set, so it can never
     * spawn a phantom screen in a real session. Used by the regression suite to
     * exercise output plug/unplug (createVirtualOutput → Workspace::outputAdded,
     * the same path a real monitor takes). See tests/harness.py `hotplug`.
     */
    QString addTestOutput();

    /** Test-only counterpart: unplug the most recently added test output. */
    QString removeTestOutput();

    /**
     * Test-only: directly set the active window's decoration policy/keep-above/
     * all-desktops state, bypassing ki3 entirely -- simulating state a
     * WindowRule or the user set *before* ki3 ever touched the window, so a
     * test can prove ki3 restores exactly that instead of a hardcoded default
     * (review finding M2) and that a sticky window is excluded from
     * auto-tiling (review finding M4). No-op (returns false) unless
     * KI3_TEST_HOOKS is set, or if there is no active window.
     */
    bool setActiveWindowNoBorder(bool noBorder);

    /** Test-only counterpart of setActiveWindowNoBorder() for keep-above. */
    bool setActiveWindowKeepAbove(bool keepAbove);

    /** Test-only counterpart of setActiveWindowNoBorder() for all-desktops (sticky). */
    bool setActiveWindowOnAllDesktops(bool onAllDesktops);

    /** Switch to workspace (virtual desktop) @p number (1-based), i3/sway-style. */
    void switchToWorkspace(int number);

    /** Move the active window to workspace @p number (1-based), following it to its output. */
    void moveActiveToWorkspace(int number);

    /**
     * Give each output a starting desktop so no desktop number is shown on two
     * screens at once — the i3/sway invariant. Outputs matching a configured
     * entry in `ki3rc`'s `[Workspaces]` group (see loadWorkspaceOutputPreferences(),
     * called from this constructor) get that desktop number; every other output
     * falls back to the lowest unreserved number, output i -> desktop i+1 in
     * enumeration order (today's behaviour, unchanged, when nothing is
     * configured). Called once from Ki3Tiler's constructor.
     */
    void assignInitialDesktops();

    /**
     * Queue a single (coalesced) pruneEmptyDesktops() after the current
     * event. Called from Ki3Tiler::handleWindowRemoved(): let KWin finish
     * destroying the window before checking whether its desktop became
     * empty (the window may still appear in workspace()->windows()).
     */
    void schedulePrune();

Q_SIGNALS:
    /**
     * Emitted whenever any output's current desktop changes as a side effect
     * of moveActiveToWorkspace(). `Ki3Tiler` connects this to its own
     * Q_SCRIPTABLE desktopsChanged() signal -- the D-Bus-visible one must be
     * emitted from the registered `/Ki3` object itself (Ki3Tiler), not this
     * class, so this is a plain internal forwarding signal.
     */
    void desktopsChanged();

private:
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
     * ki3-PLAN.md). Calling TileTreeController::dropGroup() here first removes
     * the connection proactively, so by the time the real destruction happens
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

    // Non-owning; both outlive this (constructed before it in Ki3Tiler's
    // constructor, destroyed after it). Every call into either is read-only
    // or a narrow, well-defined mutation -- see the class doc comment above.
    TileTreeController *m_tileTree;
    DecorationController *m_decoration;

    // User's desktop -> output priority list, from ki3rc [Workspaces]. Desktop
    // number -> ordered output names (first connected one wins). Empty entries
    // (or numbers with no configured outputs) mean "no preference, use the
    // default policy" — see claimConfiguredOutputs().
    QMap<int, QStringList> m_workspaceOutputPreference;

    // Coalesces output plug/unplug events into a single deferred reconcile.
    bool m_reconcilePending = false;

    // Test-only: virtual outputs hot-plugged via addTestOutput(), newest
    // last. QPointer so an output that vanishes another way self-clears.
    QList<QPointer<BackendOutput>> m_testOutputs;
    int m_testOutputSeq = 0;

    // Coalesces pruneEmptyDesktops calls after window removal / workspace switch.
    bool m_prunePending = false;
};

} // namespace KWin

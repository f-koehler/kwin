/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ki3decorationcontroller.h"
#include "ki3sessionguard.h"
#include "ki3tiletreecontroller.h"
#include "ki3workspacecontroller.h"

#include <QList>
#include <QObject>

namespace KWin
{

class Ki3Tiler;

/**
 * Registers ki3's global shortcuts and dispatches them into whichever
 * controller owns the acted-on state, plus the two behaviors with no state
 * of their own (spawning a terminal, closing the active window). Fifth and
 * last controller extracted from the `Ki3Tiler` god object (M1 refactor,
 * Phase 5) -- see `~/.claude/plans/toasty-fluttering-kitten.md` and the
 * matching ki3-PLAN.md entry. After this phase `Ki3Tiler` is the thin
 * orchestration shell the plan's dependency graph always intended: it
 * constructs and wires together all five controllers and stays the object
 * that owns plugin lifecycle, cross-controller orchestration
 * (`handleWindowAdded()` and friends, `toggleFloating()`), and the `/Ki3`
 * D-Bus surface.
 *
 * Depends on every other controller (non-owning pointers, given at
 * construction) -- the thinnest controller, with nothing left to block on,
 * per the plan's extraction order. The one deliberate exception to "no
 * controller calls back up into `Ki3Tiler`": the float-toggle shortcut needs
 * `Ki3Tiler::toggleFloating()` itself, a genuine two-controller sequence
 * (`TileTreeController::isFloating/forgetWindow/insertWindow` +
 * `DecorationController::createFloatChrome/destroyFloatChrome`) that has no
 * single-controller home and was never going to move here — so this class
 * also takes a non-owning `Ki3Tiler *` purely to invoke that one method.
 */
class ShortcutController : public QObject
{
    Q_OBJECT

public:
    explicit ShortcutController(Ki3SessionGuard *sessionGuard, TileTreeController *tileTree,
                                DecorationController *decoration, WorkspaceController *workspace,
                                Ki3Tiler *tiler, QObject *parent = nullptr);

    /**
     * Register global shortcuts. Called explicitly from Ki3Tiler's
     * constructor, *after* Ki3SessionGuard::backupIfNeeded() -- registering
     * any earlier would mean every key this steals gets captured against a
     * backup that was never actually taken (m_captureShortcutOwners still
     * false), silently losing the reversible-session guarantee.
     */
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

    /** Whether Meta+R resize mode (see registerShortcuts()) is currently active. */
    bool resizeModeActive() const
    {
        return m_resizeMode;
    }

    /**
     * Enter/leave resize mode; shared by the internal Meta+R toggle, the bare
     * Escape/Return exit shortcuts (see registerResizeModeShortcuts()), and
     * the auto-exit Ki3Tiler::handleWindowRemoved() triggers (closing the
     * last window leaves nothing to resize). No-op if @p active already
     * matches the current state, so the auto-exit doesn't spam the log/D-Bus
     * signal on every window close. Binds/unbinds the bare-key resize
     * shortcuts to approximate i3/sway's keyboard grab — see
     * registerResizeModeShortcuts().
     */
    void setResizeMode(bool active);

Q_SIGNALS:
    /**
     * Emitted whenever resize mode is toggled on or off. `Ki3Tiler` connects
     * this to its own Q_SCRIPTABLE resizeModeChanged() signal -- the
     * D-Bus-visible one must be emitted from the registered `/Ki3` object
     * itself (Ki3Tiler), not this class, so this is a plain internal
     * forwarding signal (same pattern as TileTreeController::layoutChanged()
     * and WorkspaceController::desktopsChanged()).
     */
    void resizeModeChanged();

private:
    /**
     * Toggle resize mode (i3/sway "mode resize", `Meta+R`): while active, bare
     * h/j/k/l/arrow keys (no modifier) resize the focused leaf, Escape/Return
     * leave the mode, and every other ki3 shortcut is inert — matching i3/sway,
     * where entering resize mode grabs the keyboard so nothing else fires until
     * the mode is explicitly left. Press `Meta+R` again (always live, see
     * registerShortcuts()) to leave the mode from outside it too.
     */
    void toggleResizeMode();

    /** Move keyboard focus in @p edge direction (bound to Meta+h/j/k/l/arrows). */
    void handleDirectional(Qt::Edge edge);

    /** Launch a terminal emulator (i3-style Meta+Return). */
    void spawnTerminal();

    /** Close the active window (i3/sway-style Meta+Shift+Q). */
    void closeActiveWindow();

    // Non-owning; all four outlive this (constructed before it in Ki3Tiler's
    // constructor, destroyed after it). See the class doc comment above for
    // which of each is called and why.
    Ki3SessionGuard *m_sessionGuard;
    TileTreeController *m_tileTree;
    DecorationController *m_decoration;
    WorkspaceController *m_workspace;
    Ki3Tiler *m_tiler;

    // Whether Meta+R resize mode is active; see setResizeMode().
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

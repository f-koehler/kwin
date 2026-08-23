/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QKeySequence>
#include <QString>

namespace KWin
{

/**
 * Reversible-session support: ki3 currently uses the user's real ~/.config
 * (whole-directory isolation was tried and dropped -- see ki3-PLAN.md
 * 2026-06-24 -- it broke non-KDE terminal tools). Instead of isolating
 * config, Ki3SessionGuard snapshots the *specific* real kwinrc groups /
 * global shortcuts ki3 is about to overwrite the first time it runs, and
 * restores them on a clean exit, so switching back to plain Plasma sees
 * Plasma's own settings again, and switching back into ki3 re-applies
 * ki3's. See ki3-PLAN.md for the full write-up, and ki3session.cpp for the
 * implementation.
 *
 * Owned by Ki3Tiler (constructed first, before anything that mutates
 * kwinrc/global shortcuts) and used from both Ki3Tiler's own ctor/dtor and
 * its shortcut-registration code. Deliberately not a QObject: it has no
 * signals/slots and nothing here needs Qt's meta-object machinery.
 */
class Ki3SessionGuard
{
public:
    Ki3SessionGuard() = default;

    /**
     * Snapshot whatever ki3 is about to overwrite in the *real* kwinrc/
     * kglobalshortcutsrc -- but only the first time, guarded by
     * `ki3rc [SessionBackup] Pending`. A leftover `Pending=true` from an
     * unclean previous exit means the real config *already* holds ki3's own
     * values, not the user's; re-snapshotting then would capture ki3's state
     * as if it were the original, corrupting the restore this is meant to
     * provide. Called first thing in Ki3Tiler's constructor, before anything
     * that mutates kwinrc/global shortcuts. See restoreOnCleanExit() and
     * ki3session.cpp for the full design (ki3-PLAN.md has the write-up).
     */
    void backupIfNeeded();

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
    void noteShortcutGrab(const QKeySequence &key);

    /**
     * Undo backupIfNeeded()'s overwrites: restore the real kwinrc groups ki3
     * touched and hand every captured shortcut back to its original owner
     * via KGlobalAccel's setForeignShortcut(). Called from ~Ki3Tiler(),
     * which runs on a normal logout well before Workspace/
     * VirtualDesktopManager teardown (Application::destroyPlugins() runs
     * before destroyWorkspace() in ApplicationWayland::~ApplicationWayland(),
     * see main_wayland.cpp) -- so this can still use the live APIs. A no-op
     * if nothing is pending (nothing was ever backed up, or a clean exit
     * already restored and cleared it). No effect on a hard crash/SIGKILL,
     * which skips destructors entirely -- session/ki3-restore-if-pending.sh
     * (run at the next normal Plasma login via XDG autostart) is the
     * fallback for that case.
     */
    void restoreOnCleanExit();

private:
    /** True if @p actionUniqueName is one of ki3's own "ki3_*" action ids. */
    bool ki3OwnsAction(const QString &actionUniqueName) const;

    // Set once by backupIfNeeded(): true if this session took a fresh
    // SessionBackup snapshot, false if one was already Pending from an
    // unclean exit (meaning the real config already holds ki3's own values,
    // so there's nothing genuine left to capture this session). Gates every
    // noteShortcutGrab() call for the rest of the session. See
    // ki3session.cpp.
    bool m_captureShortcutOwners = false;
};

} // namespace KWin

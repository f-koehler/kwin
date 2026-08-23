/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Reversible-session support: ki3 currently uses the user's real ~/.config
// (whole-directory isolation was tried and dropped -- see ki3-PLAN.md
// 2026-06-24 -- it broke non-KDE terminal tools). Instead of isolating
// config, ki3 snapshots the *specific* real kwinrc groups / global shortcuts
// it's about to overwrite the first time it runs, and restores them on a
// clean exit, so switching back to plain Plasma sees Plasma's own settings
// again, and switching back into ki3 re-applies ki3's. See ki3-PLAN.md for
// the full write-up (dated entry alongside this file's introduction).

#include "ki3sessionguard.h"

#include "ki3_logging.h"

#include "virtualdesktops.h"

#include <KConfigGroup>
#include <KGlobalAccel>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QStringList>

#include <algorithm>

namespace KWin
{

namespace
{

// The real kwinrc groups ki3 either writes directly (Effect-overview, via
// disableOverviewHotCorner()) or causes KWin core to write as a side effect
// of ki3's own operation (Desktops, via VirtualDesktopManager::setCount() in
// ki3workspaces.cpp; Tiling, via TileManager's own debounced autosave --
// tilemanager.cpp -- reacting to every CustomTile split/insert).
const QStringList &kwinrcBackupGroups()
{
    static const QStringList groups = {
        QStringLiteral("Effect-overview"),
        QStringLiteral("Desktops"),
        QStringLiteral("Tiling"),
    };
    return groups;
}

} // namespace

void Ki3SessionGuard::backupIfNeeded()
{
    KSharedConfigPtr ki3Config = KSharedConfig::openConfig(QStringLiteral("ki3rc"));
    KConfigGroup backup = ki3Config->group(QStringLiteral("SessionBackup"));

    if (backup.readEntry("Pending", false)) {
        // A previous ki3 session didn't exit cleanly: the real config already
        // holds *ki3's* values, not the user's. Snapshotting now would
        // capture ki3's own state as if it were the original, permanently
        // losing the real one. Just keep going -- registerShortcuts() etc.
        // still need to (re-)apply ki3's overrides this session, there's
        // just nothing new to capture while doing so.
        qCInfo(KWIN_KI3) << "session backup already pending (unclean previous exit) -- not re-snapshotting";
        m_captureShortcutOwners = false;
        return;
    }

    KSharedConfigPtr kwinConfig = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    KConfigGroup kwinrcBackupRoot = backup.group(QStringLiteral("KwinrcBackup"));
    for (const QString &name : kwinrcBackupGroups()) {
        KConfigGroup dst = kwinrcBackupRoot.group(name);
        dst.deleteGroup(); // discard any stale copy from an aborted earlier attempt
        kwinConfig->group(name).copyTo(&dst);
    }

    backup.writeEntry("Pending", true);
    backup.sync();
    m_captureShortcutOwners = true;
    qCInfo(KWIN_KI3) << "session backup taken";
}

bool Ki3SessionGuard::ki3OwnsAction(const QString &actionUniqueName) const
{
    // Every ki3 QAction's objectName (== its KGlobalAccel action id) is
    // "ki3_..." -- see registerShortcuts()/registerResizeModeShortcuts().
    return actionUniqueName.startsWith(QLatin1String("ki3_"));
}

void Ki3SessionGuard::noteShortcutGrab(const QKeySequence &key)
{
    if (!m_captureShortcutOwners || key.isEmpty()) {
        return;
    }
    const QList<KGlobalShortcutInfo> owners = KGlobalAccel::globalShortcutsByKey(key);
    if (owners.isEmpty()) {
        return; // nobody held it -- nothing to give back later
    }
    const KGlobalShortcutInfo &owner = owners.first();
    if (ki3OwnsAction(owner.uniqueName())) {
        // Already ki3's own, from earlier this same session (e.g. a second
        // Meta+R resize-mode activation re-stealing a key it grabbed the
        // first time). Nothing new to capture.
        return;
    }

    KSharedConfigPtr ki3Config = KSharedConfig::openConfig(QStringLiteral("ki3rc"));
    KConfigGroup backup = ki3Config->group(QStringLiteral("SessionBackup"));
    const int count = backup.readEntry("ShortcutCount", 0);

    // An action can own several keys (e.g. ki3 itself binds {vim-key,
    // arrow-key} pairs to one action) -- don't record the same action twice
    // just because more than one of its keys gets stolen.
    for (int i = 0; i < count; ++i) {
        const KConfigGroup existing = backup.group(QStringLiteral("Shortcut_%1").arg(i));
        if (existing.readEntry("Component") == owner.componentUniqueName()
            && existing.readEntry("Action") == owner.uniqueName()) {
            return;
        }
    }

    KConfigGroup entry = backup.group(QStringLiteral("Shortcut_%1").arg(count));
    entry.writeEntry("Component", owner.componentUniqueName());
    entry.writeEntry("Action", owner.uniqueName());
    entry.writeEntry("ComponentFriendly", owner.componentFriendlyName());
    entry.writeEntry("ActionFriendly", owner.friendlyName());
    // KGlobalAccel's setForeignShortcut() takes raw Qt key ints (D-Bus "ai"),
    // not key-sequence strings -- store those directly (as stringified ints;
    // KConfig QStringLists are just comma-joined text) so both this class's
    // own restore below *and* the crash-recovery fallback script
    // (session/ki3-restore-if-pending.py, which has no Qt to decode a
    // "Meta+W"-style string with) can use them as-is, no re-parsing needed.
    // "Keys" alongside is the human-readable form, for debug logging only.
    QStringList keyInts;
    QStringList keyStrings;
    keyInts.reserve(owner.keys().size());
    keyStrings.reserve(owner.keys().size());
    for (const QKeySequence &seq : owner.keys()) {
        keyInts << QString::number(seq[0].toCombined());
        keyStrings << seq.toString(QKeySequence::PortableText);
    }
    entry.writeEntry("KeyInts", keyInts);
    entry.writeEntry("Keys", keyStrings);

    backup.writeEntry("ShortcutCount", count + 1);
    backup.sync();
    qCDebug(KWIN_KI3) << "captured shortcut owner" << owner.componentUniqueName() << owner.uniqueName()
                      << "keys" << keyStrings;
}

void Ki3SessionGuard::restoreOnCleanExit()
{
    KSharedConfigPtr ki3Config = KSharedConfig::openConfig(QStringLiteral("ki3rc"));
    KConfigGroup backup = ki3Config->group(QStringLiteral("SessionBackup"));
    if (!backup.readEntry("Pending", false)) {
        return; // nothing was ever backed up, or a clean exit already restored it
    }

    // 1. kwinrc groups, verbatim -- delete the live group first so anything
    //    ki3's session added that wasn't in the original (e.g. a new
    //    [Tiling] desktop/output subgroup) doesn't survive as an orphan.
    KSharedConfigPtr kwinConfig = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    KConfigGroup kwinrcBackupRoot = backup.group(QStringLiteral("KwinrcBackup"));
    for (const QString &name : kwinrcBackupGroups()) {
        KConfigGroup dst = kwinConfig->group(name);
        dst.deleteGroup();
        kwinrcBackupRoot.group(name).copyTo(&dst);
    }
    kwinConfig->sync();

    // 2. Desktops, also through the live API so in-memory state matches the
    //    restored file too -- belt-and-suspenders against a redundant
    //    VirtualDesktopManager::save() later in the shutdown sequence
    //    re-writing ki3's still-live (larger) desktop count over the file
    //    we just restored. [Tiling]/[Effect-overview] have no equivalent
    //    live manager that could re-save them in the remaining shutdown
    //    window, so the plain file restore above is enough for those.
    const KConfigGroup desktopsBackup = kwinrcBackupRoot.group(QStringLiteral("Desktops"));
    const int originalCount = desktopsBackup.readEntry("Number", 1);
    VirtualDesktopManager *vdm = VirtualDesktopManager::self();
    vdm->setCount(uint(std::max(1, originalCount)));
    for (VirtualDesktop *desktop : vdm->desktops()) {
        const QString name = desktopsBackup.readEntry(
            QStringLiteral("Name_%1").arg(desktop->x11DesktopNumber()), QString());
        if (!name.isEmpty()) {
            desktop->setName(name);
        }
    }

    // 3. Shortcuts: hand each captured one back to its original action via
    //    KGlobalAccel's setForeignShortcut(). Not wrapped by the KGlobalAccel
    //    C++ convenience class (only globalShortcutsByKey()/
    //    stealShortcutSystemwide()/setShortcut() are) and there's no
    //    generated D-Bus interface header for it in KWin's tree (unlike
    //    org.kde.KWin's /Effects, used by disableOverviewHotCorner()), so
    //    call it dynamically. actionId tuple order (componentUnique,
    //    actionUnique, componentFriendly, actionFriendly) confirmed by a
    //    live round-trip test against org.kde.kglobalaccel before this was
    //    wired in -- see ki3-PLAN.md.
    QDBusInterface kglobalaccel(QStringLiteral("org.kde.kglobalaccel"),
                                QStringLiteral("/kglobalaccel"),
                                QStringLiteral("org.kde.KGlobalAccel"),
                                QDBusConnection::sessionBus());
    const int shortcutCount = backup.readEntry("ShortcutCount", 0);
    for (int i = 0; i < shortcutCount; ++i) {
        const KConfigGroup entry = backup.group(QStringLiteral("Shortcut_%1").arg(i));
        const QString component = entry.readEntry("Component");
        const QString action = entry.readEntry("Action");
        if (component.isEmpty() || action.isEmpty()) {
            continue;
        }
        const QStringList actionId = {
            component,
            action,
            entry.readEntry("ComponentFriendly"),
            entry.readEntry("ActionFriendly"),
        };
        QList<int> keys;
        for (const QString &keyInt : entry.readEntry("KeyInts", QStringList())) {
            bool ok = false;
            const int combined = keyInt.toInt(&ok);
            if (ok) {
                keys << combined;
            }
        }
        const QDBusMessage reply = kglobalaccel.call(QStringLiteral("setForeignShortcut"),
                                                     QVariant::fromValue(actionId),
                                                     QVariant::fromValue(keys));
        if (reply.type() == QDBusMessage::ErrorMessage) {
            qCWarning(KWIN_KI3) << "failed to restore shortcut" << component << action
                                << "-" << reply.errorMessage();
        } else {
            qCInfo(KWIN_KI3) << "restored shortcut" << component << action << "->" << keys;
        }
    }

    backup.deleteGroup();
    ki3Config->sync();
    qCInfo(KWIN_KI3) << "session state restored on clean exit";
}

} // namespace KWin

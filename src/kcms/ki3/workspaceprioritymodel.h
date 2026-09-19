/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QAbstractListModel>
#include <QMap>
#include <QStringList>

class QDBusServiceWatcher;

namespace KWin
{

/**
 * Editor model for `ki3rc`'s `[Workspaces]` group: which outputs a given
 * desktop number prefers, in priority order (i3/sway `workspace <n> output
 * <o1> <o2> ...`) -- see `WorkspaceController::loadWorkspaceOutputPreferences()`
 * (`kwin/src/plugins/ki3/ki3workspacecontroller.cpp`) for the exact key format
 * this reads/writes (group key = desktop number as a string, value = a
 * comma-separated `QStringList` of output names).
 *
 * The *rows* (which desktop numbers are worth editing) come entirely from
 * local config, not the running compositor: the union of 1..@ref
 * kBaseDesktopCount (matching the Meta+1..9 shortcuts every ki3 session
 * registers -- see README.md) and whatever desktop numbers already have a
 * `[Workspaces]` entry (in case of a hand-edited higher number). This is
 * deliberate -- an earlier version used `liveDesktopNumbers()` (which desktop
 * a ki3 session currently has instantiated), but ki3 only keeps a desktop
 * "live" while it's shown or has windows (see README.md's "empty desktops are
 * removed on the fly"), so that only ever showed whichever workspaces
 * happened to be open at the moment System Settings was launched -- useless
 * for *pre*-configuring a workspace's preferred output before ever opening
 * it, and it silently changed if the live session's desktops changed after
 * the KCM had already loaded (no re-fetch on window open).
 *
 * The *output names* offered to add to a row's priority list still can't
 * come from local config -- only a running compositor knows which outputs
 * are currently connected -- so those still come over the same
 * "org.kde.KWin" / "/Ki3" / "org.kde.ki3" D-Bus object ki3-pager already uses
 * (see `ki3-pager/ki3pagerbackend.cpp`). If ki3 isn't currently running (no
 * session, or a different tiling setup), `/Ki3` doesn't exist -- @ref
 * available reports that so the QML side can explain why the "add output"
 * picker has nothing to offer. Existing entries (already in a row's priority
 * list, e.g. from a previous session with a different monitor connected)
 * still show and can be removed/reordered either way, since removing/
 * reordering only needs the *names already in the list*, not a live output
 * enumeration.
 */
class WorkspacePriorityModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QStringList allOutputNames READ allOutputNames NOTIFY allOutputNamesChanged)
    Q_PROPERTY(bool isSaveNeeded READ isSaveNeeded NOTIFY isSaveNeededChanged)

public:
    enum Role {
        DesktopNumberRole = Qt::UserRole + 1,
        OutputPriorityRole,
    };
    Q_ENUM(Role)

    // Matches the Meta+1..9 / Meta+Shift+1..9 shortcuts README.md documents --
    // see the class doc comment for why the row set is based on this rather
    // than whatever desktops happen to be live right now.
    static constexpr int kBaseDesktopCount = 9;

    explicit WorkspacePriorityModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool available() const;
    QStringList allOutputNames() const;
    bool isSaveNeeded() const;

    /**
     * Outputs not yet in @p desktopNumber's priority list, computed in C++
     * (rather than a QML-side `Array.prototype.filter()`/`.includes()` over
     * a `QStringList`-typed property) so the "add output" combo box's model
     * doesn't depend on exactly how the QML engine bridges `QStringList` to
     * JS arrays. Empty whenever @ref available is false, since there's
     * nothing live to offer adding.
     */
    Q_INVOKABLE QStringList availableOutputsForDesktop(int desktopNumber) const;

    /** Append @p output to @p desktopNumber's priority list, if not already present. */
    Q_INVOKABLE void addOutput(int desktopNumber, const QString &output);

    /** Remove @p output from @p desktopNumber's priority list. */
    Q_INVOKABLE void removeOutput(int desktopNumber, const QString &output);

    /** Swap @p output with its predecessor in @p desktopNumber's priority list. */
    Q_INVOKABLE void moveOutputUp(int desktopNumber, const QString &output);

    /** Swap @p output with its successor in @p desktopNumber's priority list. */
    Q_INVOKABLE void moveOutputDown(int desktopNumber, const QString &output);

    /**
     * Re-query connected output names from the running ki3 (@ref available
     * becomes false, and @ref allOutputNames empty, if `/Ki3` isn't present)
     * and reload this model's rows from `ki3rc [Workspaces]`/the base range
     * (see the class doc comment). Called at construction and whenever the
     * watched D-Bus service (re)registers.
     */
    Q_INVOKABLE void reload();

    /**
     * Write every row's current priority list back to `ki3rc [Workspaces]`:
     * updates/creates the key for a non-empty list, removes the key entirely
     * for an emptied one.
     */
    void save();

    void load();
    bool isDefaults() const;
    void defaults();

Q_SIGNALS:
    void availableChanged();
    void allOutputNamesChanged();
    void isSaveNeededChanged();

private:
    void setPriority(int desktopNumber, const QStringList &outputs);
    int rowForDesktop(int desktopNumber) const;

    QDBusServiceWatcher *m_serviceWatcher;
    bool m_available = false;
    QStringList m_allOutputNames;
    QList<int> m_desktopNumbers;
    QMap<int, QStringList> m_priority; // desktop number -> ordered output names
    QMap<int, QStringList> m_savedPriority; // last loaded/saved snapshot, for isSaveNeeded()
};

} // namespace KWin

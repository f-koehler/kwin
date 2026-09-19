/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "workspaceprioritymodel.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QSet>

namespace KWin
{

static const QString s_service = QStringLiteral("org.kde.KWin");
static const QString s_path = QStringLiteral("/Ki3");
static const QString s_iface = QStringLiteral("org.kde.ki3");

WorkspacePriorityModel::WorkspacePriorityModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_serviceWatcher(new QDBusServiceWatcher(s_service, QDBusConnection::sessionBus(),
                                                QDBusServiceWatcher::WatchForOwnerChange, this))
{
    // "/Ki3" only exists while the ki3 plugin is loaded, which -- unlike
    // s_service itself (KWin's own D-Bus service, always present) -- can come
    // and go independently of this KCM's lifetime (a hot-reloaded plugin, or
    // the KCM opened outside a ki3 session at all). Re-probe on every owner
    // change rather than assuming an initial reload() stays valid.
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceRegistered, this, &WorkspacePriorityModel::reload);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &WorkspacePriorityModel::reload);
    reload();
}

int WorkspacePriorityModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_desktopNumbers.size();
}

QVariant WorkspacePriorityModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_desktopNumbers.size()) {
        return {};
    }
    const int desktopNumber = m_desktopNumbers.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return desktopNumber;
    case DesktopNumberRole:
        return desktopNumber;
    case OutputPriorityRole:
        return m_priority.value(desktopNumber);
    }
    return {};
}

QHash<int, QByteArray> WorkspacePriorityModel::roleNames() const
{
    return {
        {Qt::DisplayRole, "display"},
        {DesktopNumberRole, "desktopNumber"},
        {OutputPriorityRole, "outputPriority"},
    };
}

bool WorkspacePriorityModel::available() const
{
    return m_available;
}

QStringList WorkspacePriorityModel::allOutputNames() const
{
    return m_allOutputNames;
}

bool WorkspacePriorityModel::isSaveNeeded() const
{
    return m_priority != m_savedPriority;
}

int WorkspacePriorityModel::rowForDesktop(int desktopNumber) const
{
    return m_desktopNumbers.indexOf(desktopNumber);
}

void WorkspacePriorityModel::setPriority(int desktopNumber, const QStringList &outputs)
{
    if (m_priority.value(desktopNumber) == outputs) {
        return;
    }
    m_priority[desktopNumber] = outputs;
    const int row = rowForDesktop(desktopNumber);
    if (row >= 0) {
        const QModelIndex idx = index(row);
        Q_EMIT dataChanged(idx, idx, {OutputPriorityRole});
    }
    Q_EMIT isSaveNeededChanged();
}

void WorkspacePriorityModel::addOutput(int desktopNumber, const QString &output)
{
    QStringList outputs = m_priority.value(desktopNumber);
    if (output.isEmpty() || outputs.contains(output)) {
        return;
    }
    outputs.append(output);
    setPriority(desktopNumber, outputs);
}

void WorkspacePriorityModel::removeOutput(int desktopNumber, const QString &output)
{
    QStringList outputs = m_priority.value(desktopNumber);
    if (outputs.removeAll(output) > 0) {
        setPriority(desktopNumber, outputs);
    }
}

void WorkspacePriorityModel::moveOutputUp(int desktopNumber, const QString &output)
{
    QStringList outputs = m_priority.value(desktopNumber);
    const int i = outputs.indexOf(output);
    if (i > 0) {
        outputs.swapItemsAt(i, i - 1);
        setPriority(desktopNumber, outputs);
    }
}

void WorkspacePriorityModel::moveOutputDown(int desktopNumber, const QString &output)
{
    QStringList outputs = m_priority.value(desktopNumber);
    const int i = outputs.indexOf(output);
    if (i >= 0 && i < outputs.size() - 1) {
        outputs.swapItemsAt(i, i + 1);
        setPriority(desktopNumber, outputs);
    }
}

QStringList WorkspacePriorityModel::availableOutputsForDesktop(int desktopNumber) const
{
    if (!m_available) {
        return {};
    }
    const QStringList already = m_priority.value(desktopNumber);
    QStringList result;
    result.reserve(m_allOutputNames.size());
    for (const QString &name : m_allOutputNames) {
        if (!already.contains(name)) {
            result.append(name);
        }
    }
    return result;
}

void WorkspacePriorityModel::reload()
{
    // QDBusInterface::isValid() only checks that the *service* exists and
    // the service/path/interface strings are syntactically valid -- it does
    // NOT verify the object path actually exists remotely (confirmed: it
    // stays true against a bare "org.kde.KWin" service with no "/Ki3" object
    // registered at all, i.e. any KWin session, ki3 or not). "org.kde.KWin"
    // is provided by every KWin session regardless of whether the ki3 plugin
    // is loaded, so that check alone made `available` report true in a
    // plain, non-ki3 Plasma session too -- exactly the case this property
    // exists to detect. Actually calling a real method and checking the
    // *reply* is the only reliable way to tell.
    QDBusInterface iface(s_service, s_path, s_iface, QDBusConnection::sessionBus());
    const QDBusReply<QStringList> reply = iface.call(QStringLiteral("outputNames"));

    const bool wasAvailable = m_available;
    m_available = reply.isValid();
    if (m_available != wasAvailable) {
        Q_EMIT availableChanged();
    }

    const QStringList outputNames = m_available ? reply.value() : QStringList();
    if (outputNames != m_allOutputNames) {
        m_allOutputNames = outputNames;
        Q_EMIT allOutputNamesChanged();
    }

    load();
}

void WorkspacePriorityModel::load()
{
    // The row set: 1..kBaseDesktopCount (the Meta+1..9 shortcuts, always
    // worth pre-configuring even if never opened yet) union whatever desktop
    // numbers already have a [Workspaces] entry (e.g. a hand-edited higher
    // number) -- see the class doc comment for why this doesn't ask the
    // running compositor for its *live* desktops instead.
    const KConfigGroup group =
        KSharedConfig::openConfig(QStringLiteral("ki3rc"))->group(QStringLiteral("Workspaces"));
    QSet<int> desktopNumbers;
    for (int n = 1; n <= kBaseDesktopCount; ++n) {
        desktopNumbers.insert(n);
    }
    for (const QString &key : group.keyList()) {
        bool ok = false;
        const int n = key.toInt(&ok);
        if (ok && n >= 1) {
            desktopNumbers.insert(n);
        }
    }

    beginResetModel();
    m_desktopNumbers = QList<int>(desktopNumbers.constBegin(), desktopNumbers.constEnd());
    std::sort(m_desktopNumbers.begin(), m_desktopNumbers.end());
    endResetModel();

    m_priority.clear();
    for (const int desktopNumber : std::as_const(m_desktopNumbers)) {
        QStringList outputs = group.readEntry(QString::number(desktopNumber), QStringList());
        for (QString &name : outputs) {
            name = name.trimmed();
        }
        outputs.removeAll(QString());
        m_priority.insert(desktopNumber, outputs);
    }
    m_savedPriority = m_priority;
    Q_EMIT isSaveNeededChanged();
}

bool WorkspacePriorityModel::isDefaults() const
{
    for (auto it = m_priority.constBegin(); it != m_priority.constEnd(); ++it) {
        if (!it.value().isEmpty()) {
            return false;
        }
    }
    return true;
}

void WorkspacePriorityModel::defaults()
{
    for (const int desktopNumber : std::as_const(m_desktopNumbers)) {
        setPriority(desktopNumber, QStringList());
    }
}

void WorkspacePriorityModel::save()
{
    KConfigGroup group =
        KSharedConfig::openConfig(QStringLiteral("ki3rc"))->group(QStringLiteral("Workspaces"));
    for (const int desktopNumber : std::as_const(m_desktopNumbers)) {
        const QString key = QString::number(desktopNumber);
        const QStringList outputs = m_priority.value(desktopNumber);
        if (outputs.isEmpty()) {
            group.deleteEntry(key);
        } else {
            group.writeEntry(key, outputs);
        }
    }
    group.sync();
    m_savedPriority = m_priority;
    Q_EMIT isSaveNeededChanged();
}

} // namespace KWin

/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3module.h"
#include "ki3settings.h"
#include "workspaceprioritymodel.h"

#include <KPluginFactory>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QQmlEngine>

K_PLUGIN_FACTORY_WITH_JSON(Ki3ModuleFactory, "kcm_ki3.json", registerPlugin<KWin::Ki3Module>();)

namespace KWin
{

Ki3Module::Ki3Module(QObject *parent, const KPluginMetaData &metaData)
    : KQuickManagedConfigModule(parent, metaData)
    , m_settings(new Ki3Settings(this))
    , m_workspacePriority(new WorkspacePriorityModel(this))
{
    qmlRegisterAnonymousType<Ki3Settings>("org.kde.kwin.kcm.ki3", 0);

    setButtons(Apply | Default);

    connect(m_workspacePriority, &WorkspacePriorityModel::isSaveNeededChanged, this, &Ki3Module::settingsChanged);
}

Ki3Settings *Ki3Module::ki3Settings() const
{
    return m_settings;
}

WorkspacePriorityModel *Ki3Module::workspacePriorityModel() const
{
    return m_workspacePriority;
}

void Ki3Module::load()
{
    KQuickManagedConfigModule::load();
    m_workspacePriority->load();
}

void Ki3Module::save()
{
    KQuickManagedConfigModule::save();
    m_workspacePriority->save();

    // Apply everything live if a ki3 session is currently running -- see
    // ki3-pager/ki3pagerbackend.cpp for the same D-Bus object this talks to.
    // A plain method call (not the "/KWin" org.kde.KWin reloadConfig broadcast
    // signal KWin's own kwinrc-backed KCMs use, e.g. virtualdesktops.cpp)
    // since ki3rc isn't something Workspace::slotReloadConfig() -- which only
    // reparses kwinrc -- knows anything about.
    QDBusInterface iface(QStringLiteral("org.kde.KWin"), QStringLiteral("/Ki3"),
                         QStringLiteral("org.kde.ki3"), QDBusConnection::sessionBus());
    if (iface.isValid()) {
        iface.call(QDBus::NoBlock, QStringLiteral("reloadConfig"));
    }
}

void Ki3Module::defaults()
{
    KQuickManagedConfigModule::defaults();
    m_workspacePriority->defaults();
}

bool Ki3Module::isDefaults() const
{
    // The kcfg-backed settings' own defaults state is tracked automatically
    // by the base class (see KQuickManagedConfigModule's class doc comment);
    // this only needs to report the state it can't see: the workspace
    // priority model, which isn't a KCoreConfigSkeleton object.
    return m_workspacePriority->isDefaults();
}

bool Ki3Module::isSaveNeeded() const
{
    return m_workspacePriority->isSaveNeeded();
}

} // namespace KWin

#include "ki3module.moc"

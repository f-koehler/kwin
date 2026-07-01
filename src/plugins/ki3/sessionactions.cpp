/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "sessionactions.h"
#include "ki3_logging.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>

namespace KWin
{
namespace SessionActions
{

static void call(const QDBusConnection &bus, const QString &service, const QString &path,
                  const QString &interface, const QString &method)
{
    QDBusInterface iface(service, path, interface, bus);
    const QDBusMessage reply = iface.call(method);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qCWarning(KWIN_KI3) << "session action" << service << method << "failed:" << reply.errorMessage();
    }
}

void lock()
{
    call(QDBusConnection::sessionBus(), QStringLiteral("org.freedesktop.ScreenSaver"),
         QStringLiteral("/ScreenSaver"), QStringLiteral("org.freedesktop.ScreenSaver"),
         QStringLiteral("Lock"));
}

void logout()
{
    call(QDBusConnection::sessionBus(), QStringLiteral("org.kde.Shutdown"),
         QStringLiteral("/Shutdown"), QStringLiteral("org.kde.Shutdown"),
         QStringLiteral("logout"));
}

void restart()
{
    call(QDBusConnection::sessionBus(), QStringLiteral("org.kde.Shutdown"),
         QStringLiteral("/Shutdown"), QStringLiteral("org.kde.Shutdown"),
         QStringLiteral("logoutAndReboot"));
}

void shutdown()
{
    call(QDBusConnection::sessionBus(), QStringLiteral("org.kde.Shutdown"),
         QStringLiteral("/Shutdown"), QStringLiteral("org.kde.Shutdown"),
         QStringLiteral("logoutAndShutdown"));
}

void suspend()
{
    QDBusInterface iface(QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
                          QStringLiteral("org.freedesktop.login1.Manager"), QDBusConnection::systemBus());
    const QDBusMessage reply = iface.call(QStringLiteral("Suspend"), true);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qCWarning(KWIN_KI3) << "session action suspend failed:" << reply.errorMessage();
    }
}

}
}

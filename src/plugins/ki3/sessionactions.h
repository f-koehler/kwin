/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

namespace KWin
{
namespace SessionActions
{

/** Lock the screen (org.freedesktop.ScreenSaver, owned by kwin_wayland itself). */
void lock();

/** End the session cleanly, saving state (org.kde.Shutdown.logout()). */
void logout();

/** End the session and reboot (org.kde.Shutdown.logoutAndReboot()). */
void restart();

/** End the session and power off (org.kde.Shutdown.logoutAndShutdown()). */
void shutdown();

/** Suspend to RAM via logind directly -- there is no "logout" involved. */
void suspend();

}
}

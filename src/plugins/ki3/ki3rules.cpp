/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3rules.h"

#include "window.h"

#include <KConfigGroup>
#include <KSharedConfig>

namespace KWin
{

WindowRule::WindowRule(Field field, const QString &glob)
    : m_field(field)
    , m_pattern(QRegularExpression::fromWildcard(glob, Qt::CaseInsensitive))
{
}

WindowRule WindowRule::matchClass(const QString &glob)
{
    return WindowRule(Field::Class, glob);
}

WindowRule WindowRule::matchTitle(const QString &glob)
{
    return WindowRule(Field::Title, glob);
}

bool WindowRule::matches(const Window *window) const
{
    if (!window || !m_pattern.isValid()) {
        return false;
    }
    switch (m_field) {
    case Field::Class:
        // WM_CLASS carries two strings (instance + class); match either.
        return m_pattern.match(window->resourceClass()).hasMatch()
            || m_pattern.match(window->resourceName()).hasMatch();
    case Field::Title:
        return m_pattern.match(window->caption()).hasMatch();
    }
    return false;
}

QList<WindowRule> builtinIgnoredRules()
{
    return {
        // xwaylandvideobridge autostarts in a full Plasma session and maps an
        // invisible, focusable, resizable XWayland window. Untreated, ki3 hands
        // it a tile and real windows tile into only part of the screen. It sets
        // no skip-taskbar/no-focus hints (the usual cure is a KWin rule by
        // class), so the reliable signal is the window class. Ignored outright
        // rather than made to float: it's invisible on screen, and floating
        // would give it a visible title bar it was never meant to have.
        WindowRule::matchClass(QStringLiteral("xwaylandvideobridge")),
        // ../../../ki3-toggle-tiling/ is a quick-launch popup (pick a window,
        // toggle a rule for it) -- a perfectly ordinary top-level window as
        // far as KWin is concerned, so without this ki3 would tile *it* into
        // whatever layout is currently focused instead of leaving it as an
        // obviously-visible dialog with its own native decoration. App ID set
        // explicitly via QGuiApplication::setDesktopFileName() in that
        // project's main.cpp, matching its .desktop file's basename -- see
        // the 2026-09-19 ki3-PLAN.md entry (this shipped broken: the popup
        // silently got tiled into a sliver next to whatever launched it,
        // looking like the whole app did nothing).
        WindowRule::matchClass(QStringLiteral("org.kde.ki3.toggle-tiling")),
    };
}

QList<WindowRule> loadFloatingRules()
{
    QList<WindowRule> rules;

    const KConfigGroup group =
        KSharedConfig::openConfig(QStringLiteral("ki3rc"))->group(QStringLiteral("General"));

    const auto appendRules = [&rules](const QStringList &globs, auto factory) {
        for (const QString &glob : globs) {
            const QString trimmed = glob.trimmed();
            if (!trimmed.isEmpty()) {
                rules.append(factory(trimmed));
            }
        }
    };
    appendRules(group.readEntry("FloatingClasses", QStringList()), &WindowRule::matchClass);
    appendRules(group.readEntry("FloatingTitles", QStringList()), &WindowRule::matchTitle);

    return rules;
}

} // namespace KWin

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

WindowRule::WindowRule(Field field, const QString &glob, const QString &secondGlob)
    : m_field(field)
    , m_pattern(QRegularExpression::fromWildcard(glob, Qt::CaseInsensitive))
    , m_titlePattern(field == Field::ClassAndTitle
                         ? QRegularExpression::fromWildcard(secondGlob, Qt::CaseInsensitive)
                         : QRegularExpression())
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

WindowRule WindowRule::matchClassAndTitle(const QString &classGlob, const QString &titleGlob)
{
    return WindowRule(Field::ClassAndTitle, classGlob, titleGlob);
}

bool WindowRule::matches(const Window *window) const
{
    if (!window || !m_pattern.isValid()) {
        return false;
    }
    // WM_CLASS carries two strings (instance + class); match either.
    const bool classMatches = m_pattern.match(window->resourceClass()).hasMatch()
        || m_pattern.match(window->resourceName()).hasMatch();
    switch (m_field) {
    case Field::Class:
        return classMatches;
    case Field::Title:
        return m_pattern.match(window->caption()).hasMatch();
    case Field::ClassAndTitle:
        return classMatches && m_titlePattern.isValid() && m_titlePattern.match(window->caption()).hasMatch();
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

    // Combined class+title rules: both must match the *same* window, unlike
    // the independent OR'd lists above -- e.g. a title glob generic enough
    // to also match unrelated windows ("*Settings*") can be pinned to one
    // specific application's class instead of floating every window with a
    // matching title regardless of what app it belongs to. Each entry is
    // "classGlob\ttitleGlob" -- '\t' as an internal field separator that
    // won't collide with real class/title text, the same trick
    // kglobalshortcutsrc itself uses to pack multiple fields into one
    // QStringList entry. Not yet exposed in the kcm_ki3/ki3-toggle-tiling
    // rule editors (hand-edit ki3rc for now) -- see ki3-PLAN.md.
    for (const QString &pair : group.readEntry("FloatingClassAndTitlePairs", QStringList())) {
        const QStringList parts = pair.split(QLatin1Char('\t'));
        if (parts.size() != 2) {
            continue; // malformed entry (e.g. hand-edited without the separator)
        }
        const QString classGlob = parts[0].trimmed();
        const QString titleGlob = parts[1].trimmed();
        if (!classGlob.isEmpty() && !titleGlob.isEmpty()) {
            rules.append(WindowRule::matchClassAndTitle(classGlob, titleGlob));
        }
    }

    return rules;
}

} // namespace KWin

/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>

namespace KWin
{

class Window;

/**
 * A single rule that matches a window by one of its attributes. Used to classify
 * windows ki3 should treat specially — currently to mark them non-tileable.
 *
 * A rule matches one field against a case-insensitive glob pattern
 * (e.g. "xwaylandvideobridge", "*Picture-in-Picture*"). Extending the matcher to
 * a new attribute is a matter of adding a Field value, a factory, and a case in
 * matches().
 */
class WindowRule
{
public:
    /** Match the window's WM class / wayland app-id (resourceClass or resourceName). */
    static WindowRule matchClass(const QString &glob);
    /** Match the window's caption (title). */
    static WindowRule matchTitle(const QString &glob);

    /** Whether @p window satisfies this rule. */
    bool matches(const Window *window) const;

private:
    enum class Field {
        Class,
        Title,
    };

    WindowRule(Field field, const QString &glob);

    Field m_field;
    QRegularExpression m_pattern;
};

/** Built-in non-tileable rules shipped with ki3 (e.g. xwaylandvideobridge). */
QList<WindowRule> builtinNonTileableRules();

/**
 * Built-in rules plus user-declared ones from `ki3rc` ([General] keys
 * NonTileableClasses / NonTileableTitles, comma-separated globs). Respects
 * XDG_CONFIG_HOME, so in a ki3 session it reads ~/.config-ki3/ki3rc.
 */
QList<WindowRule> loadNonTileableRules();

} // namespace KWin

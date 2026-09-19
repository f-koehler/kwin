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
 * windows ki3 should treat specially — currently either to always float them
 * (user-configurable, see loadFloatingRules()) or to never touch them at all
 * (built-in only, see builtinIgnoredRules()).
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

/**
 * Built-in rules for windows ki3 must never touch at all -- not tiled, not
 * floating, no chrome, exactly as if ki3 didn't exist. Deliberately separate
 * from the user-facing floating rules (loadFloatingRules()): these are
 * technical workarounds for windows that would look wrong with ki3's own
 * floating title bar (e.g. xwaylandvideobridge's normally-invisible helper
 * window), not real "this window should float" preferences, so they're not
 * exposed in ki3rc/the KCM and never merged into the floating-rules list.
 */
QList<WindowRule> builtinIgnoredRules();

/**
 * User-declared floating rules from `ki3rc` ([General] keys
 * FloatingClasses / FloatingTitles, comma-separated globs) -- windows i3/sway
 * style: never tiled, always a real floating window with ki3's own chrome,
 * exactly as if the user had toggled floating on it by hand. Does *not*
 * include the built-in ignore rules -- see builtinIgnoredRules() -- those are
 * a fully separate mechanism. Respects XDG_CONFIG_HOME, so in a ki3 session
 * it reads ~/.config-ki3/ki3rc.
 */
QList<WindowRule> loadFloatingRules();

} // namespace KWin

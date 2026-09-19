/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ki3settings.h"
#include "workspaceprioritymodel.h"

#include <KQuickManagedConfigModule>

namespace KWin
{

/**
 * System Settings module for ki3 (see ../../plugins/ki3/): non-tileable
 * window rules, tile gap, border thickness (all plain `ki3rc [General]`
 * KConfigXT entries -- see ki3settings.kcfg) and the per-desktop output-
 * priority list (`ki3rc [Workspaces]`, dynamic keys -- handled separately by
 * WorkspacePriorityModel, see its class doc comment for why).
 *
 * Deliberately has no dependency on the ki3 plugin itself or on internal KWin
 * headers -- like every other KWin KCM, it only edits config files and talks
 * to a *running* ki3 over D-Bus (see reloadConfig() below) to apply changes
 * live; it works equally well with no ki3 session running at all (editing
 * for next time).
 */
class Ki3Module : public KQuickManagedConfigModule
{
    Q_OBJECT

    Q_PROPERTY(Ki3Settings *ki3Settings READ ki3Settings CONSTANT)
    Q_PROPERTY(KWin::WorkspacePriorityModel *workspacePriorityModel READ workspacePriorityModel CONSTANT)

public:
    explicit Ki3Module(QObject *parent, const KPluginMetaData &metaData);

    Ki3Settings *ki3Settings() const;
    WorkspacePriorityModel *workspacePriorityModel() const;

    bool isDefaults() const override;
    bool isSaveNeeded() const override;

public Q_SLOTS:
    void load() override;
    void save() override;
    void defaults() override;

private:
    Ki3Settings *m_settings;
    WorkspacePriorityModel *m_workspacePriority;
};

} // namespace KWin

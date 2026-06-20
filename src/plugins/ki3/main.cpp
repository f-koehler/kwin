/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ki3tiler.h"

#include <KPluginFactory>

namespace KWin
{

class KWIN_EXPORT Ki3TilerFactory : public PluginFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PluginFactory_iid FILE "metadata.json")
    Q_INTERFACES(KWin::PluginFactory)

public:
    explicit Ki3TilerFactory() = default;

    std::unique_ptr<Plugin> create() const override;
};

std::unique_ptr<Plugin> Ki3TilerFactory::create() const
{
    return std::make_unique<Ki3Tiler>();
}

} // namespace KWin

#include "main.moc"

# SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>
# SPDX-License-Identifier: GPL-2.0-or-later

kcoreaddons_add_plugin(ki3 INSTALL_NAMESPACE "kwin/plugins")

ecm_qt_declare_logging_category(ki3
    HEADER ki3_logging.h
    IDENTIFIER KWIN_KI3
    CATEGORY_NAME kwin_ki3
    DEFAULT_SEVERITY Debug
)

target_sources(ki3 PRIVATE
    main.cpp
    ki3rules.cpp
    ki3tiler.cpp
    sessionactions.cpp
    sessionosd.cpp
)

target_link_libraries(ki3 PRIVATE
    kwin

    KF6::ConfigCore
    KF6::I18n
    KF6::GlobalAccel

    Qt6::DBus
    Qt6::Widgets
)

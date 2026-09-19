# SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>
# SPDX-License-Identifier: GPL-2.0-or-later

set(kcm_ki3_PART_SRCS
    ki3module.cpp
    workspaceprioritymodel.cpp
)

kconfig_add_kcfg_files(kcm_ki3_PART_SRCS ki3settings.kcfgc GENERATE_MOC)

kcmutils_add_qml_kcm(kcm_ki3 SOURCES ${kcm_ki3_PART_SRCS})

target_link_libraries(kcm_ki3 PRIVATE
    Qt6::DBus
    Qt6::Qml

    KF6::ConfigCore
    KF6::I18n
    KF6::KCMUtils
    KF6::KCMUtilsQuick
)

install(FILES ki3settings.kcfg DESTINATION ${KDE_INSTALL_KCFGDIR})

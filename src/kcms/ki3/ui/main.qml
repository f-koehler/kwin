/*
    SPDX-FileCopyrightText: 2026 Fabian Koehler <fabian@fkoehler.me>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

KCM.SimpleKCM {
    id: root

    implicitWidth: Kirigami.Units.gridUnit * 35
    implicitHeight: Kirigami.Units.gridUnit * 30

    // A single string list (ki3Settings.nonTileableClasses/nonTileableTitles)
    // edited as "chips" with a text field + add button. No drag/reorder --
    // membership, not order, is all that matters for these two lists. A
    // plain Repeater rather than a ListView -- the per-item "modelData"
    // binding for a bare QStringList model turned out unreliable specifically
    // through ListView's QQmlDelegateModel (see the 2026-09-19 ki3-PLAN.md
    // entry); Repeater's own per-item property injection for the exact same
    // kind of model is what the workspace-priority editor below already
    // relies on successfully.
    component RuleListEditor: ColumnLayout {
        id: editor

        property var model: []
        property string placeholderText
        signal added(string value)
        signal removed(string value)

        RowLayout {
            Layout.fillWidth: true

            QQC2.TextField {
                id: newValueField
                Layout.fillWidth: true
                placeholderText: editor.placeholderText
                onAccepted: addButton.clicked()
            }
            QQC2.Button {
                id: addButton
                icon.name: "list-add"
                text: i18nc("@action:button", "Add")
                enabled: newValueField.text.trim().length > 0
                onClicked: {
                    const value = newValueField.text.trim();
                    // kcfg StringList entries aren't a set -- nothing dedupes
                    // them on the C++ side, so an exact duplicate would
                    // otherwise show up twice (and get saved twice) with no
                    // feedback that anything happened at all.
                    if (!editor.model.includes(value)) {
                        editor.added(value);
                    }
                    newValueField.clear();
                    newValueField.forceActiveFocus();
                }
            }
        }

        Repeater {
            model: editor.model

            delegate: Kirigami.TitleSubtitleWithActions {
                id: ruleDelegate
                required property string modelData

                Layout.fillWidth: true
                title: ruleDelegate.modelData
                displayHint: QQC2.Button.IconOnly
                actions: [
                    Kirigami.Action {
                        icon.name: "edit-delete-remove-symbolic"
                        text: i18nc("@action:button", "Remove")
                        onTriggered: editor.removed(ruleDelegate.modelData)
                    }
                ]
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.largeSpacing

        Kirigami.FormLayout {
            Layout.fillWidth: true

            QQC2.SpinBox {
                id: gapSpinBox
                Kirigami.FormData.label: i18n("Extra gap between tiles:")
                from: 0
                to: 64
                value: kcm.ki3Settings.gap
                onValueModified: kcm.ki3Settings.gap = value
                textFromValue: (value, locale) => i18n("%1 px", value)
                valueFromText: (text, locale) => parseInt(text)

                KCM.SettingStateBinding {
                    configObject: kcm.ki3Settings
                    settingName: "gap"
                }
            }

            QQC2.Label {
                Kirigami.FormData.label: ""
                text: i18nc("@info", "On top of the space the border itself needs (2 x border thickness)")
                opacity: 0.7
                font: Kirigami.Theme.smallFont
            }

            QQC2.SpinBox {
                id: outerGapSpinBox
                Kirigami.FormData.label: i18n("Extra gap around the screen edge:")
                from: 0
                to: 64
                value: kcm.ki3Settings.outerGap
                onValueModified: kcm.ki3Settings.outerGap = value
                textFromValue: (value, locale) => i18n("%1 px", value)
                valueFromText: (text, locale) => parseInt(text)

                KCM.SettingStateBinding {
                    configObject: kcm.ki3Settings
                    settingName: "outerGap"
                }
            }

            QQC2.Label {
                Kirigami.FormData.label: ""
                text: i18nc("@info", "On top of the space the border itself needs (1 x border thickness)")
                opacity: 0.7
                font: Kirigami.Theme.smallFont
            }

            QQC2.SpinBox {
                id: borderThicknessSpinBox
                Kirigami.FormData.label: i18n("Border thickness:")
                from: 1
                to: 16
                value: kcm.ki3Settings.borderThickness
                onValueModified: kcm.ki3Settings.borderThickness = value
                textFromValue: (value, locale) => i18n("%1 px", value)
                valueFromText: (text, locale) => parseInt(text)

                KCM.SettingStateBinding {
                    configObject: kcm.ki3Settings
                    settingName: "borderThickness"
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true

            Kirigami.Heading {
                Kirigami.FormData.isSection: true
                level: 2
                text: i18n("Windows that should never be tiled")
            }

            RuleListEditor {
                Kirigami.FormData.label: i18n("Window class:")
                Layout.fillWidth: true
                model: kcm.ki3Settings.nonTileableClasses
                placeholderText: i18nc("@info:placeholder", "e.g. org.kde.kcalc")
                onAdded: (value) => {
                    let list = kcm.ki3Settings.nonTileableClasses;
                    list.push(value);
                    kcm.ki3Settings.nonTileableClasses = list;
                }
                onRemoved: (value) => {
                    kcm.ki3Settings.nonTileableClasses =
                        kcm.ki3Settings.nonTileableClasses.filter(v => v !== value);
                }
            }

            RuleListEditor {
                Kirigami.FormData.label: i18n("Window title:")
                Layout.fillWidth: true
                model: kcm.ki3Settings.nonTileableTitles
                placeholderText: i18nc("@info:placeholder", "e.g. Picture-in-Picture")
                onAdded: (value) => {
                    let list = kcm.ki3Settings.nonTileableTitles;
                    list.push(value);
                    kcm.ki3Settings.nonTileableTitles = list;
                }
                onRemoved: (value) => {
                    kcm.ki3Settings.nonTileableTitles =
                        kcm.ki3Settings.nonTileableTitles.filter(v => v !== value);
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        ColumnLayout {
            Layout.fillWidth: true

            Kirigami.Heading {
                level: 2
                text: i18n("Per-desktop output priority")
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                type: Kirigami.MessageType.Information
                visible: !kcm.workspacePriorityModel.available
                text: i18n("ki3 isn't currently running, so connected screens can't be listed. Existing entries below can still be reordered or removed; start a ki3 session to add new ones.")
            }

            Repeater {
                id: workspaceRepeater
                model: kcm.workspacePriorityModel

                delegate: Kirigami.FormLayout {
                    id: workspaceDelegate
                    required property int desktopNumber
                    required property var outputPriority

                    Layout.fillWidth: true

                    RowLayout {
                        Kirigami.FormData.label: i18n("Workspace %1:", workspaceDelegate.desktopNumber)
                        Layout.fillWidth: true

                        Repeater {
                            model: workspaceDelegate.outputPriority
                            delegate: QQC2.Button {
                                id: outputChip
                                required property string modelData
                                required property int index

                                text: outputChip.modelData
                                icon.name: "preferences-desktop-display"
                                display: QQC2.AbstractButton.TextBesideIcon

                                QQC2.ToolTip.visible: hovered
                                QQC2.ToolTip.text: i18nc("@info:tooltip", "Priority %1 -- click to remove", outputChip.index + 1)

                                onClicked: kcm.workspacePriorityModel.removeOutput(workspaceDelegate.desktopNumber, outputChip.modelData)
                            }
                        }

                        QQC2.ComboBox {
                            id: addOutputCombo
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 8
                            enabled: kcm.workspacePriorityModel.available
                            model: kcm.workspacePriorityModel.availableOutputsForDesktop(workspaceDelegate.desktopNumber)
                            displayText: currentIndex === -1 ? i18nc("@info:placeholder", "Add output…") : currentText
                        }
                        QQC2.Button {
                            icon.name: "list-add"
                            enabled: addOutputCombo.currentIndex !== -1
                            onClicked: {
                                kcm.workspacePriorityModel.addOutput(workspaceDelegate.desktopNumber, addOutputCombo.currentText);
                                addOutputCombo.currentIndex = -1;
                            }
                        }
                    }
                }
            }
        }
    }
}

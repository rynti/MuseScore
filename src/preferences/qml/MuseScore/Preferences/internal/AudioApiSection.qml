/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
import QtQuick
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

BaseSection {
    id: root

    title: qsTrc("preferences", "Audio")

    property int currentAudioApiIndex: -1
    property var audioApiList: null
    property bool audioApiSelectionEnabled: true
    property bool jackWorkerRpcWarningVisible: false
    property string jackWorkModeName: ""

    signal currentAudioApiIndexChangeRequested(int newIndex)
    signal useWorkerRpcAndRestartRequested()

    Row {
        spacing: 8

        ComboBoxWithTitle {
            id: apiComboBox

            title: qsTrc("preferences", "Audio driver")
            columnWidth: root.columnWidth

            visible: root.audioApiList.length > 1
            enabled: root.audioApiSelectionEnabled

            currentIndex: root.currentAudioApiIndex
            model: root.audioApiList

            navigation.name: "AudioApiBox"
            navigation.panel: root.navigation
            navigation.row: 1

            onValueEdited: function(newIndex, newValue) {
                root.currentAudioApiIndexChangeRequested(newIndex)
            }
        }
    }

    Rectangle {
        id: workerRpcWarning

        readonly property color warningColor: "#D89400"

        visible: root.jackWorkerRpcWarningVisible
        width: parent.width
        height: warningContent.implicitHeight + 24

        color: Utils.colorWithAlpha(warningColor, 0.12)
        border.color: warningColor
        border.width: 1
        radius: 4

        ColumnLayout {
            id: warningContent

            anchors.fill: parent
            anchors.margins: 12
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                StyledIconLabel {
                    Layout.alignment: Qt.AlignTop

                    iconCode: IconCode.WARNING
                    color: workerRpcWarning.warningColor
                }

                StyledTextLabel {
                    Layout.fillWidth: true

                    horizontalAlignment: Text.AlignLeft
                    wrapMode: Text.WordWrap

                    text: qsTrc(
                              "preferences",
                              "JACK is currently running in %1. JACK remains available, but latency-correct recording is not guaranteed. Worker RPC mode is recommended.")
                          .arg(root.jackWorkModeName)
                }
            }

            FlatButton {
                Layout.alignment: Qt.AlignLeft

                text: qsTrc("preferences", "Switch to Worker RPC and restart")
                accentButton: true
                isNarrow: true

                navigation.name: "UseWorkerRpcButton"
                navigation.panel: root.navigation
                navigation.row: 2

                onClicked: {
                    root.useWorkerRpcAndRestartRequested()
                }
            }
        }
    }

    CommonAudioApiConfiguration {
        columnWidth: root.columnWidth

        navigation: root.navigation
        navigationOrderStart: 3
    }
}

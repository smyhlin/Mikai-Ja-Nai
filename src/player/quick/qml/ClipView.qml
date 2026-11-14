pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs as NativeDialogs
import Mikai.Player

Item {
    id: root
    objectName: "clipView"

    required property PlayerController controller
    required property ClipController editor

    width: 1060
    height: 820
    focus: true
    readonly property color textColor: card.textColor
    readonly property color surfaceColor: controller.darkTheme ? Qt.lighter(card.cardColor, 1.4)
                                                               : Qt.darker(card.cardColor, 1.04)
    readonly property color borderColor: card.edgeColor
    readonly property real controlRadius: Math.max(0, controller.cornerRadius)
    readonly property real gap: 8
    readonly property real sectionGap: 12

    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("Cut moment")

    Keys.onPressed: event => {
        if (replaceDialog.visible || editor.choosingOutput) return
        if (event.key === Qt.Key_Escape) editor.requestClose()
        else if (event.key === Qt.Key_Space) editor.togglePlay()
        else if (event.key === Qt.Key_M) editor.toggleMute()
        else if (event.key === Qt.Key_BracketLeft) editor.setStartToCurrent()
        else if (event.key === Qt.Key_BracketRight) editor.setEndToCurrent()
        else if (event.key === Qt.Key_Left)
            editor.stepSeconds((event.modifiers & Qt.ShiftModifier) ? -0.1 : -1)
        else if (event.key === Qt.Key_Right)
            editor.stepSeconds((event.modifiers & Qt.ShiftModifier) ? 0.1 : 1)
        else return
        event.accepted = true
    }
    Component.onCompleted: forceActiveFocus()

    PlayerPanel {
        id: card
        anchors.fill: parent
        controller: root.controller
        opaqueSurface: true
        padding: 16

        ColumnLayout {
            anchors.fill: parent
            spacing: root.gap

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 38
                spacing: root.sectionGap
                PlayerIcon {
                    controller: root.controller
                    name: "clapperboard"
                    tint: root.textColor
                    sourceSize: 22
                }
                Text {
                    text: qsTr("Cut moment")
                    color: root.textColor
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                    textFormat: Text.PlainText
                }
                Text {
                    Layout.fillWidth: true
                    text: root.editor.caption
                    color: root.textColor
                    opacity: 0.6
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                    textFormat: Text.PlainText
                }
            }

            Rectangle {
                id: preview
                objectName: "clipPreviewCard"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 180
                color: Qt.rgba(0, 0, 0, 1)
                border.width: 1
                border.color: root.borderColor
                radius: root.controlRadius
                clip: true

                MpvVideoItem {
                    id: video
                    objectName: "clipPreviewVideo"
                    anchors.fill: parent
                    anchors.margins: 1
                    session: root.editor.session
                    onVideoFramePresented: root.editor.notePresentedFrame()
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    color: preview.color
                    visible: !root.editor.videoVisible
                    Image {
                        id: still
                        objectName: "clipPreviewStill"
                        anchors.fill: parent
                        source: root.editor.previewSource
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                        smooth: true
                    }
                }
                BusyIndicator {
                    anchors.centerIn: parent
                    width: 36
                    height: 36
                    running: root.editor.previewLoading
                    visible: running
                    palette.highlight: root.controller.accentColor
                    Accessible.name: qsTr("Loading preview")
                }
                Text {
                    anchors.centerIn: parent
                    text: qsTr("Preview unavailable")
                    color: root.textColor
                    visible: !root.editor.previewLoading && !root.editor.videoVisible
                             && still.source.toString().length === 0
                    font.pixelSize: 13
                }
                TapHandler {
                    enabled: root.editor.editingEnabled
                    acceptedButtons: Qt.LeftButton
                    onTapped: {
                        root.editor.togglePlay()
                        root.forceActiveFocus()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                spacing: root.gap
                Text {
                    Layout.preferredWidth: 110
                    text: root.editor.positionText
                    color: root.textColor
                    font.pixelSize: 14
                    font.features: { "tnum": 1 }
                    textFormat: Text.PlainText
                    Accessible.name: qsTr("Preview position: %1").arg(text)
                }
                Item { Layout.fillWidth: true }
                PlayerButton {
                    controller: root.controller
                    iconName: "chevron-left"
                    toolTip: qsTr("Back one second (Left); Shift for 0.1 seconds")
                    enabled: root.editor.editingEnabled
                    onClicked: { root.editor.stepSeconds(-1); root.forceActiveFocus() }
                }
                PlayerButton {
                    id: previewPlayButton
                    objectName: "clipPlay"
                    controller: root.controller
                    iconName: root.editor.playing ? "pause" : "play"
                    toolTip: qsTr("Play / pause preview (Space)")
                    enabled: root.editor.editingEnabled
                    implicitWidth: 44
                    implicitHeight: 44
                    onClicked: { root.editor.togglePlay(); root.forceActiveFocus() }
                    background: Rectangle {
                        color: root.surfaceColor
                        border.color: previewPlayButton.visualFocus ? root.controller.accentColor : root.borderColor
                        border.width: 1
                        radius: root.controlRadius
                        antialiasing: true
                    }
                }
                PlayerButton {
                    controller: root.controller
                    iconName: "chevron-right"
                    toolTip: qsTr("Forward one second (Right); Shift for 0.1 seconds")
                    enabled: root.editor.editingEnabled
                    onClicked: { root.editor.stepSeconds(1); root.forceActiveFocus() }
                }
                Item { Layout.fillWidth: true }
                PlayerButton {
                    objectName: "clipMute"
                    controller: root.controller
                    iconName: root.editor.muted ? "mute" : "volume"
                    toolTip: root.editor.muted ? qsTr("Unmute preview (M)") : qsTr("Mute preview (M)")
                    enabled: root.editor.editingEnabled
                    onClicked: { root.editor.toggleMute(); root.forceActiveFocus() }
                }
                Text {
                    Layout.preferredWidth: 64
                    text: qsTr("Loop preview")
                    color: root.textColor
                    opacity: 0.6
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }
            }

            ClipTimeline {
                objectName: "clipTimeline"
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                controller: root.controller
                editor: root.editor
                textColor: root.textColor
                surfaceColor: root.surfaceColor
                borderColor: root.borderColor
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                spacing: root.gap
                PlayerButton {
                    objectName: "clipSetIn"
                    controller: root.controller
                    iconName: "chevron-right"
                    text: root.editor.startText
                    toolTip: qsTr("Set start to current playhead ( [ )")
                    enabled: root.editor.editingEnabled
                    onClicked: { root.editor.setStartToCurrent(); root.forceActiveFocus() }
                }
                Text { text: "–"; color: root.textColor; opacity: 0.4 }
                PlayerButton {
                    objectName: "clipSetOut"
                    controller: root.controller
                    iconName: "chevron-left"
                    text: root.editor.endText
                    toolTip: qsTr("Set end to current playhead ( ] )")
                    enabled: root.editor.editingEnabled
                    onClicked: { root.editor.setEndToCurrent(); root.forceActiveFocus() }
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("%1 s / 60").arg(Number(root.editor.selectionDuration).toLocaleString(Qt.locale(), 'f', 1))
                    color: root.textColor
                    font.pixelSize: 12
                    font.features: { "tnum": 1 }
                    horizontalAlignment: Text.AlignHCenter
                }
                Repeater {
                    model: [5, 10, 15, 30, 60]
                    delegate: PlayerButton {
                        required property int modelData
                        objectName: "clipPreset" + modelData
                        controller: root.controller
                        text: qsTr("%1s").arg(modelData)
                        toolTip: qsTr("Set clip length to %1 seconds").arg(modelData)
                        checked: Math.abs(root.editor.selectionDuration - modelData) < 0.05
                        enabled: root.editor.editingEnabled
                        compact: true
                        Layout.preferredWidth: 44
                        Layout.minimumHeight: 34
                        onClicked: { root.editor.applyDurationPreset(modelData); root.forceActiveFocus() }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: root.borderColor
                opacity: 0.5
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                spacing: root.gap
                Text {
                    text: qsTr("MP4")
                    color: root.textColor
                    opacity: 0.6
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
                RowLayout {
                    spacing: 0
                    Repeater {
                        model: [1080, 720, 480]
                        delegate: PlayerButton {
                            required property int modelData
                            objectName: "clipQuality" + modelData
                            controller: root.controller
                            text: qsTr("%1p").arg(modelData)
                            checked: root.editor.qualityHeight === modelData
                            enabled: root.editor.editingEnabled
                            compact: true
                            Layout.minimumHeight: 34
                            onClicked: { root.editor.qualityHeight = modelData; root.forceActiveFocus() }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: root.editor.estimatedSize
                    color: root.textColor
                    opacity: 0.6
                    font.pixelSize: 12
                    font.features: { "tnum": 1 }
                    textFormat: Text.PlainText
                }
                PlayerButton {
                    objectName: "clipCancelExport"
                    controller: root.controller
                    text: qsTr("Cancel")
                    iconName: "close"
                    visible: root.editor.busy
                    enabled: !root.editor.cancelPending
                    onClicked: root.editor.cancelExport()
                }
                PlayerButton {
                    id: exportButton
                    objectName: "clipExport"
                    controller: root.controller
                    text: root.editor.busy ? qsTr("Encoding… %1%").arg(Math.round(root.editor.progress * 100))
                        : root.editor.failed ? qsTr("Try again")
                        : root.editor.saved ? qsTr("Export another") : qsTr("Export MP4")
                    enabled: root.editor.canExport
                    implicitWidth: 172
                    implicitHeight: 42
                    onClicked: root.editor.requestExport()
                    background: Rectangle {
                        color: exportButton.enabled ? root.controller.accentColor : root.surfaceColor
                        radius: root.controlRadius
                        border.width: exportButton.visualFocus ? 2 : 0
                        border.color: root.textColor
                        antialiasing: true
                    }
                    contentItem: RowLayout {
                        spacing: root.gap
                        Item { Layout.fillWidth: true }
                        PlayerIcon {
                            controller: root.controller
                            name: "download"
                            sourceSize: 18
                            tint: exportButton.enabled ? root.editor.accentInk : root.textColor
                        }
                        Text {
                            text: exportButton.text
                            color: exportButton.enabled ? root.editor.accentInk : root.textColor
                            opacity: exportButton.enabled ? 1 : 0.55
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            textFormat: Text.PlainText
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 4
                Rectangle {
                    anchors.fill: parent
                    radius: 2
                    color: root.surfaceColor
                    visible: root.editor.busy
                }
                Rectangle {
                    width: parent.width * root.editor.progress
                    height: parent.height
                    radius: 2
                    color: root.controller.accentColor
                    visible: root.editor.busy
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                spacing: root.gap
                Text {
                    id: status
                    objectName: "clipStatus"
                    Layout.fillWidth: true
                    text: root.editor.statusText
                    color: root.textColor
                    opacity: root.editor.failed ? 1 : 0.65
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    Accessible.role: root.editor.failed ? Accessible.AlertMessage : Accessible.StaticText
                    Accessible.name: text
                    ToolTip.visible: statusHover.hovered && truncated
                    ToolTip.text: text
                    HoverHandler { id: statusHover }
                }
                PlayerButton {
                    controller: root.controller
                    iconName: "folder"
                    toolTip: qsTr("Show in folder")
                    visible: root.editor.saved
                    onClicked: root.editor.revealOutput()
                }
                PlayerButton {
                    controller: root.controller
                    iconName: "link"
                    toolTip: qsTr("Copy path")
                    visible: root.editor.saved
                    onClicked: root.editor.copyOutputPath()
                }
            }
        }
    }

    NativeDialogs.FileDialog {
        id: saveDialog
        objectName: "clipSaveDialog"
        title: qsTr("Save clip")
        fileMode: NativeDialogs.FileDialog.SaveFile
        nameFilters: [qsTr("MP4 video (*.mp4)")]
        defaultSuffix: "mp4"
        options: NativeDialogs.FileDialog.DontConfirmOverwrite
        onAccepted: root.editor.acceptOutput(selectedFile)
        onRejected: {
            root.editor.cancelOutput()
            root.forceActiveFocus()
        }
    }
    Dialog {
        id: replaceDialog
        objectName: "clipReplaceDialog"
        parent: root
        anchors.centerIn: parent
        width: Math.min(480, root.width - 32)
        title: qsTr("Replace existing file?")
        modal: true
        focus: true
        standardButtons: Dialog.Yes | Dialog.No
        contentItem: Text {
            text: qsTr("%1 already exists. Replace it after the new clip is complete?").arg(root.editor.overwriteFileName)
            color: root.textColor
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
        }
        onAccepted: { root.editor.confirmOverwrite(true); root.forceActiveFocus() }
        onRejected: { root.editor.confirmOverwrite(false); root.forceActiveFocus() }
    }
    Connections {
        target: root.editor
        function onOutputRequested(suggestedFile: url) {
            saveDialog.selectedFile = suggestedFile
            saveDialog.open()
        }
        function onStateChanged() {
            if (!root.editor.choosingOutput && saveDialog.visible) saveDialog.close()
            if (root.editor.overwritePending && !replaceDialog.visible) replaceDialog.open()
            else if (!root.editor.overwritePending && replaceDialog.visible) replaceDialog.close()
        }
    }
}

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Mikai.Player

// Compact controls live over the same video scene as the full player. Playback,
// menus, window operations and source lifetime remain owned by the controller.
Item {
    id: root

    required property PlayerController controller

    readonly property bool focusInside: {
        if (!root.Window.window) return false
        let node = root.Window.window.activeFocusItem
        while (node) {
            if (node === root) return true
            node = node.parent
        }
        return false
    }
    readonly property bool interactionActive: root.focusInside || dragArea.pressed
                                                || leftResize.pressed || rightResize.pressed
                                                || topResize.pressed || bottomResize.pressed
                                                || topLeftResize.pressed || topRightResize.pressed
                                                || bottomLeftResize.pressed || bottomRightResize.pressed

    signal menuRequested(string menuId, rect anchorRect)

    anchors.fill: parent
    focus: true
    activeFocusOnTab: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Picture-in-picture controls")

    readonly property color ink: controller.darkTheme ? "#f4f4f5" : "#18181b"
    readonly property color mutedInk: controller.darkTheme ? "#d4d4d8" : "#3f3f46"
    readonly property color scrim: controller.darkTheme ? Qt.rgba(9 / 255, 10 / 255, 13 / 255, 0.88)
                                                        : Qt.rgba(0.98, 0.98, 0.99, 0.93)

    function anchorRect(item) {
        const p = item.mapToItem(root.parent, 0, 0)
        return Qt.rect(p.x, p.y, item.width, item.height)
    }

    function requestMenu(item, menuId) {
        root.controller.noteUserActivity()
        root.menuRequested(menuId, root.anchorRect(item))
    }

    function beginResize(area, edges) {
        area.forceActiveFocus()
        root.controller.noteUserActivity()
        root.controller.beginWindowResize(edges)
    }

    onInteractionActiveChanged: {
        if (root.interactionActive) root.controller.noteUserActivity()
    }

    Rectangle {
        anchors.fill: parent
        radius: Math.max(4, Math.min(root.controller.cornerRadius + 4, Math.min(width, height) / 2))
        color: root.scrim
        border.width: 1
        border.color: root.controller.darkTheme ? Qt.rgba(1, 1, 1, 0.2)
                                                : Qt.rgba(0, 0, 0, 0.16)
        antialiasing: true
    }

    // Dedicated grab region avoids stealing button presses; its native move
    // request is honored by Windows/X11 and remains compositor-controlled on Wayland.
    MouseArea {
        id: dragArea
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.right: parent.right
        height: 40
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeAllCursor
        onPressed: mouse => {
            root.forceActiveFocus()
            root.controller.noteUserActivity()
            root.controller.beginWindowMove()
            mouse.accepted = true
        }
        onDoubleClicked: root.controller.leavePip()
    }

    RowLayout {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        height: 40
        spacing: 4
        z: 1

        PlayerIcon {
            controller: root.controller
            name: "pip"
            tint: root.controller.accentColor
            sourceSize: 15
        }

        Label {
            Layout.fillWidth: true
            text: root.controller.episodeLabel.length > 0
                  ? root.controller.episodeLabel : root.controller.caption
            color: root.ink
            font.pixelSize: 12
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Accessible.ignored: true
        }

        PlayerButton {
            id: restoreButton
            controller: root.controller
            compact: true
            iconName: "expand"
            toolTip: qsTr("Restore full player")
            Accessible.name: toolTip
            implicitWidth: 32
            implicitHeight: 32
            onClicked: {
                root.controller.noteUserActivity()
                root.controller.leavePip()
            }
        }

        PlayerButton {
            id: closeButton
            controller: root.controller
            compact: true
            iconName: "close"
            toolTip: qsTr("Close player")
            Accessible.name: toolTip
            implicitWidth: 32
            implicitHeight: 32
            onClicked: {
                root.controller.noteUserActivity()
                root.controller.requestClose()
            }
        }
    }

    ColumnLayout {
        id: controls
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.bottomMargin: 8
        spacing: 1

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            PlayerButton {
                controller: root.controller
                objectName: "player.pip.playPause"
                compact: true
                iconName: root.controller.playing ? "pause" : "play"
                toolTip: root.controller.playing ? qsTr("Pause (Space)") : qsTr("Play (Space)")
                Accessible.name: toolTip
                enabled: root.controller.hasMedia
                implicitWidth: 36
                implicitHeight: 36
                onClicked: {
                    root.controller.noteUserActivity()
                    root.controller.togglePlayPause()
                }
            }

            PlayerButton {
                controller: root.controller
                compact: true
                text: "−85"
                toolTip: qsTr("Seek backward 85 seconds")
                Accessible.name: toolTip
                enabled: root.controller.hasMedia
                implicitWidth: 42
                implicitHeight: 36
                onClicked: {
                    root.controller.noteUserActivity()
                    root.controller.seekBy(-85)
                }
            }

            PlayerButton {
                controller: root.controller
                compact: true
                text: "+85"
                toolTip: qsTr("Seek forward 85 seconds")
                Accessible.name: toolTip
                enabled: root.controller.hasMedia
                implicitWidth: 42
                implicitHeight: 36
                onClicked: {
                    root.controller.noteUserActivity()
                    root.controller.skipForward()
                }
            }

            VolumeControl {
                id: volumeControl
                controller: root.controller
                objectName: "player.pip.volume"
                Layout.fillWidth: true
                Layout.minimumWidth: 90
                Layout.maximumWidth: 148
                Layout.preferredWidth: 118
                Layout.alignment: Qt.AlignVCenter
            }

            PlayerButton {
                id: audioButton
                controller: root.controller
                compact: true
                iconName: "microphone"
                toolTip: qsTr("Audio tracks")
                Accessible.name: toolTip
                visible: root.controller.localMedia || root.controller.audioTracksAvailable
                implicitWidth: 34
                implicitHeight: 36
                onClicked: root.requestMenu(audioButton, "audio")
            }

            PlayerButton {
                id: subtitlesButton
                controller: root.controller
                compact: true
                iconName: "captions"
                toolTip: qsTr("Subtitle tracks")
                Accessible.name: toolTip
                visible: root.controller.localMedia || root.controller.subtitleTracksAvailable
                implicitWidth: 34
                implicitHeight: 36
                onClicked: root.requestMenu(subtitlesButton, "subtitles")
            }

            PlayerButton {
                id: moreButton
                controller: root.controller
                objectName: "player.pip.settings.button"
                compact: true
                iconName: "gear"
                toolTip: qsTr("Player settings and enhancements")
                Accessible.name: toolTip
                implicitWidth: 34
                implicitHeight: 36
                onClicked: root.requestMenu(moreButton, "settings")
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: root.controller.formatTime(root.controller.position)
                color: root.ink
                font.pixelSize: 10
                font.family: "monospace"
                font.weight: Font.Medium
                Accessible.name: qsTr("Position %1").arg(text)
            }

            Timeline {
                id: timeline
                controller: root.controller
                objectName: "player.pip.timeline.scrub"
                Layout.fillWidth: true
                Layout.preferredHeight: 28
            }

            Label {
                text: root.controller.formatRemaining(root.controller.position)
                color: root.mutedInk
                font.pixelSize: 10
                font.family: "monospace"
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignRight
                Accessible.name: qsTr("Remaining %1").arg(text)
            }
        }
    }

    // Native resize requests begin on the initial press. The compositor remains
    // authoritative on Wayland; no synthetic geometry fallback is attempted.
    MouseArea {
        id: leftResize
        x: 0; y: 8; width: 7; height: Math.max(0, parent.height - 16)
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeHorCursor
        onPressed: root.beginResize(leftResize, Qt.LeftEdge)
    }
    MouseArea {
        id: rightResize
        x: parent.width - width; y: 8; width: 7; height: Math.max(0, parent.height - 16)
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeHorCursor
        onPressed: root.beginResize(rightResize, Qt.RightEdge)
    }
    MouseArea {
        id: topResize
        x: 8; y: 0; width: Math.max(0, parent.width - 16); height: 7
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeVerCursor
        onPressed: root.beginResize(topResize, Qt.TopEdge)
    }
    MouseArea {
        id: bottomResize
        x: 8; y: parent.height - height; width: Math.max(0, parent.width - 16); height: 7
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeVerCursor
        onPressed: root.beginResize(bottomResize, Qt.BottomEdge)
    }
    MouseArea {
        id: topLeftResize
        x: 0; y: 0; width: 9; height: 9
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeFDiagCursor
        onPressed: root.beginResize(topLeftResize, Qt.TopEdge | Qt.LeftEdge)
    }
    MouseArea {
        id: topRightResize
        x: parent.width - width; y: 0; width: 9; height: 9
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.beginResize(topRightResize, Qt.TopEdge | Qt.RightEdge)
    }
    MouseArea {
        id: bottomLeftResize
        x: 0; y: parent.height - height; width: 9; height: 9
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.beginResize(bottomLeftResize, Qt.BottomEdge | Qt.LeftEdge)
    }
    MouseArea {
        id: bottomRightResize
        x: parent.width - width; y: parent.height - height; width: 9; height: 9
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeFDiagCursor
        onPressed: root.beginResize(bottomRightResize, Qt.BottomEdge | Qt.RightEdge)
    }

    Keys.onPressed: event => {
        root.controller.noteUserActivity()
        switch (event.key) {
        case Qt.Key_Space:
            root.controller.togglePlayPause()
            event.accepted = true
            break
        case Qt.Key_Left:
            root.controller.seekBy(-5)
            event.accepted = true
            break
        case Qt.Key_Right:
            root.controller.seekBy(5)
            event.accepted = true
            break
        case Qt.Key_Up:
            root.controller.adjustVolume(0.1)
            event.accepted = true
            break
        case Qt.Key_Down:
            root.controller.adjustVolume(-0.1)
            event.accepted = true
            break
        case Qt.Key_Escape:
            root.controller.leavePip()
            event.accepted = true
            break
        case Qt.Key_M:
            root.controller.toggleMute()
            event.accepted = true
            break
        case Qt.Key_F:
            root.controller.leavePip()
            event.accepted = true
            break
        default:
            event.accepted = root.controller.handleKey(event.key, event.modifiers)
            break
        }
    }
}

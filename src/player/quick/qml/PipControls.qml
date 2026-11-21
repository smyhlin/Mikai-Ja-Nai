import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Mikai.Player

// Minimal browser-style PiP overlay over the live video scene. Chrome stays
// hidden when the pointer leaves or the window loses focus, and dragging
// anywhere on the video surface moves the PiP window.
Item {
    id: root

    required property PlayerController controller

    readonly property bool windowActive: root.Window.window ? root.Window.window.active : false
    readonly property bool pointerInside: windowHover.hovered || dragArea.containsMouse
    readonly property bool resizing: leftResize.pressed || rightResize.pressed
                                     || topResize.pressed || bottomResize.pressed
                                     || topLeftResize.pressed || topRightResize.pressed
                                     || bottomLeftResize.pressed || bottomRightResize.pressed
    readonly property bool activeDragOrScrub: dragArea.pressed || timeline.dragging
                                              || volumeControl.interactionActive || resizing
    readonly property bool interactionActive: pointerInside || activeDragOrScrub
    readonly property bool chromeShown: pointerInside || activeDragOrScrub
                                        || (windowActive && keyboardReveal)

    property bool keyboardReveal: false

    readonly property Item timelineItem: timeline

    signal menuRequested(string menuId, rect anchorRect)

    anchors.fill: parent
    focus: true
    activeFocusOnTab: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Picture-in-picture controls")

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

    onWindowActiveChanged: {
        if (!windowActive) {
            keyboardReveal = false
        }
    }

    onInteractionActiveChanged: {
        if (root.interactionActive) root.controller.noteUserActivity()
    }

    // Tracks hover across the entire PiP surface without consuming child input.
    HoverHandler {
        id: windowHover
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    }

    Timer {
        id: pipSingleClickTimer
        interval: Math.min(220, Qt.styleHints.mouseDoubleClickInterval)
        repeat: false
        onTriggered: {
            if (root.controller.hasMedia) {
                root.controller.noteUserActivity()
                root.controller.togglePlayPause()
            }
        }
    }

    // Full-surface drag & click target: drag anywhere on the video to move the
    // PiP window, single-click to toggle Play/Pause, double-click to expand.
    MouseArea {
        id: dragArea
        anchors.fill: parent
        z: 0
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        property point pressPos: Qt.point(0, 0)
        property bool dragStarted: false

        onPressed: mouse => {
            pressPos = Qt.point(mouse.x, mouse.y)
            dragStarted = false
            root.forceActiveFocus()
            root.controller.noteUserActivity()
            mouse.accepted = true
        }
        onPositionChanged: mouse => {
            if (!pressed || dragStarted) return
            const dx = mouse.x - pressPos.x
            const dy = mouse.y - pressPos.y
            if ((dx * dx + dy * dy) >= 16) {
                pipSingleClickTimer.stop()
                dragStarted = true
                root.controller.beginWindowMove()
            }
        }
        onReleased: {
            if (!dragStarted && root.controller.hasMedia) {
                pipSingleClickTimer.restart()
            }
            dragStarted = false
        }
        onCanceled: {
            pipSingleClickTimer.stop()
            dragStarted = false
        }
        onDoubleClicked: {
            pipSingleClickTimer.stop()
            dragStarted = true
            root.controller.leavePip()
        }
    }

    // Auto-hiding minimal chrome layer: fades completely away on mouse/focus leave.
    Item {
        id: chromeLayer
        anchors.fill: parent
        z: 2
        opacity: root.chromeShown ? 1.0 : 0.0
        visible: opacity > 0.01

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }

        // Subtle top edge gradient for icon legibility.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 60
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.58) }
                GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.0) }
            }
        }

        // Subtle bottom edge gradient for timestamp & seek bar legibility.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 64
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.0) }
                GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.65) }
            }
        }

        // Top-left: mute button + slim volume slider.
        VolumeControl {
            id: volumeControl
            controller: root.controller
            compact: true
            objectName: "player.pip.volume"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 8
            anchors.topMargin: 6
            width: Math.min(138, Math.max(84, parent.width * 0.32))
            height: 32
        }

        // Top-right: subtle settings gear, restore/expand, close.
        RowLayout {
            id: topRightActions
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: 8
            anchors.topMargin: 6
            spacing: 2

            PlayerButton {
                id: moreButton
                controller: root.controller
                objectName: "player.pip.settings.button"
                compact: true
                iconName: "gear"
                toolTip: qsTr("Player settings and enhancements")
                Accessible.name: toolTip
                implicitWidth: 30
                implicitHeight: 30
                opacity: hovered || activeFocus ? 1.0 : 0.72
                onClicked: root.requestMenu(moreButton, "settings")
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

        // Center: clean large Play/Pause button.
        PlayerButton {
            id: centerPlayPause
            controller: root.controller
            objectName: "player.pip.playPause"
            anchors.centerIn: parent
            iconName: root.controller.playing ? "pause" : "play"
            toolTip: root.controller.playing ? qsTr("Pause (Space)") : qsTr("Play (Space)")
            Accessible.name: toolTip
            enabled: root.controller.hasMedia
            implicitWidth: 56
            implicitHeight: 56
            onClicked: {
                root.controller.noteUserActivity()
                root.controller.togglePlayPause()
            }
        }

        // Bottom: elapsed (left) & remaining (right) above a full-width slim seek bar.
        ColumnLayout {
            id: bottomBar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.bottomMargin: 4
            spacing: 0

            RowLayout {
                Layout.fillWidth: true

                Label {
                    text: root.controller.formatTime(root.controller.position)
                    color: "#ffffff"
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    Accessible.name: qsTr("Position %1").arg(text)
                }

                Item { Layout.fillWidth: true }

                Label {
                    text: root.controller.formatRemaining(root.controller.position)
                    color: "#e4e4e7"
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    horizontalAlignment: Text.AlignRight
                    Accessible.name: qsTr("Remaining %1").arg(text)
                }
            }

            Timeline {
                id: timeline
                controller: root.controller
                objectName: "player.pip.timeline.scrub"
                Layout.fillWidth: true
                Layout.preferredHeight: 20
            }
        }
    }

    // Native resize requests begin on the initial press. The compositor remains
    // authoritative on Wayland; no synthetic geometry fallback is attempted.
        MouseArea {
        id: leftResize
        z: 5
        x: 0; y: 8; width: 7; height: Math.max(0, parent.height - 16)
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeHorCursor
        onPressed: root.beginResize(leftResize, Qt.LeftEdge)
    }
    MouseArea {
        id: rightResize
        z: 5
        x: parent.width - width; y: 8; width: 7; height: Math.max(0, parent.height - 16)
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeHorCursor
        onPressed: root.beginResize(rightResize, Qt.RightEdge)
    }
    MouseArea {
        id: topResize
        z: 5
        x: 8; y: 0; width: Math.max(0, parent.width - 16); height: 7
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeVerCursor
        onPressed: root.beginResize(topResize, Qt.TopEdge)
    }
    MouseArea {
        id: bottomResize
        z: 5
        x: 8; y: parent.height - height; width: Math.max(0, parent.width - 16); height: 7
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeVerCursor
        onPressed: root.beginResize(bottomResize, Qt.BottomEdge)
    }
    MouseArea {
        id: topLeftResize
        z: 5
        x: 0; y: 0; width: 9; height: 9
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeFDiagCursor
        onPressed: root.beginResize(topLeftResize, Qt.TopEdge | Qt.LeftEdge)
    }
    MouseArea {
        id: topRightResize
        z: 5
        x: parent.width - width; y: 0; width: 9; height: 9
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.beginResize(topRightResize, Qt.TopEdge | Qt.RightEdge)
    }
    MouseArea {
        id: bottomLeftResize
        z: 5
        x: 0; y: parent.height - height; width: 9; height: 9
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.beginResize(bottomLeftResize, Qt.BottomEdge | Qt.LeftEdge)
    }
    MouseArea {
        id: bottomRightResize
        z: 5
        x: parent.width - width; y: parent.height - height; width: 9; height: 9
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.beginResize(bottomRightResize, Qt.BottomEdge | Qt.RightEdge)
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            keyboardReveal = true
        }
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

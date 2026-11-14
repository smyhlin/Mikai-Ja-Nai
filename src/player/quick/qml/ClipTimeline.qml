pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Mikai.Player

Item {
    id: root

    required property PlayerController controller
    required property ClipController editor
    required property color textColor
    required property color surfaceColor
    required property color borderColor

    readonly property real span: Math.max(0.1, editor.windowEnd - editor.windowStart)
    readonly property real inX: xForTime(editor.selectionStart)
    readonly property real outX: xForTime(editor.selectionEnd)
    readonly property real needleX: xForTime(editor.position)
    readonly property real deviceRatio: Window.window ? Window.window.devicePixelRatio : 1
    readonly property real handleHitWidth: 14
    readonly property real frameHeight: 72

    implicitHeight: 140
    activeFocusOnTab: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Clip timeline")
    Accessible.description: qsTr("Drag either edge to trim, the middle to move, or the ruler to seek.")

    function xForTime(seconds: real): real {
        return Math.max(0, Math.min(width, (seconds - editor.windowStart) / span * width))
    }
    function timeForX(x: real): real {
        return editor.windowStart + Math.max(0, Math.min(width, x)) / Math.max(1, width) * span
    }

    onWidthChanged: editor.setFilmstripWidth(width)
    Component.onCompleted: editor.setFilmstripWidth(width)

    Text {
        anchors.left: parent.left
        anchors.top: parent.top
        text: root.editor.windowStartText
        color: root.textColor
        opacity: 0.6
        font.pixelSize: 11
        font.features: { "tnum": 1 }
        textFormat: Text.PlainText
    }
    Text {
        anchors.right: parent.right
        anchors.top: parent.top
        text: root.editor.windowEndText
        color: root.textColor
        opacity: 0.6
        font.pixelSize: 11
        font.features: { "tnum": 1 }
        textFormat: Text.PlainText
    }
    MouseArea {
        id: seekRail
        objectName: "clipSeekRail"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 32
        enabled: root.editor.editingEnabled
        cursorShape: Qt.PointingHandCursor
        onPressed: mouse => {
            root.forceActiveFocus()
            root.editor.seekTo(root.timeForX(mouse.x))
        }
        onPositionChanged: mouse => {
            if (pressed) root.editor.seekTo(root.timeForX(mouse.x))
        }
        onWheel: wheel => {
            root.editor.stepSeconds((wheel.angleDelta.y > 0 ? 1 : -1)
                                   * ((wheel.modifiers & Qt.ShiftModifier) ? 0.1 : 1))
            wheel.accepted = true
        }
    }

    Item {
        id: filmstrip
        objectName: "clipFilmstrip"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 28
        height: root.frameHeight
        clip: true

        Rectangle {
            anchors.fill: parent
            color: root.surfaceColor
            border.color: root.borderColor
            radius: Math.min(8, root.controller.cornerRadius)
            antialiasing: true
        }
        Repeater {
            id: frames
            model: root.editor.thumbnails
            delegate: Item {
                id: thumbnail
                required property int index
                required property url imageSource
                required property real timestamp
                x: index * filmstrip.width / Math.max(1, frames.count)
                width: filmstrip.width / Math.max(1, frames.count)
                height: filmstrip.height
                Image {
                    objectName: "clipThumbnail"
                    anchors.fill: parent
                    anchors.rightMargin: 1
                    source: thumbnail.imageSource
                    asynchronous: true
                    cache: false
                    fillMode: Image.PreserveAspectCrop
                    smooth: true
                }
            }
        }
        Rectangle {
            x: 0
            width: root.inX
            height: parent.height
            color: Qt.rgba(0, 0, 0, 0.65)
        }
        Rectangle {
            x: root.outX
            width: Math.max(0, parent.width - x)
            height: parent.height
            color: Qt.rgba(0, 0, 0, 0.65)
        }
        Rectangle {
            x: root.inX
            width: Math.max(2, root.outX - root.inX)
            height: parent.height
            color: Qt.rgba(root.controller.accentColor.r, root.controller.accentColor.g,
                           root.controller.accentColor.b, 0.18)
            border.width: 2
            border.color: root.controller.accentColor
            radius: Math.min(6, root.controller.cornerRadius)
            antialiasing: true
        }
        Repeater {
            model: 2
            delegate: Rectangle {
                required property int index
                readonly property bool leading: index === 0
                x: leading ? root.inX : root.outX - width
                width: 12
                height: parent.height
                color: root.controller.accentColor
                radius: Math.min(4, root.controller.cornerRadius)
                antialiasing: true
                Rectangle {
                    anchors.centerIn: parent
                    width: 2
                    height: 24
                    radius: 1
                    color: root.editor.accentInk
                }
            }
        }
        MouseArea {
            id: rangeInput
            objectName: "clipRangeInput"
            anchors.fill: parent
            hoverEnabled: true
            enabled: root.editor.editingEnabled
            preventStealing: true
            property int mode: 0
            property real anchorX: 0
            property real anchorTime: 0
            property real originalStart: 0
            property real originalEnd: 0
            cursorShape: {
                const left = Math.abs(mouseX - root.inX)
                const right = Math.abs(mouseX - root.outX)
                if (left <= root.handleHitWidth || right <= root.handleHitWidth)
                    return Qt.SizeHorCursor
                if (mouseX > root.inX && mouseX < root.outX)
                    return pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                return Qt.PointingHandCursor
            }
            onPressed: mouse => {
                root.forceActiveFocus()
                root.editor.beginTimelineGesture()
                anchorX = mouse.x
                anchorTime = root.timeForX(mouse.x)
                originalStart = root.editor.selectionStart
                originalEnd = root.editor.selectionEnd
                const left = Math.abs(mouse.x - root.inX)
                const right = Math.abs(mouse.x - root.outX)
                if (Math.min(left, right) <= root.handleHitWidth) mode = left <= right ? 1 : 2
                else if (mouse.x > root.inX && mouse.x < root.outX) mode = 3
                else {
                    mode = 4
                    root.editor.seekTo(anchorTime)
                }
            }
            onPositionChanged: mouse => {
                if (!pressed) return
                const seconds = root.timeForX(mouse.x)
                if (mode === 1) {
                    root.editor.setSelection(seconds, originalEnd, 1)
                    root.editor.seekTo(root.editor.selectionStart)
                } else if (mode === 2) {
                    root.editor.setSelection(originalStart, seconds, 2)
                    root.editor.seekTo(root.editor.selectionEnd)
                } else if (mode === 3) {
                    root.editor.moveSelection(originalStart, seconds - anchorTime)
                } else if (mode === 4 && Math.abs(mouse.x - anchorX) >= Qt.styleHints.startDragDistance) {
                    root.editor.setSelection(Math.min(anchorTime, seconds), Math.max(anchorTime, seconds))
                    root.editor.seekTo(seconds)
                }
            }
            onReleased: {
                if (mode === 3) root.editor.seekTo(root.editor.selectionStart)
                mode = 0
            }
            onCanceled: mode = 0
        }
    }
    Rectangle {
        id: moveGrip
        objectName: "clipMoveSelection"
        x: Math.max(0, Math.min(root.width - width, (root.inX + root.outX - width) / 2))
        y: filmstrip.y + filmstrip.height + 3
        width: 36
        height: 24
        radius: Math.min(6, root.controller.cornerRadius)
        color: moveInput.containsMouse || moveInput.pressed ? root.controller.accentColor : root.surfaceColor
        border.color: root.borderColor
        border.width: 1
        enabled: root.editor.editingEnabled
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Move selected interval")
        ToolTip.visible: moveInput.containsMouse || activeFocus
        ToolTip.text: qsTr("Drag to move the whole clip. Left / Right moves one second; Shift moves 0.1 seconds.")
        Row {
            anchors.centerIn: parent
            spacing: 3
            Repeater {
                model: 3
                Rectangle {
                    width: 2
                    height: 10
                    radius: 1
                    color: moveInput.containsMouse || moveInput.pressed ? root.editor.accentInk : root.textColor
                    opacity: 0.8
                }
            }
        }
        Keys.onPressed: event => {
            if (event.key !== Qt.Key_Left && event.key !== Qt.Key_Right) return
            const delta = (event.key === Qt.Key_Left ? -1 : 1)
                          * ((event.modifiers & Qt.ShiftModifier) ? 0.1 : 1)
            root.editor.moveSelection(root.editor.selectionStart, delta)
            event.accepted = true
        }
        MouseArea {
            id: moveInput
            anchors.fill: parent
            hoverEnabled: true
            preventStealing: true
            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            property real initialTime: 0
            property real initialStart: 0
            onPressed: mouse => {
                moveGrip.forceActiveFocus()
                root.editor.beginTimelineGesture()
                initialTime = root.timeForX(mapToItem(root, mouse.x, mouse.y).x)
                initialStart = root.editor.selectionStart
            }
            onPositionChanged: mouse => {
                if (pressed)
                    root.editor.moveSelection(initialStart,
                        root.timeForX(mapToItem(root, mouse.x, mouse.y).x) - initialTime)
            }
            onReleased: root.editor.seekTo(root.editor.selectionStart)
        }
    }
    Rectangle {
        x: Math.round(root.needleX * root.deviceRatio) / root.deviceRatio - width / 2
        y: 25
        width: 2
        height: filmstrip.y + filmstrip.height - y
        color: Qt.rgba(1, 1, 1, 1)
        Accessible.ignored: true
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: -3
            width: 8
            height: 8
            radius: 4
            color: parent.color
            antialiasing: true
        }
    }
}

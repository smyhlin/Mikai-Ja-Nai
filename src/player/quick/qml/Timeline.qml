pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Mikai.Player

// The seek rail. Length, playhead and skip bands come from the controller; this
// item owns only the pointer gesture and the transient hover readout. A drag
// seeks continuously so the clock never runs ahead of the engine, and the rail
// is the only place a preview is requested - the view glue owns the card.
Item {
    id: root

    required property PlayerController controller

    readonly property real mediaDuration: Number.isFinite(controller.duration)
                                         ? Math.max(0, controller.duration) : 0
    readonly property real positionFraction: {
        const position = Number(controller.position)
        return seekable && Number.isFinite(position)
               ? Math.max(0, Math.min(1, position / mediaDuration)) : 0
    }
    readonly property real value: Number.isFinite(Number(controller.position))
                                  ? Math.max(0, Math.min(mediaDuration, Number(controller.position))) : 0
    readonly property real minimumValue: 0
    readonly property real maximumValue: mediaDuration
    readonly property real stepSize: 5
    readonly property bool seekable: mediaDuration > 0
    readonly property bool dragging: scrub.pressed && scrub.dragStarted
    readonly property bool interactionActive: root.dragging || root.activeFocus

    readonly property color railColor: Qt.rgba(1, 1, 1, 0.2)
    readonly property color playedColor: controller.accentColor
    readonly property color markColor: Qt.rgba(controller.accentColor.r, controller.accentColor.g,
                                               controller.accentColor.b, 0.42)
    readonly property color readoutText: controller.darkTheme ? "#f4f4f5" : "#18181b"
    readonly property color readoutFill: Qt.rgba(0, 0, 0, controller.darkTheme ? 0.72 : 0.6)

    signal previewRequested(real seconds, point anchorPoint)
    signal previewDismissed()

    objectName: "player.timeline.scrub"
    implicitWidth: 320
    implicitHeight: 28

    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Timeline")
    Accessible.description: qsTr("Drag to seek. Left and right arrows seek five seconds; Home and End jump to either end.")
    Accessible.onIncreaseAction: root.seekByClamped(5)
    Accessible.onDecreaseAction: root.seekByClamped(-5)

    function fractionAt(x) {
        if (!root.seekable || root.width <= 0) return 0
        return Math.max(0, Math.min(1, x / root.width))
    }

    function secondsAt(x) {
        return root.fractionAt(x) * root.mediaDuration
    }

    function seekByClamped(delta) {
        if (!root.seekable) return
        const position = Number(root.controller.position)
        const current = Number.isFinite(position) ? position : 0
        root.controller.seekTo(Math.max(0, Math.min(root.mediaDuration, current + delta)))
    }

    function pointerAnchor(x) {
        return root.mapToItem(null, x, 0)
    }

    Rectangle {
        id: track
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: 4
        radius: height / 2
        color: root.railColor
    }

    // One band per provider segment. The clock decides which of them is offered
    // as a skip pill; the rail only shows where they are.
    Repeater {
        model: root.controller.timelineMarks

        delegate: Rectangle {
            id: markBand

            required property var modelData
            readonly property var mark: modelData
            readonly property real startSeconds: Number(mark.start)
            readonly property real endSeconds: Number(mark.end)
            readonly property real from: root.seekable && Number.isFinite(startSeconds)
                                          ? Math.max(0, Math.min(1, startSeconds / root.mediaDuration))
                                          : 0
            readonly property real to: root.seekable && Number.isFinite(endSeconds)
                                        ? Math.max(0, Math.min(1, endSeconds / root.mediaDuration))
                                        : 0
            readonly property real markerWidth: Math.min(track.width, Math.max(2, (to - from) * track.width))

            visible: root.seekable && track.width > 0 && Number.isFinite(startSeconds)
                     && Number.isFinite(endSeconds) && to > from
            x: track.x + Math.min(from * track.width, track.width - markerWidth)
            width: markerWidth
            height: track.height
            radius: height / 2
            anchors.verticalCenter: track.verticalCenter
            color: root.markColor
        }
    }

    Rectangle {
        id: played
        anchors.verticalCenter: parent.verticalCenter
        width: root.positionFraction * root.width
        height: track.height
        radius: height / 2
        color: root.playedColor
    }

    Rectangle {
        id: hoverMarker
        visible: scrub.containsMouse && !root.dragging && root.seekable
        x: scrub.mouseX - width / 2
        anchors.verticalCenter: parent.verticalCenter
        width: 2
        height: track.height + 6
        color: Qt.rgba(1, 1, 1, 0.7)
    }

    Rectangle {
        id: handle
        anchors.verticalCenter: parent.verticalCenter
        x: root.positionFraction * root.width - width / 2
        width: 12
        height: 12
        radius: width / 2
        color: "white"
        border.width: 1
        border.color: Qt.rgba(0, 0, 0, 0.25)
        visible: root.seekable
    }

    // The drag readout is time and label only: the frame under the pointer is
    // the hover card's job, and it is not shown while the engine is following
    // the drag.
    Rectangle {
        id: dragReadout
        visible: root.dragging
        x: Math.max(0, Math.min(root.width - width, scrub.mouseX - width / 2))
        y: -height - 6
        width: readout.implicitWidth + 16
        height: readout.implicitHeight + 8
        radius: Math.max(3, Math.min(8, root.controller.cornerRadius))
        color: root.readoutFill

        Label {
            id: readout
            anchors.centerIn: parent
            text: root.controller.formatTime(root.secondsAt(scrub.mouseX))
            color: root.readoutText
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
    }

    MouseArea {
        id: scrub

        property bool dragStarted: false

        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        enabled: root.seekable
        cursorShape: root.seekable ? Qt.PointingHandCursor : Qt.ArrowCursor

        function requestPreview() {
            root.previewRequested(root.secondsAt(mouseX), root.pointerAnchor(mouseX))
        }

        onEntered: {
            if (root.seekable && !pressed) requestPreview()
        }
        onPressed: mouse => {
            root.forceActiveFocus()
            root.controller.noteUserActivity()
            dragStarted = true
            root.controller.seekTo(root.secondsAt(mouse.x))
            root.previewRequested(root.secondsAt(mouse.x),
                                  root.pointerAnchor(mouse.x))
            mouse.accepted = true
        }
        onPositionChanged: mouse => {
            if (pressed && dragStarted) {
                // Clamped against the live duration, not a value sampled when
                // the drag began.
                root.controller.seekTo(root.secondsAt(mouse.x))
                root.previewRequested(root.secondsAt(mouse.x),
                                      root.pointerAnchor(mouse.x))
                return
            }
            if (containsMouse) root.previewRequested(root.secondsAt(mouse.x),
                                                     root.pointerAnchor(mouse.x))
        }
        onReleased: {
            dragStarted = false
            if (containsMouse && root.seekable) {
                root.previewRequested(root.secondsAt(mouseX),
                                      root.pointerAnchor(mouseX))
            } else {
                root.previewDismissed()
            }
        }
        onExited: {
            if (!pressed) root.previewDismissed()
        }
        onContainsMouseChanged: {
            if (!containsMouse && !pressed) root.previewDismissed()
        }
    }

    Connections {
        target: root.controller
        function onMediaChanged() {
            root.previewDismissed()
            root.controller.hideHoverPreview()
        }
    }

    focus: false
    activeFocusOnTab: true

    Keys.onPressed: event => {
        if (!event.isAutoRepeat) root.controller.noteUserActivity()
        switch (event.key) {
        case Qt.Key_Left:
            root.seekByClamped(-5)
            event.accepted = true
            break
        case Qt.Key_Right:
            root.seekByClamped(5)
            event.accepted = true
            break
        case Qt.Key_Home:
            if (root.seekable) root.controller.seekTo(0)
            event.accepted = true
            break
        case Qt.Key_End:
            if (root.seekable) root.controller.seekTo(root.mediaDuration)
            event.accepted = true
            break
        case Qt.Key_Space:
            root.controller.togglePlayPause()
            event.accepted = true
            break
        default:
            break
        }
    }
}

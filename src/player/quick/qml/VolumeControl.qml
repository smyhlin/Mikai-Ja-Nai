import QtQuick
import QtQuick.Layouts
import Mikai.Player

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

    readonly property bool interactionActive: trackArea.pressed || root.focusInside
    readonly property real level: Number.isFinite(root.controller.volume)
                                  ? Math.max(0, Math.min(1, root.controller.volume)) : 0
    readonly property real value: level

    readonly property color railColor: Qt.rgba(1, 1, 1, 0.22)
    readonly property color fillColor: root.controller.muted ? Qt.rgba(1, 1, 1, 0.4)
                                                             : root.controller.accentColor
    readonly property string glyph: root.controller.muted || root.level <= 0.001
                                    ? "mute"
                                    : root.level < 0.5 ? "volume-low" : "volume"

    objectName: "player.volume"
    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Volume")

    function setLevelAt(x) {
        if (rail.width <= 0) return
        root.controller.setVolume(Math.max(0, Math.min(1, x / rail.width)))
    }

    function volumeText() {
        return qsTr("%1%").arg(Math.round(root.level * 100))
    }

    RowLayout {
        id: row
        anchors.fill: parent
        spacing: 6

        PlayerButton {
            id: muteButton
            controller: root.controller
            objectName: "player.volume.mute"
            compact: true
            iconName: root.glyph
            toolTip: root.controller.muted ? qsTr("Unmute (M)") : qsTr("Mute (M)")
            Accessible.name: toolTip
            implicitWidth: 40
            implicitHeight: 40
            onClicked: {
                root.controller.noteUserActivity()
                root.controller.toggleMute()
            }
        }

        Item {
            id: railHost

            objectName: "player.volume.slider"
            readonly property real value: root.level
            readonly property real minimumValue: 0
            readonly property real maximumValue: 1
            readonly property real stepSize: 0.1

            Layout.fillWidth: true
            Layout.minimumWidth: 56
            Layout.maximumWidth: 152
            Layout.preferredWidth: 140
            Layout.alignment: Qt.AlignVCenter
            implicitHeight: 24

            focus: false
            activeFocusOnTab: true

            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Volume")
            Accessible.description: qsTr("Drag or use the arrow keys to change the volume.")
            Accessible.onIncreaseAction: root.controller.adjustVolume(0.1)
            Accessible.onDecreaseAction: root.controller.adjustVolume(-0.1)

            Rectangle {
                id: rail
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 4
                radius: height / 2
                color: root.railColor
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: root.level * rail.width
                height: rail.height
                radius: height / 2
                color: root.fillColor
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                x: root.level * rail.width - width / 2
                width: 10
                height: 10
                radius: width / 2
                color: "white"
                border.width: 1
                border.color: Qt.rgba(0, 0, 0, 0.25)
            }

            MouseArea {
                id: trackArea
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.PointingHandCursor

                onPressed: mouse => {
                    railHost.forceActiveFocus()
                    root.controller.noteUserActivity()
                    root.setLevelAt(mouse.x)
                    mouse.accepted = true
                }
                onPositionChanged: mouse => {
                    if (pressed) root.setLevelAt(mouse.x)
                }
                onWheel: wheel => {
                    const angleDelta = wheel.angleDelta.y
                    const pixelDelta = wheel.pixelDelta.y
                    const steps = angleDelta !== 0 ? angleDelta / 120 : pixelDelta / 15
                    if (steps !== 0) root.controller.adjustVolume(steps * 0.1)
                    wheel.accepted = true
                }
            }

            Keys.onPressed: event => {
                if (!event.isAutoRepeat) root.controller.noteUserActivity()
                switch (event.key) {
                case Qt.Key_Left:
                case Qt.Key_Down:
                    root.controller.adjustVolume(-0.1)
                    event.accepted = true
                    break
                case Qt.Key_Right:
                case Qt.Key_Up:
                    root.controller.adjustVolume(0.1)
                    event.accepted = true
                    break
                default:
                    break
                }
            }
        }
    }
}

import QtQuick
import QtQuick.Controls.Basic
import Mikai.Player

Item {
    id: root

    required property PlayerController controller

    property point anchor: Qt.point(0, 0)

    readonly property bool shown: controller.hoverPreviewVisible
    readonly property color textColor: controller.darkTheme ? "#f4f4f5" : "#18181b"
    readonly property color mutedColor: controller.darkTheme ? "#a1a1aa" : "#52525b"
    readonly property color surfaceColor: controller.darkTheme ? Qt.rgba(0.07, 0.08, 0.10, 0.96)
                                                               : Qt.rgba(0.98, 0.98, 0.99, 0.98)
    readonly property color borderColor: controller.darkTheme ? Qt.rgba(1, 1, 1, 0.16)
                                                              : Qt.rgba(0, 0, 0, 0.14)

    readonly property real cardRadius: Math.max(2, controller.cornerRadius)
    readonly property real videoAspect: (controller.sourceWidth > 0 && controller.sourceHeight > 0)
                                        ? Math.max(0.75, Math.min(2.4, controller.sourceWidth / controller.sourceHeight))
                                        : (16 / 9)
    readonly property bool hasFrameArea: frame.status !== Image.Error
    objectName: "player.timeline.preview"
    visible: shown
    width: Math.min(208, Math.max(0, parent.width - 16))
    height: Math.min(Math.max(0, parent.height - 16),
                     (hasFrameArea ? Math.round(width / videoAspect) : 0) + labelBar.height)
    x: Math.max(8, Math.min(parent.width - width - 8, anchor.x - width / 2))
    y: Math.max(8, Math.min(parent.height - height - 8, anchor.y - height - 10))
    z: 15

    Accessible.role: Accessible.ToolTip
    Accessible.name: controller.hoverPreviewLabel !== ""
                     ? qsTr("Preview at %1, %2").arg(controller.hoverPreviewTime)
                                                 .arg(controller.hoverPreviewLabel)
                     : qsTr("Preview at %1").arg(controller.hoverPreviewTime)

    Rectangle {
        id: card
        anchors.fill: parent
        radius: root.cardRadius
        color: root.surfaceColor
        border.width: 1
        border.color: root.borderColor
        clip: true

        Rectangle {
            id: imageFrame
            visible: root.hasFrameArea
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: root.hasFrameArea
                    ? Math.min(Math.round(width / root.videoAspect), Math.max(0, root.height - labelBar.height))
                    : 0
            color: Qt.rgba(0, 0, 0, 0.35)
            clip: true

            Image {
                id: frame
                objectName: "player.timeline.preview.frame"
                anchors.fill: parent
                source: root.controller.hoverPreviewSource
                asynchronous: false
                cache: false
                fillMode: Image.PreserveAspectFit
                visible: status === Image.Ready
            }

            Label {
                anchors.centerIn: parent
                visible: frame.status === Image.Loading || frame.status === Image.Null
                text: qsTr("Loading preview…")
                color: root.mutedColor
                font.pixelSize: 11
            }
        }

        Rectangle {
            id: labelBar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: imageFrame.bottom
            height: 26
            color: "transparent"

            Label {
                id: timeLabel
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: root.controller.hoverPreviewTime
                color: root.textColor
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.features: ({ "tnum": 1 })
            }

            Label {
                id: segmentLabel
                visible: text !== ""
                anchors.left: timeLabel.right
                anchors.leftMargin: 8
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: root.controller.hoverPreviewLabel
                color: root.mutedColor
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
    }
}

import QtQuick
import QtQuick.Controls.Basic
import Mikai.Player

// The timeline hover card. The decoded frame is never produced here: the
// controller's FramePreviewService decodes on demand and publishes
// `image://mikai-hover/<token>`; this item only loads that URL and shows the
// controller's own time/label. Placement is anchored to the timeline and
// clamped inside the view, and the card takes no input at all.
Item {
    id: root

    required property PlayerController controller

    /// Pointer anchor in the view glue's coordinate space (the view root).
    property point anchor: Qt.point(0, 0)
    /// The timeline this card describes; the card sits just above it.
    property Item timelineItem: null

    readonly property bool shown: controller.hoverPreviewVisible
                                  && controller.hoverPreviewSource.toString() !== ""
    readonly property color textColor: controller.darkTheme ? "#f4f4f5" : "#18181b"
    readonly property color mutedColor: controller.darkTheme ? "#a1a1aa" : "#52525b"
    readonly property color surfaceColor: controller.darkTheme ? Qt.rgba(0.07, 0.08, 0.10, 0.96)
                                                               : Qt.rgba(0.98, 0.98, 0.99, 0.98)
    readonly property color borderColor: controller.darkTheme ? Qt.rgba(1, 1, 1, 0.16)
                                                              : Qt.rgba(0, 0, 0, 0.14)

    readonly property real cardRadius: Math.max(2, controller.cornerRadius)
    readonly property real timelineTop: timelineItem
                                        ? timelineItem.mapToItem(parent, 0, 0).y
                                        : parent.height - 120

    visible: shown
    width: Math.min(208, Math.max(0, parent.width - 16))
    height: Math.min(Math.max(0, parent.height - 16),
                     imageFrame.height + labelBar.height)
    x: Math.max(8, Math.min(parent.width - width - 8, anchor.x - width / 2))
    y: Math.max(8, Math.min(parent.height - height - 8, timelineTop - height - 10))
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
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Math.round(width * 9 / 16)
            color: Qt.rgba(0, 0, 0, 0.35)
            clip: true

            Image {
                id: frame
                anchors.fill: parent
                source: root.controller.hoverPreviewSource
                asynchronous: true
                cache: false
                fillMode: Image.PreserveAspectCrop
                visible: status === Image.Ready
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

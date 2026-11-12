import QtQuick
import Mikai.Player

// Shared card for every panel the native player shows over the picture.
//
// The controller owns accent, corner radius and theme; this item owns nothing
// but geometry. Content goes into the default slot (a child of `contentItem`),
// which is inset far enough that nothing crosses the rounded corners, and the
// panel's natural size follows that content unless the caller sets a size.
Item {
    id: root

    required property PlayerController controller

    property string title
    property real padding: 12
    property bool opaqueSurface: false

    /// The padded content host: a bound width and height live here, never a
    /// layout of its own, so children keep full control of their placement.
    readonly property Item contentItem: contentHost
    default property alias contentChildren: contentHost.data

    // The player card palette, theme-aware and shared with the sheets: the
    // card fill and hairline of the Widgets chrome in dark, the app's light
    // surface in light. Accent and radius are the controller's live tokens.
    readonly property color textColor: controller.darkTheme ? "#f4f4f5" : "#18181b"
    readonly property color cardColor: controller.darkTheme ? Qt.rgba(20 / 255, 21 / 255,
                                                                     27 / 255, opaqueSurface ? 1 : 0.97)
                                                           : Qt.rgba(0.98, 0.98, 0.985, opaqueSurface ? 1 : 0.98)
    readonly property color edgeColor: controller.darkTheme ? Qt.rgba(1, 1, 1, 0.18)
                                                            : Qt.rgba(0, 0, 0, 0.12)
    readonly property real cardRadius: Math.max(0, Math.min(controller.cornerRadius,
                                                             Math.min(width, height) / 2))

    // Keep content inside the rounded corners, even when callers use no padding.
    readonly property real inset: Math.max(Math.max(0, padding), cardRadius * 0.293)
    readonly property real titleGap: 10

    implicitWidth: Math.ceil(inset * 2 + Math.max(titleText.implicitWidth,
                                                  contentHost.implicitWidth))
    implicitHeight: Math.ceil(inset * 2 + (titleText.visible ? titleText.implicitHeight
                                                            + titleGap : 0)
                              + contentHost.childrenRect.height)

    Accessible.role: Accessible.Pane
    Accessible.name: root.title.length > 0 ? root.title : ""
    Accessible.ignored: root.title.length === 0

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: mouse => { mouse.accepted = true }
        Accessible.ignored: true
    }

    Rectangle {
        id: surface
        anchors.fill: parent
        radius: root.cardRadius
        color: root.cardColor
        border.width: 1
        border.color: root.edgeColor
        antialiasing: true
    }

    Text {
        id: titleText
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: root.inset
        anchors.rightMargin: root.inset
        anchors.topMargin: root.inset
        text: root.title
        visible: text.length > 0
        color: root.textColor
        font.pixelSize: 13
        font.weight: Font.DemiBold
        elide: Text.ElideRight
        wrapMode: Text.NoWrap
        textFormat: Text.PlainText
        Accessible.ignored: true
    }

    Item {
        id: contentHost
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: titleText.visible ? titleText.bottom : parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.inset
        anchors.rightMargin: root.inset
        anchors.topMargin: titleText.visible ? root.titleGap : root.inset
        anchors.bottomMargin: root.inset
    }
}

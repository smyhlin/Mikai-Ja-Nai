import QtQuick
import QtQuick.Controls.Basic
import Mikai.Player

// The one button of the native player chrome: a Basic.ToolButton (text, enabled,
// checkable, checked and clicked stay the inherited contract) with an optional
// provider glyph and a themed, instantaneous state. States are plain colour
// swaps - no animation, no hover delay, no popup of its own.
ToolButton {
    id: root

    required property PlayerController controller

    property string iconName
    property string toolTip
    property bool compact: false

    readonly property int minimumSide: compact ? 26 : 34
    readonly property color textColor: !enabled
                                       ? (controller.darkTheme ? Qt.rgba(1, 1, 1, 0.35)
                                                               : Qt.rgba(0, 0, 0, 0.35))
                                       : (controller.darkTheme ? "#f4f4f5" : "#18181b")
    readonly property color wash: checked
                                  ? Qt.rgba(controller.accentColor.r, controller.accentColor.g,
                                            controller.accentColor.b, 0.22)
                                  : pressed
                                    ? (controller.darkTheme ? Qt.rgba(1, 1, 1, 0.18)
                                                            : Qt.rgba(0, 0, 0, 0.10))
                                    : hovered
                                      ? (controller.darkTheme ? Qt.rgba(1, 1, 1, 0.10)
                                                              : Qt.rgba(0, 0, 0, 0.05))
                                      : "transparent"
    readonly property color edgeColor: visualFocus
                                       ? controller.accentColor
                                       : checked
                                         ? Qt.rgba(controller.accentColor.r,
                                                   controller.accentColor.g,
                                                   controller.accentColor.b, 0.55)
                                         : "transparent"
    readonly property real buttonRadius: Math.max(0, Math.min(controller.cornerRadius,
                                                              height / 2))

    activeFocusOnTab: true
    hoverEnabled: true
    leftPadding: compact ? 8 : 14
    rightPadding: compact ? 8 : 14
    topPadding: compact ? 4 : 7
    bottomPadding: compact ? 4 : 7

    implicitWidth: Math.max(minimumSide, contentBox.implicitWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(minimumSide, contentBox.implicitHeight + topPadding + bottomPadding)

    Accessible.role: Accessible.Button
    Accessible.name: toolTip.length > 0 ? toolTip
                                        : text.length > 0 ? text : iconName
    Accessible.description: toolTip.length > 0 && text.length > 0 ? text : ""
    Accessible.checkable: checkable
    Accessible.checked: checkable && checked
    Accessible.focusable: activeFocusOnTab
    Accessible.onPressAction: if (enabled) click()
    Accessible.onToggleAction: if (checkable) toggle()

    ToolTip.visible: (hovered || visualFocus) && toolTip.length > 0
    ToolTip.text: toolTip

    background: Rectangle {
        radius: root.buttonRadius
        color: root.wash
        border.width: root.edgeColor.a > 0 ? 1 : 0
        border.color: root.edgeColor
        antialiasing: true
    }

    contentItem: Item {
        id: contentBox
        readonly property real glyphGap: root.compact ? 6 : 8
        readonly property real glyphWidth: glyph.visible
                                           ? glyph.width + (textLabel.visible ? glyphGap : 0)
                                           : 0
        readonly property real labelWidth: textLabel.visible ? textLabel.implicitWidth : 0
        readonly property real groupWidth: glyphWidth + labelWidth
        readonly property real groupX: Math.max(0, Math.round((width - groupWidth) / 2))
        implicitWidth: groupWidth
        implicitHeight: Math.max(glyph.visible ? glyph.height : 0,
                                 textLabel.visible ? textLabel.implicitHeight : 0)

        PlayerIcon {
            id: glyph
            controller: root.controller
            name: root.iconName
            visible: root.iconName.length > 0
            sourceSize: root.compact ? 15 : 18
            tint: root.checked && root.enabled ? root.controller.accentColor : root.textColor
            x: contentBox.groupX
            y: Math.round((contentBox.height - height) / 2)
        }

        Text {
            id: textLabel
            x: contentBox.groupX + contentBox.glyphWidth
            y: Math.round((contentBox.height - height) / 2)
            text: root.text
            visible: text.length > 0
            color: root.textColor
            font.pixelSize: root.compact ? 11 : 12
            font.weight: Font.Medium
            elide: Text.ElideRight
            textFormat: Text.PlainText
            width: Math.min(implicitWidth, Math.max(0, contentBox.width - x))
            verticalAlignment: Text.AlignVCenter
        }
    }
}

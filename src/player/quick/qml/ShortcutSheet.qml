pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Mikai.Player

// The in-scene keyboard-shortcut card. Rows come from the controller's own
// keyHints model; the overlay keeps only its own focus and scroll position and
// closes through the controller-visible `visible` flag.
FocusScope {
    id: root

    required property PlayerController controller

    /// Standard Item.visible: the view glue owns z-order, this owns the card.
    visible: false
    focus: visible
    anchors.fill: parent

    readonly property color textColor: controller.darkTheme ? "#f4f4f5" : "#18181b"
    readonly property color mutedColor: controller.darkTheme ? "#a1a1aa" : "#52525b"
    readonly property color borderColor: controller.darkTheme ? Qt.rgba(1, 1, 1, 0.14)
                                                              : Qt.rgba(0, 0, 0, 0.14)
    readonly property color keycapColor: controller.darkTheme ? Qt.rgba(1, 1, 1, 0.06)
                                                              : Qt.rgba(0, 0, 0, 0.05)
    readonly property color scrimColor: Qt.rgba(0, 0, 0, controller.darkTheme ? 0.6 : 0.4)
    readonly property int cardRadius: Math.max(2, controller.cornerRadius)

    property Item previousFocus: null

    function open() {
        if (visible) return
        previousFocus = Window.window ? Window.window.activeFocusItem : null
        visible = true
        controller.noteUserActivity()
        forceActiveFocus()
        closeButton.forceActiveFocus()
    }

    function close() {
        if (!visible) return
        visible = false
        const restore = previousFocus
        previousFocus = null
        if (restore && restore.visible) restore.forceActiveFocus()
    }

    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("Keyboard shortcuts")
    Keys.onEscapePressed: event => {
        close()
        event.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        color: root.scrimColor
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: mouse => {
            root.controller.noteUserActivity()
            mouse.accepted = true
        }
        onWheel: wheel => {
            root.controller.noteUserActivity()
            wheel.accepted = true
        }
    }

    PlayerPanel {
        id: card
        controller: root.controller
        title: qsTr("Keyboard shortcuts")
        padding: 16
        anchors.centerIn: parent
        width: Math.min(Math.max(0, root.width - 24), 560)
        height: Math.min(Math.max(0, root.height - 32), implicitHeight)
        clip: true

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 12

            ListView {
                id: shortcutList
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: Math.min(contentHeight, 440)
                Layout.minimumHeight: 80
                clip: true
                spacing: 2
                model: root.controller.keyHintsMenu
                activeFocusOnTab: true
                KeyNavigation.tab: closeButton
                KeyNavigation.backtab: closeButton
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                Keys.onUpPressed: event => {
                    contentY = Math.max(0, contentY - 48)
                    event.accepted = true
                }
                Keys.onDownPressed: event => {
                    contentY = Math.min(Math.max(0, contentHeight - height), contentY + 48)
                    event.accepted = true
                }

                delegate: Item {
                    id: hintRow
                    required property var model
                    width: shortcutList.width
                    height: hintRow.model.heading ? headingLabel.implicitHeight + 14
                                                  : Math.max(keycap.height, actionLabel.implicitHeight) + 10

                    Accessible.role: Accessible.StaticText
                    Accessible.name: hintRow.model.heading
                                     ? hintRow.model.label
                                     : (hintRow.model.detail
                                        ? hintRow.model.label + ", " + hintRow.model.detail
                                        : hintRow.model.label)

                    Label {
                        id: headingLabel
                        visible: hintRow.model.heading
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: hintRow.model.heading ? hintRow.model.label.toUpperCase() : ""
                        color: root.mutedColor
                        font.pixelSize: 11
                        font.letterSpacing: 0.8
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        id: keycap
                        visible: !hintRow.model.heading
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(220, Math.max(64, keyLabel.implicitWidth + 20))
                        height: Math.max(26, keyLabel.implicitHeight + 8)
                        radius: Math.max(3, root.cardRadius - 2)
                        color: root.keycapColor
                        border.width: 1
                        border.color: root.borderColor

                        Label {
                            id: keyLabel
                            anchors.centerIn: parent
                            width: parent.width - 16
                            text: hintRow.model.label
                            color: root.textColor
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                        }
                    }

                    Label {
                        id: actionLabel
                        visible: !hintRow.model.heading
                        anchors.left: keycap.right
                        anchors.leftMargin: 14
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: hintRow.model.detail
                        color: hintRow.model.detail === "" ? root.mutedColor : root.textColor
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Press ? or / to open this card anywhere in the player.")
                    color: root.mutedColor
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                PlayerButton {
                    id: closeButton
                    controller: root.controller
                    text: qsTr("Close")
                    toolTip: qsTr("Close the shortcut card")
                    KeyNavigation.tab: shortcutList
                    KeyNavigation.backtab: shortcutList
                    onClicked: root.close()
                }
            }
        }
    }
}

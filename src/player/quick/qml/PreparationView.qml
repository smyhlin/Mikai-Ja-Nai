import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Mikai.Player

// The engine-preparation card: the whole content of the separate nonactivating
// status window. Every word comes from the controller's typed preparation
// fields; the window's own run state (running/succeeded/failed) only chooses the
// presentation. Nothing here starts, stops or inspects an engine.
Rectangle {
    id: root

    required property PlayerController controller
    /// The native PreparationWindow, injected as an initial property. Null only
    /// while the scene is being created by a tool; all bindings guard it.
    property var view: null

    readonly property bool dark: controller.darkTheme
    readonly property bool running: view !== null && view.running
    readonly property bool succeeded: view !== null && view.succeeded
    readonly property bool failed: view !== null && view.failed

    readonly property color surfaceColor: dark ? Qt.rgba(20 / 255, 21 / 255, 27 / 255, 0.98)
                                               : Qt.rgba(0.98, 0.98, 0.985, 0.99)
    readonly property color textColor: dark ? "#f4f4f5" : "#18181b"
    readonly property color mutedColor: dark ? "#a1a1aa" : "#52525b"
    readonly property color edgeColor: dark ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(0, 0, 0, 0.12)
    readonly property color railColor: dark ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(0, 0, 0, 0.08)
    readonly property color accent: controller.accentColor
    /// Ink that stays readable on the live accent, which the viewer can change.
    readonly property color onAccent: (0.299 * accent.r + 0.587 * accent.g
                                       + 0.114 * accent.b) > 0.6 ? "#101014" : "#ffffff"
    readonly property real padding: 18
    readonly property real cardRadius: Math.max(0, Math.min(controller.cornerRadius + 4,
                                                             Math.min(width, height) / 2))

    /// One-based stage wording, exactly the app-owned wrapper the Widgets
    /// overlay used; the caller's phase and detail stay verbatim.
    readonly property string stageLine: {
        if (controller.preparationStageIndex < 0) return controller.preparationPhase;
        const position = controller.preparationStageIndex + 1;
        const count = controller.preparationStageCount;
        return count > 0 ? qsTr("Stage %1 of %2: %3").arg(position).arg(count)
                                                .arg(controller.preparationPhase)
                         : qsTr("Stage %1: %2").arg(position)
                                               .arg(controller.preparationPhase);
    }

    readonly property string detailLine: succeeded || failed
                                         ? controller.preparationSummary
                                         : controller.preparationDetail

    // Compact status card: long model and phase words wrap instead of widening
    // the window, and the card's height follows them because the view sizes
    // itself to the root item.
    width: 400
    height: column.implicitHeight + padding * 2
    color: "transparent"

    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Engine preparation")

    Rectangle {
        id: surface
        anchors.fill: parent
        radius: root.cardRadius
        color: root.surfaceColor
        border.width: 1
        border.color: root.edgeColor
        antialiasing: true
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: root.padding
        spacing: 10

        Text {
            id: titleText
            Layout.fillWidth: true
            text: root.succeeded ? qsTr("Engine ready")
                                 : root.failed ? qsTr("Engine build stopped")
                                               : root.controller.preparationTitle
            color: root.textColor
            font.pixelSize: 15
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            textFormat: Text.PlainText
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            BusyIndicator {
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                visible: root.running && root.controller.preparationStageCount <= 0
                running: visible
                Accessible.ignored: true
            }

            Text {
                id: stageText
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.stageLine
                color: root.mutedColor
                font.pixelSize: 13
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                textFormat: Text.PlainText
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }

        ProgressBar {
            id: progress
            Layout.fillWidth: true
            Layout.preferredHeight: 6
            visible: root.controller.preparationStageCount > 0 || root.succeeded || root.failed
            from: 0
            to: Math.max(1, root.controller.preparationStageCount)
            value: root.succeeded ? 1
                                  : root.failed ? 0
                                                : Math.max(0, Math.min(root.controller.preparationStageIndex,
                                                                       to))
            Accessible.role: Accessible.ProgressBar
            Accessible.name: qsTr("Preparation progress")
            Accessible.description: root.controller.preparationStageCount > 0
                                    ? qsTr("%1 of %2 stages").arg(Math.max(0,
                                                                          Math.min(
                                                                              root.controller.preparationStageIndex,
                                                                              to)))
                                                             .arg(to)
                                    : qsTr("Stage count not known yet")

            background: Rectangle {
                implicitHeight: 6
                radius: height / 2
                color: root.railColor
            }

            contentItem: Item {
                implicitHeight: 6

                Rectangle {
                    width: parent.width * progress.visualPosition
                    height: parent.height
                    radius: height / 2
                    color: root.accent
                }
            }
        }

        Text {
            id: detailText
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.detailLine
            color: root.mutedColor
            font.pixelSize: 12
            wrapMode: Text.Wrap
            maximumLineCount: 4
            elide: Text.ElideRight
            textFormat: Text.PlainText
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Text {
            objectName: "player.preparation.elapsed"
            Layout.fillWidth: true
            visible: root.running
            text: root.view !== null
                  ? qsTr("Elapsed: %1:%2:%3")
                    .arg(String(Math.floor(root.view.elapsedSeconds / 3600)).padStart(2, "0"))
                    .arg(String(Math.floor(root.view.elapsedSeconds / 60) % 60).padStart(2, "0"))
                    .arg(String(root.view.elapsedSeconds % 60).padStart(2, "0"))
                  : ""
            color: root.mutedColor
            font.pixelSize: 12
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            BusyIndicator {
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                visible: root.running && root.controller.preparationCancelPending
                running: visible
                Accessible.name: qsTr("Cancellation requested")
                Accessible.ignored: !visible
            }

            Button {
                id: actionButton
                visible: !root.succeeded
                enabled: root.failed
                         || (root.running && !root.controller.preparationCancelPending)
                implicitHeight: 30
                implicitWidth: Math.max(96, actionLabel.implicitWidth + 24)
                padding: 12
                text: root.failed ? qsTr("Dismiss")
                                  : root.controller.preparationCancelPending
                                    ? qsTr("Stopping…") : qsTr("Cancel")
                Accessible.role: Accessible.Button
                Accessible.name: root.failed ? qsTr("Dismiss preparation status")
                                             : root.controller.preparationCancelPending
                                               ? qsTr("Cancellation requested")
                                               : qsTr("Cancel engine preparation")

                onClicked: {
                    if (root.failed) {
                        if (root.view !== null) root.view.dismiss();
                        return;
                    }
                    root.controller.cancelEnginePreparation();
                }

                background: Rectangle {
                    radius: Math.max(4, root.controller.cornerRadius)
                    color: root.accent
                    opacity: actionButton.enabled ? (actionButton.down ? 0.82 : 1.0) : 0.45
                    border.width: actionButton.activeFocus ? 2 : 0
                    border.color: root.onAccent

                    Behavior on opacity {
                        NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                    }
                }

                contentItem: Text {
                    id: actionLabel
                    text: actionButton.text
                    color: root.onAccent
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                }
            }
        }
    }
}

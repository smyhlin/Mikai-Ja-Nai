pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Mikai.Player

// Main transport chrome: the bottom dock, the flank episode chevrons and the
// provider skip pills. Every value is read from the controller and every action
// is one of its typed commands; menu buttons only name the level and the parent
// scene's rect, so no native popup is created here.
Item {
    id: root

    required property PlayerController controller

    anchors.fill: parent

    readonly property bool chromeVisible: !controller.autoHidden
    readonly property bool compactLayout: root.width < 620
    readonly property int dockWidth: Math.max(1, Math.min(980, root.width - 16))

    readonly property bool focusInside: {
        if (!root.Window.window) return false
        let node = root.Window.window.activeFocusItem
        while (node) {
            if (node === root) return true
            node = node.parent
        }
        return false
    }

    // Hover movement wakes the chrome, but passive hover is not an idle hold:
    // a pointer resting over the dock must not prevent fullscreen auto-hide.
    readonly property bool interactionActive: timeline.dragging
                                              || root.focusInside
                                              || pointerPress.pressed

    /// Placement hook for the hover card: the card is anchored to this item's
    /// scene rect by the view glue, never drawn here.
    readonly property Item timelineItem: timeline

    readonly property color labelColor: "white"
    readonly property color mutedLabelColor: Qt.rgba(1, 1, 1, 0.65)

    signal menuRequested(string menuId, rect anchorRect)

    implicitWidth: 320
    implicitHeight: 180
    // A real press holds the chrome; resting over it does not.
    TapHandler {
        id: pointerPress
        acceptedButtons: Qt.AllButtons
    }

    opacity: root.chromeVisible ? 1 : 0
    visible: opacity > 0.001
    Behavior on opacity {
        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
    }

    onInteractionActiveChanged: {
        if (root.interactionActive) root.controller.noteUserActivity()
    }

    /// Scene-space rect of a control, for the cascading menu placement. The
    /// controller and the overlay never see a QML item pointer.
    function anchorRect(item) {
        const p = item.mapToItem(root.parent, 0, 0)
        return Qt.rect(p.x, p.y, item.width, item.height)
    }

    function requestMenu(item, menuId) {
        root.controller.noteUserActivity()
        root.menuRequested(menuId, root.anchorRect(item))
    }

    // ── episode chevrons ──────────────────────────────────────────────
    PlayerButton {
        id: previousButton
        controller: root.controller
        iconName: "chevron-left"
        toolTip: qsTr("Previous episode")
        Accessible.name: toolTip
        compact: true
        visible: root.controller.hasPreviousEpisode && root.chromeVisible
        width: 48
        height: 48
        x: Math.max(8, Math.round(root.width * 0.012))
        y: Math.round((root.height - height) / 2)
        onClicked: {
            root.controller.noteUserActivity()
            root.controller.goToPreviousEpisode()
        }
    }

    PlayerButton {
        id: nextButton
        controller: root.controller
        iconName: "chevron-right"
        toolTip: qsTr("Next episode")
        Accessible.name: toolTip
        compact: true
        visible: root.controller.hasNextEpisode && root.chromeVisible
        width: 48
        height: 48
        x: root.width - width - Math.max(8, Math.round(root.width * 0.012))
        y: Math.round((root.height - height) / 2)
        onClicked: {
            root.controller.noteUserActivity()
            root.controller.goToNextEpisode()
        }
    }

    // ── skip offers, stacked above the dock ──────────────────────────
    Column {
        id: skipOffers

        anchors.right: dock.right
        anchors.bottom: dock.top
        anchors.bottomMargin: 8
        spacing: 8
        visible: root.chromeVisible

        Repeater {
            model: root.controller.skipOffers

            delegate: PlayerButton {
                id: skipButton
                controller: root.controller

                required property var model
                required property int index

                visible: !model.heading && model.enabled
                width: Math.min(implicitWidth, root.dockWidth)
                text: model.label !== "" ? model.label : qsTr("Skip segment")
                toolTip: model.tooltip !== "" ? model.tooltip : text
                Accessible.name: toolTip
                onClicked: {
                    root.controller.noteUserActivity()
                    root.controller.activateSkipOffer(index)
                }
            }
        }
    }

    // ── dock ─────────────────────────────────────────────────────────
    PlayerPanel {
        id: dock

        controller: root.controller
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: root.dockWidth
        height: root.compactLayout ? 136 : 104
        padding: 12

        ColumnLayout {
            anchors.fill: parent
            spacing: 4

            // Full action row at desktop widths.
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                visible: !root.compactLayout

                VolumeControl {
                    id: volume
                    controller: root.controller
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: root.width < 900 ? 132 : 180
                }

                Item { Layout.fillWidth: true }

                PlayerButton {
                    id: playButton
                    controller: root.controller
                    objectName: "player.playPause"
                    iconName: root.controller.playing ? "pause" : "play"
                    toolTip: root.controller.playing ? qsTr("Pause (Space)") : qsTr("Play (Space)")
                    Accessible.name: toolTip
                    enabled: root.controller.hasMedia
                    compact: false
                    implicitWidth: 48
                    implicitHeight: 48
                    onClicked: {
                        root.controller.noteUserActivity()
                        root.controller.togglePlayPause()
                    }
                }

                PlayerButton {
                    id: skipForwardButton
                    controller: root.controller
                    text: "+85"
                    toolTip: qsTr("Skip forward 85 seconds")
                    Accessible.name: toolTip
                    enabled: root.controller.hasMedia
                    compact: true
                    implicitWidth: 40
                    implicitHeight: 40
                    onClicked: {
                        root.controller.noteUserActivity()
                        root.controller.skipForward()
                    }
                }

                Item { Layout.fillWidth: true }

                RowLayout {
                    spacing: 4

                    PlayerButton {
                        id: pipButton
                        controller: root.controller
                        iconName: "pip"
                        toolTip: qsTr("Picture in picture")
                        Accessible.name: toolTip
                        compact: true
                        implicitWidth: 40
                        implicitHeight: 40
                        onClicked: {
                            root.controller.noteUserActivity()
                            root.controller.togglePip()
                        }
                    }

                    PlayerButton {
                        id: windowModeButton
                        controller: root.controller
                        iconName: root.controller.fullscreen ? "contract" : "expand"
                        toolTip: root.controller.fullscreen ? qsTr("Windowed mode (F)")
                                                            : qsTr("Fullscreen (F)")
                        Accessible.name: toolTip
                        compact: true
                        implicitWidth: 40
                        implicitHeight: 40
                        onClicked: {
                            root.controller.noteUserActivity()
                            root.controller.toggleFullscreen()
                        }
                    }

                    PlayerButton {
                        id: audioButton
                        controller: root.controller
                        iconName: "microphone"
                        toolTip: qsTr("Audio - choose a track (T)")
                        Accessible.name: toolTip
                        compact: true
                        implicitWidth: 40
                        implicitHeight: 40
                        visible: root.controller.localMedia || root.controller.audioTracksAvailable
                        enabled: root.controller.localMedia || root.controller.audioTracksAvailable
                        onClicked: {
                            root.controller.noteUserActivity()
                            root.controller.openAudioMenu()
                        }
                    }

                    PlayerButton {
                        id: subtitlesButton
                        controller: root.controller
                        iconName: "captions"
                        toolTip: qsTr("Subtitles - choose a track (C)")
                        Accessible.name: toolTip
                        compact: true
                        implicitWidth: 40
                        implicitHeight: 40
                        visible: root.controller.localMedia || root.controller.subtitleTracksAvailable
                        enabled: root.controller.localMedia || root.controller.subtitleTracksAvailable
                        onClicked: {
                            root.controller.noteUserActivity()
                            root.controller.openSubtitlesMenu()
                        }
                    }

                    PlayerButton {
                        id: clipButton
                        controller: root.controller
                        iconName: "clapperboard"
                        toolTip: qsTr("Cut a short clip from this video")
                        Accessible.name: toolTip
                        compact: true
                        implicitWidth: 40
                        implicitHeight: 40
                        visible: root.width >= 780
                        enabled: root.controller.hasVideo
                        onClicked: {
                            root.controller.noteUserActivity()
                            root.controller.openClipDialog()
                        }
                    }

                    PlayerButton {
                        id: enhanceButton
                        controller: root.controller
                        objectName: "player.enhancements.button"
                        iconName: "spark"
                        toolTip: root.controller.enhancementChipTooltip !== ""
                                 ? root.controller.enhancementChipTooltip
                                 : qsTr("Open enhancements")
                        Accessible.name: toolTip
                        compact: true
                        implicitWidth: 40
                        implicitHeight: 40
                        onClicked: root.requestMenu(enhanceButton, "enhancements")
                    }

                    PlayerButton {
                        id: settingsButton
                        controller: root.controller
                        objectName: "player.settings.button"
                        iconName: "gear"
                        toolTip: qsTr("Quality and speed (Q)")
                        Accessible.name: toolTip
                        compact: true
                        implicitWidth: 40
                        implicitHeight: 40
                        onClicked: root.requestMenu(settingsButton, "settings")
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                visible: root.compactLayout

                VolumeControl {
                    id: compactVolume
                    controller: root.controller
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: 104
                    Layout.minimumWidth: 96
                    Layout.maximumWidth: 124
                }

                Item { Layout.fillWidth: true }

                PlayerButton {
                    controller: root.controller
                    iconName: root.controller.playing ? "pause" : "play"
                    toolTip: root.controller.playing ? qsTr("Pause (Space)") : qsTr("Play (Space)")
                    enabled: root.controller.hasMedia
                    compact: true
                    objectName: "player.playPause.compact"
                    implicitWidth: 40
                    implicitHeight: 36
                    onClicked: {
                        root.controller.noteUserActivity()
                        root.controller.togglePlayPause()
                    }
                }

                PlayerButton {
                    controller: root.controller
                    text: "+85"
                    toolTip: qsTr("Skip forward 85 seconds")
                    enabled: root.controller.hasMedia
                    compact: true
                    implicitWidth: 40
                    implicitHeight: 36
                    onClicked: {
                        root.controller.noteUserActivity()
                        root.controller.skipForward()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 3
                visible: root.compactLayout

                PlayerButton {
                    controller: root.controller
                    iconName: "pip"
                    toolTip: qsTr("Picture in picture")
                    compact: true
                    implicitWidth: 34
                    implicitHeight: 34
                    onClicked: {
                        root.controller.noteUserActivity()
                        root.controller.togglePip()
                    }
                }

                PlayerButton {
                    controller: root.controller
                    iconName: root.controller.fullscreen ? "contract" : "expand"
                    toolTip: root.controller.fullscreen ? qsTr("Windowed mode (F)")
                                                        : qsTr("Fullscreen (F)")
                    compact: true
                    implicitWidth: 34
                    implicitHeight: 34
                    onClicked: {
                        root.controller.noteUserActivity()
                        root.controller.toggleFullscreen()
                    }
                }

                PlayerButton {
                    controller: root.controller
                    iconName: "microphone"
                    toolTip: qsTr("Audio - choose a track (T)")
                    compact: true
                    implicitWidth: 34
                    implicitHeight: 34
                    visible: root.controller.localMedia || root.controller.audioTracksAvailable
                    enabled: visible
                    onClicked: {
                        root.controller.noteUserActivity()
                        root.controller.openAudioMenu()
                    }
                }

                PlayerButton {
                    controller: root.controller
                    iconName: "captions"
                    toolTip: qsTr("Subtitles - choose a track (C)")
                    compact: true
                    implicitWidth: 34
                    implicitHeight: 34
                    visible: root.controller.localMedia || root.controller.subtitleTracksAvailable
                    enabled: visible
                    onClicked: {
                        root.controller.noteUserActivity()
                        root.controller.openSubtitlesMenu()
                    }
                }

                Item { Layout.fillWidth: true }

                PlayerButton {
                    id: compactEnhanceButton
                    controller: root.controller
                    objectName: "player.enhancements.button.compact"
                    iconName: "spark"
                    toolTip: root.controller.enhancementChipTooltip !== ""
                             ? root.controller.enhancementChipTooltip
                             : qsTr("Open enhancements")
                    compact: true
                    implicitWidth: 34
                    implicitHeight: 34
                    onClicked: root.requestMenu(compactEnhanceButton, "enhancements")
                }

                PlayerButton {
                    id: compactSettingsButton
                    controller: root.controller
                    objectName: "player.settings.button.compact"
                    iconName: "gear"
                    toolTip: qsTr("Quality and speed (Q)")
                    compact: true
                    implicitWidth: 34
                    implicitHeight: 34
                    onClicked: root.requestMenu(compactSettingsButton, "settings")
                }
            }

            // Timeline row: elapsed, the rail, then the remaining time.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    id: elapsedLabel
                    Layout.preferredWidth: 46
                    text: root.controller.formatTime(root.controller.position)
                    color: root.labelColor
                    opacity: 0.92
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignLeft
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Elapsed time")
                }

                Timeline {
                    id: timeline
                    controller: root.controller
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                }

                Label {
                    id: remainingLabel
                    Layout.preferredWidth: 56
                    text: root.controller.formatRemaining(root.controller.position)
                    color: root.mutedLabelColor
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignRight
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Remaining time")
                }
            }
        }
    }
}

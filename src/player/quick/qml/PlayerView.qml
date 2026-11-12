pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Mikai.Player

// The player scene. One video item, one chrome stack, one overlay stack, one
// sheet stack, all in this scene: the controller owns every piece of playback
// state, this file only places them and forwards unconsumed input.
Item {
    id: root

    required property PlayerController controller

    focus: true
    Accessible.role: Accessible.Pane
    Accessible.name: controller.windowTitle

    readonly property bool pipMode: controller.pipActive
    readonly property bool chromeVisible: !pipMode
                                          && controller.chromeState
                                             !== PlayerController.FullscreenAutoHidden
    readonly property Item timeline: controls.timelineItem
    readonly property real timelineTop: timeline ? timeline.mapToItem(root, 0, 0).y
                                                  : height - 140

    /// Every hold the single C++ idle timer must see: dock hover and scrubbing,
    /// the menu stack, the sheets and the shortcut card, the hover card, a
    /// drop in progress.
    readonly property bool interactionHold: controls.interactionActive
                                            || pipControls.interactionActive
                                            || overlay.anyMenuVisible
                                            || overlay.interactionActive
                                            || dialogs.modalVisible
                                            || dialogs.interactionActive
                                            || shortcutSheet.visible
                                            || hoverPreview.shown
                                            || dropArea.containsDrag

    readonly property bool loadingVisible: controller.openingLoading
                                           || controller.compareLoading

    onInteractionHoldChanged: controller.setInteractionActive(interactionHold)

    function openMenuAt(menuId, anchorRect) {
        if (!menuId || menuId.length === 0) return
        // Transport controls publish scene-space anchors; OverlayRoot positions
        // panels in this root's coordinates, which may differ when embedded.
        const topLeft = root.mapFromItem(null, anchorRect.x, anchorRect.y)
        const bottomRight = root.mapFromItem(null, anchorRect.x + anchorRect.width,
                                             anchorRect.y + anchorRect.height)
        controller.noteUserActivity()
        overlay.openMenu(menuId, Qt.rect(topLeft.x, topLeft.y,
                                         bottomRight.x - topLeft.x,
                                         bottomRight.y - topLeft.y))
    }

    /// Escape: sheet, menu stack, shortcut card, fullscreen, then the one C++
    /// dispatch chain. Any other key goes straight to that chain once, and is
    /// swallowed while a modal, a menu or the shortcut card owns the focus.
    function routeKey(event) {
        controller.noteUserActivity()
        if (event.key === Qt.Key_Escape) {
            if (dialogs.dismissTop()) {
                event.accepted = true
                return
            }
            if (overlay.dismissAll()) {
                event.accepted = true
                return
            }
            if (shortcutSheet.visible) {
                shortcutSheet.close()
                event.accepted = true
                return
            }
            if (controller.fullscreen) {
                controller.toggleFullscreen()
                event.accepted = true
                return
            }
            event.accepted = controller.handleKey(event.key, event.modifiers)
            return
        }
        if (dialogs.modalVisible || overlay.anyMenuVisible || shortcutSheet.visible) {
            event.accepted = true
            return
        }
        event.accepted = controller.handleKey(event.key, event.modifiers)
    }

    Component.onCompleted: {
        controller.setInteractionActive(interactionHold)
        controller.setMenuStackVisible(overlay.anyMenuVisible)
        forceActiveFocus()
    }

    Keys.onPressed: event => routeKey(event)

    // ── video ──────────────────────────────────────────────────────────
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        z: -1
    }

    MpvVideoItem {
        id: video
        objectName: "playerVideo"
        anchors.fill: parent
        session: root.controller.session
        z: 0
    }

    // Picture input sits under the chrome, so a click reaches a control first.
    MouseArea {
        id: pictureInput
        anchors.fill: parent
        z: 1
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: true
        property bool suppressClick: false

        onPressed: mouse => root.controller.noteUserActivity()

        onClicked: mouse => {
            if (mouse.button === Qt.RightButton) {
                const scenePoint = pictureInput.mapToItem(null, mouse.x, mouse.y)
                root.openMenuAt("picture", Qt.rect(scenePoint.x, scenePoint.y, 0, 0))
                return
            }
            if (suppressClick) {
                suppressClick = false
                return
            }
            root.forceActiveFocus()
            root.controller.togglePlayPause()
        }

        onDoubleClicked: mouse => {
            if (mouse.button !== Qt.LeftButton) return
            suppressClick = true
            root.forceActiveFocus()
            root.controller.toggleFullscreen()
        }
    }

    // ── empty / loading ────────────────────────────────────────────────
    Item {
        id: emptyState
        visible: root.chromeVisible && !root.controller.hasMedia
        anchors.centerIn: parent
        width: Math.min(root.width - 48, 420)
        height: emptyColumn.implicitHeight
        z: 2

        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("No media loaded. Drop a file to begin.")

        Column {
            id: emptyColumn
            anchors.fill: parent
            spacing: 14

            PlayerIcon {
                anchors.horizontalCenter: parent.horizontalCenter
                controller: root.controller
                name: "clapperboard"
                tint: root.controller.accentColor
                width: 40
                height: 40
            }

            Label {
                width: parent.width
                text: qsTr("No media loaded — drop a file to begin")
                color: root.controller.darkTheme ? "#f4f4f5" : "#18181b"
                font.pixelSize: 14
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Label {
                width: parent.width
                text: qsTr("Any local video opens here; subtitle and audio files attach to the current title.")
                color: root.controller.darkTheme ? "#a1a1aa" : "#52525b"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }

    Rectangle {
        id: loadingScrim
        visible: root.loadingVisible
        anchors.fill: parent
        color: root.controller.darkTheme ? Qt.rgba(0, 0, 0, 0.58) : Qt.rgba(0, 0, 0, 0.35)
        z: 3

        Accessible.role: Accessible.StaticText
        Accessible.name: loadingHeadline.text

        Column {
            anchors.centerIn: parent
            spacing: 14

            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                running: root.loadingVisible
                implicitWidth: 36
                implicitHeight: 36
            }

            Label {
                id: loadingHeadline
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(root.width - 64, 460)
                text: root.controller.openingLoading
                      ? root.controller.openingHeadline
                      : qsTr("Preparing comparison")
                color: "#f4f4f5"
                font.pixelSize: 14
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                elide: Text.ElideRight
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(root.width - 64, 460)
                visible: text !== ""
                text: root.controller.openingLoading ? root.controller.openingDetail : ""
                color: "#d4d4d8"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }

    // ── information chip / native HUD ─────────────────────────────────
    Rectangle {
        id: infoChip
        visible: root.chromeVisible && !root.pipMode
                 && (root.controller.episodeLabel !== ""
                     || root.controller.videoBadges.length > 0
                     || root.controller.sourceWidth > 0
                     || upscaleLabel.upscaleText !== "")
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 16
        width: Math.min(root.width - 32, infoColumn.implicitWidth + 20)
        height: infoColumn.implicitHeight + 16
        radius: Math.max(2, root.controller.cornerRadius)
        color: root.controller.darkTheme ? Qt.rgba(0.07, 0.08, 0.10, 0.82)
                                         : Qt.rgba(0.98, 0.98, 0.99, 0.9)
        border.width: 1
        border.color: root.controller.darkTheme ? Qt.rgba(1, 1, 1, 0.14)
                                                : Qt.rgba(0, 0, 0, 0.12)
        z: 12

        Accessible.role: Accessible.StaticText
        Accessible.name: {
            const parts = []
            if (root.controller.episodeLabel !== "") parts.push(root.controller.episodeLabel)
            for (let i = 0; i < root.controller.videoBadges.length; ++i)
                parts.push(root.controller.videoBadges[i])
            if (root.controller.sourceWidth > 0)
                parts.push(root.controller.sourceWidth + " by " + root.controller.sourceHeight)
            return parts.join(", ")
        }

        Column {
            id: infoColumn
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            Item {
                id: episodeTitleItem
                visible: root.controller.episodeLabel !== ""
                width: episodeTitleRow.implicitWidth
                height: episodeTitleRow.implicitHeight

                Row {
                    id: episodeTitleRow
                    spacing: 4
                    anchors.fill: parent

                    Label {
                        text: root.controller.episodeLabel
                        color: root.controller.darkTheme ? "#f4f4f5" : "#18181b"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }

                    Text {
                        visible: !root.controller.directMedia
                        text: "\u203A"
                        color: episodeArea.containsMouse ? root.controller.accentColor : (root.controller.darkTheme ? "#a1a1aa" : "#71717a")
                        font.pixelSize: 15
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                MouseArea {
                    id: episodeArea
                    anchors.fill: parent
                    enabled: !root.controller.directMedia
                    hoverEnabled: true
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: {
                        root.controller.noteUserActivity()
                        root.controller.toggleSeries()
                    }
                }
            }

            Row {
                spacing: 6

                Repeater {
                    model: root.controller.videoBadges

                    delegate: Rectangle {
                        required property var modelData

                        height: 18
                        width: badgeLabel.implicitWidth + 12
                        radius: height / 2
                        color: root.controller.darkTheme ? Qt.rgba(1, 1, 1, 0.08)
                                                        : Qt.rgba(0, 0, 0, 0.06)

                        Label {
                            id: badgeLabel
                            anchors.centerIn: parent
                            text: modelData
                            color: root.controller.darkTheme ? "#d4d4d8" : "#3f3f46"
                            font.pixelSize: 10
                            font.letterSpacing: 0.4
                        }
                    }
                }

                Label {
                    visible: root.controller.sourceWidth > 0
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.controller.sourceWidth + "×" + root.controller.sourceHeight
                    color: root.controller.darkTheme ? "#a1a1aa" : "#52525b"
                    font.pixelSize: 10
                    font.features: ({ "tnum": 1 })
                }
            }

            Label {
                id: upscaleLabel
                readonly property string upscaleText: {
                    if (root.controller.animeJanaiChipText !== "")
                        return root.controller.animeJanaiChipText
                    if (root.controller.enhancementChipText !== "")
                        return root.controller.enhancementChipText
                    return ""
                }
                visible: upscaleText !== ""
                text: upscaleText
                color: root.controller.accentColor
                font.pixelSize: 10
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                width: Math.min(root.width - 52, implicitWidth)
            }
        }
    }

    Row {
        id: windowControls
        visible: root.chromeVisible && !root.pipMode
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 4
        z: 15

        PlayerButton {
            controller: root.controller
            iconName: "minimize"
            toolTip: qsTr("Minimize")
            compact: true
            implicitWidth: 32
            implicitHeight: 32
            onClicked: {
                root.controller.noteUserActivity()
                root.controller.minimizeWindow()
            }
        }

        PlayerButton {
            controller: root.controller
            iconName: root.controller.fullscreen ? "restore" : "expand"
            toolTip: root.controller.fullscreen ? qsTr("Exit fullscreen (F11)") : qsTr("Fullscreen (F11)")
            compact: true
            implicitWidth: 32
            implicitHeight: 32
            onClicked: {
                root.controller.noteUserActivity()
                root.controller.toggleFullscreen()
            }
        }

        PlayerButton {
            controller: root.controller
            iconName: "close"
            toolTip: qsTr("Close (Esc)")
            compact: true
            implicitWidth: 32
            implicitHeight: 32
            onClicked: {
                root.controller.noteUserActivity()
                root.controller.requestClose()
            }
        }
    }

    Rectangle {
        id: nativeHud
        visible: !root.pipMode && root.controller.nativeHudVisible
        anchors.right: parent.right
        anchors.top: windowControls.visible ? windowControls.bottom : parent.top
        anchors.margins: 12
        width: Math.min(root.width - 32, 320)
        height: hudColumn.implicitHeight + 16
        radius: Math.max(2, root.controller.cornerRadius)
        color: root.controller.darkTheme ? Qt.rgba(0.07, 0.08, 0.10, 0.82)
                                         : Qt.rgba(0.98, 0.98, 0.99, 0.9)
        border.width: 1
        border.color: root.controller.nativeHudActive
                      ? Qt.rgba(root.controller.accentColor.r, root.controller.accentColor.g,
                                root.controller.accentColor.b, 0.5)
                      : (root.controller.darkTheme ? Qt.rgba(1, 1, 1, 0.14)
                                                   : Qt.rgba(0, 0, 0, 0.12))
        z: 12

        Accessible.role: Accessible.StaticText
        Accessible.name: root.controller.nativeHudHeadline

        Column {
            id: hudColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Label {
                width: parent.width
                text: root.controller.nativeHudHeadline
                color: root.controller.darkTheme ? "#f4f4f5" : "#18181b"
                font.pixelSize: 12
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            Label {
                width: parent.width
                visible: text !== ""
                text: [root.controller.nativeHudProvider,
                       root.controller.nativeHudModel,
                       root.controller.nativeHudGeometry].filter(part => part !== "").join(" · ")
                color: root.controller.darkTheme ? "#a1a1aa" : "#52525b"
                font.pixelSize: 10
                elide: Text.ElideRight
            }

            Label {
                width: parent.width
                visible: text !== ""
                text: [root.controller.nativeHudState,
                       root.controller.nativeHudFps,
                       root.controller.nativeHudInterpolation,
                       root.controller.nativeHudDropped !== ""
                           ? qsTr("%1 dropped").arg(root.controller.nativeHudDropped) : ""]
                      .filter(part => part !== "").join(" · ")
                color: root.controller.darkTheme ? "#a1a1aa" : "#52525b"
                font.pixelSize: 10
                elide: Text.ElideRight
            }

            Label {
                width: parent.width
                visible: text !== ""
                text: root.controller.nativeHudReason
                color: root.controller.nativeHudActive
                       ? (root.controller.darkTheme ? "#d4d4d8" : "#3f3f46")
                       : "#e0a458"
                font.pixelSize: 10
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }
        }
    }

    // ── status line ───────────────────────────────────────────────────
    Rectangle {
        id: statusChip
        visible: root.chromeVisible && root.controller.statusText !== ""
                 && root.controller.statusMode !== PlayerController.None
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 16
        width: Math.min(root.width - 32, statusRow.implicitWidth + 28)
        height: Math.max(30, statusRow.implicitHeight + 12)
        radius: height / 2
        color: root.controller.darkTheme ? Qt.rgba(0.07, 0.08, 0.10, 0.9)
                                         : Qt.rgba(0.98, 0.98, 0.99, 0.95)
        border.width: 1
        border.color: {
            if (root.controller.statusMode === PlayerController.Error)
                return Qt.rgba(0.88, 0.28, 0.28, 0.6)
            if (root.controller.statusMode === PlayerController.Processing)
                return Qt.rgba(root.controller.accentColor.r, root.controller.accentColor.g,
                               root.controller.accentColor.b, 0.5)
            return root.controller.darkTheme ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.12)
        }
        z: 13

        Accessible.role: root.controller.statusMode === PlayerController.Error
                         ? Accessible.AlertMessage : Accessible.StaticText
        Accessible.name: root.controller.statusText

        RowLayout {
            id: statusRow
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 6
            spacing: 8

            BusyIndicator {
                visible: root.controller.statusMode === PlayerController.RuntimePending
                         || root.controller.statusMode === PlayerController.Processing
                running: visible
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
            }

            Label {
                Layout.fillWidth: true
                text: root.controller.statusText
                color: root.controller.statusMode === PlayerController.Error
                       ? (root.controller.darkTheme ? "#fca5a5" : "#b91c1c")
                       : (root.controller.darkTheme ? "#f4f4f5" : "#18181b")
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            PlayerButton {
                visible: root.controller.retryVisible
                controller: root.controller
                text: qsTr("Retry")
                iconName: "reset"
                toolTip: qsTr("Retry loading this stream")
                compact: true
                onClicked: root.controller.retryPlayback()
            }
        }
    }

    // ── notification toast ────────────────────────────────────────────
    Rectangle {
        id: toast
        visible: root.controller.notificationVisible
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(16, root.timelineTop - height - 12)
        width: Math.min(root.width - 32, toastLabel.implicitWidth + 28)
        height: Math.max(32, toastLabel.implicitHeight + 14)
        radius: Math.max(2, root.controller.cornerRadius)
        color: root.controller.notificationFailure
               ? (root.controller.darkTheme ? Qt.rgba(0.36, 0.10, 0.10, 0.95)
                                            : Qt.rgba(0.99, 0.94, 0.94, 0.98))
               : (root.controller.darkTheme ? Qt.rgba(0.07, 0.08, 0.10, 0.95)
                                            : Qt.rgba(0.98, 0.98, 0.99, 0.98))
        border.width: 1
        border.color: root.controller.notificationFailure
                      ? Qt.rgba(0.88, 0.28, 0.28, 0.65)
                      : (root.controller.darkTheme ? Qt.rgba(1, 1, 1, 0.16)
                                                   : Qt.rgba(0, 0, 0, 0.14))
        z: 14

        Accessible.role: Accessible.AlertMessage
        Accessible.name: root.controller.notificationText

        Label {
            id: toastLabel
            anchors.centerIn: parent
            width: Math.min(root.width - 48, implicitWidth)
            text: root.controller.notificationText
            color: root.controller.notificationFailure
                   ? (root.controller.darkTheme ? "#fecaca" : "#7f1d1d")
                   : (root.controller.darkTheme ? "#f4f4f5" : "#18181b")
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
    }

    // ── chrome (exactly one of them is live: same window and session) ──
    PlayerControls {
        id: controls
        controller: root.controller
        anchors.fill: parent
        visible: root.chromeVisible
        z: 10
        onMenuRequested: (menuId, anchorRect) => root.openMenuAt(menuId, anchorRect)
    }

    PipControls {
        id: pipControls
        controller: root.controller
        anchors.fill: parent
        visible: root.pipMode
        z: 10
        onMenuRequested: (menuId, anchorRect) => root.openMenuAt(menuId, anchorRect)
    }

    // ── overlays, sheets, cards ───────────────────────────────────────
    Connections {
        target: root.timeline
        ignoreUnknownSignals: true

        function onPreviewRequested(seconds, anchorPoint) {
            hoverPreview.anchor = anchorPoint
            root.controller.noteUserActivity()
            root.controller.showHoverPreview(seconds)
        }

        function onPreviewDismissed() {
            root.controller.hideHoverPreview()
        }
    }

    HoverPreview {
        id: hoverPreview
        controller: root.controller
        timelineItem: root.timeline
        z: 15
    }

    OverlayRoot {
        id: overlay
        controller: root.controller
        anchors.fill: parent
        z: 20
    }

    Connections {
        target: overlay
        function onAnyMenuVisibleChanged() {
            root.controller.setMenuStackVisible(overlay.anyMenuVisible)
        }
    }

    ShortcutSheet {
        id: shortcutSheet
        controller: root.controller
        z: 25
    }

    PlayerDialogs {
        id: dialogs
        controller: root.controller
        z: 30
    }

    // ── drop target ───────────────────────────────────────────────────
    function acceptedLocalUrls(urls) {
        const accepted = []
        for (let i = 0; i < urls.length; ++i) {
            const url = urls[i]
            if (url.scheme.toLowerCase() === "file") accepted.push(url)
        }
        return accepted
    }

    DropArea {
        id: dropArea
        anchors.fill: parent
        z: 40

        onEntered: root.controller.noteUserActivity()
        onPositionChanged: root.controller.noteUserActivity()
        onDropped: drop => {
            const urls = root.acceptedLocalUrls(drop.urls)
            if (urls.length === 0) return
            drop.acceptProposedAction()
            root.controller.noteUserActivity()
            root.controller.handleDroppedUrls(urls)
        }
    }

    Rectangle {
        id: dropIndicator
        visible: dropArea.containsDrag
        anchors.fill: parent
        anchors.margins: 10
        color: Qt.rgba(root.controller.accentColor.r, root.controller.accentColor.g,
                       root.controller.accentColor.b, 0.12)
        border.width: 2
        border.color: Qt.rgba(root.controller.accentColor.r, root.controller.accentColor.g,
                              root.controller.accentColor.b, 0.7)
        radius: Math.max(2, root.controller.cornerRadius)
        z: 41

        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("Drop a video, subtitle or audio file")

        Label {
            anchors.centerIn: parent
            text: qsTr("Drop a video, subtitle or audio file")
            color: root.controller.darkTheme ? "#f4f4f5" : "#18181b"
            font.pixelSize: 15
            font.weight: Font.DemiBold
        }
    }

    // ── accessibility announcements ───────────────────────────────────
    Item {
        id: announcer
        property string message: ""

        Accessible.role: Accessible.AlertMessage
        Accessible.name: message

        function announce(text) {
            message = text
            Accessible.announce(text)
        }
    }

    Connections {
        target: root.controller

        function onShortcutsSheetRequested() {
            shortcutSheet.open()
        }

        function onPictureFocusRequested() {
            overlay.dismissAll()
            root.forceActiveFocus()
        }

        function onChromeWakeRequested() {
            overlay.dismissAll()
        }

        function onAlertRequested(message) {
            announcer.announce(message)
        }
    }
}

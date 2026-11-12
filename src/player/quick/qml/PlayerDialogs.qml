pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Mikai.Player

// Ordinary in-scene player sheets: resume, processing remedies and the
// OpenSubtitles search. The controller owns every state and command; this item
// owns only focus, scroll position and which sheet is on top.
Item {
    id: root

    required property PlayerController controller

    readonly property color textColor: controller.darkTheme ? "#f4f4f5" : "#18181b"
    readonly property color mutedColor: controller.darkTheme ? "#a1a1aa" : "#52525b"
    readonly property color borderColor: controller.darkTheme ? Qt.rgba(1, 1, 1, 0.14)
                                                              : Qt.rgba(0, 0, 0, 0.14)
    readonly property color fieldColor: controller.darkTheme ? Qt.rgba(1, 1, 1, 0.06)
                                                             : Qt.rgba(0, 0, 0, 0.05)
    readonly property color scrimColor: Qt.rgba(0, 0, 0, controller.darkTheme ? 0.6 : 0.4)
    readonly property color accentWash: Qt.rgba(controller.accentColor.r, controller.accentColor.g,
                                                controller.accentColor.b, 0.18)
    readonly property int cardRadius: Math.max(2, controller.cornerRadius)

    readonly property bool resumeVisible: controller.resumeSheetVisible
    readonly property bool searchVisible: controller.subtitleSearchVisible
    readonly property bool remediesVisible: controller.processingActive
    /// True while a sheet blocks the player behind it.
    readonly property bool modalVisible: resumeVisible || searchVisible
    /// Aggregate hold this item reports to the view glue's interaction state.
    readonly property bool interactionActive: modalVisible || remediesVisible

    readonly property bool canSearch: !controller.subtitleSearchBusy
                                      && !controller.subtitleDownloadBusy
    readonly property bool canDownload: canSearch && resultsList.count > 0
                                         && resultsList.currentIndex >= 0
    readonly property string remedyMessage: controller.processingMessage !== ""
                                            ? controller.processingMessage
                                            : controller.processingReason

    /// Canonical provider set: the system's two-letter code first, then the
    /// fixed list, each once. Selection comes from the controller property.
    readonly property var subtitleLanguages: {
        const system = Qt.locale().name.substring(0, 2).toLowerCase()
        const ordered = [system, "uk", "en", "ru", "pl", "de", "es"]
        const codes = []
        for (let i = 0; i < ordered.length; ++i) {
            if (ordered[i].length !== 2 || codes.indexOf(ordered[i]) >= 0) continue
            codes.push(ordered[i])
        }
        const current = controller.subtitleSearchLanguage
        if (current !== "" && codes.indexOf(current) < 0) codes.push(current)
        const out = []
        for (let i = 0; i < codes.length; ++i)
            out.push({ code: codes[i], name: Qt.locale(codes[i]).nativeLanguageName })
        return out
    }

    property Item previousFocus: null

    anchors.fill: parent

    /// Escape priority as the controller declares it: resume, then the search
    /// sheet. Remedies stay until one is chosen, so they are not dismissible.
    function dismissTop() {
        if (resumeVisible) {
            controller.answerResume(controller.resumeSheetGeneration,
                                    PlayerController.Dismiss)
            return true
        }
        if (searchVisible) {
            controller.closeSubtitleSearch()
            return true
        }
        return false
    }

    function chooseLanguage(code) {
        if (code === "" || code === controller.subtitleSearchLanguage) return
        controller.setSubtitleSearchLanguage(code)
    }

    function downloadCurrent() {
        if (!canDownload) return
        controller.downloadSubtitle(resultsList.currentIndex)
    }

    onModalVisibleChanged: {
        if (modalVisible) {
            if (previousFocus === null && Window.window)
                previousFocus = Window.window.activeFocusItem
        } else {
            const restore = previousFocus
            previousFocus = null
            if (restore && restore.visible) restore.forceActiveFocus()
        }
    }

    Connections {
        target: root.controller
        function onResumeSheetAttentionRequested(visible) {
            if (visible && root.resumeVisible) continueButton.forceActiveFocus()
        }
    }

    // ── resume ─────────────────────────────────────────────────────
    FocusScope {
        id: resumeLayer
        anchors.fill: parent
        visible: root.resumeVisible
        enabled: visible
        z: 30
        focus: visible

        onVisibleChanged: {
            if (visible) Qt.callLater(() => continueButton.forceActiveFocus())
        }

        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Continue playing")

        Keys.onEscapePressed: event => {
            root.dismissTop()
            event.accepted = true
        }
        // A key the sheet does not use must not reach the player behind it.
        Keys.onPressed: event => {
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
            id: resumePanel
            controller: root.controller
            title: qsTr("Continue playing")
            padding: 20
            anchors.centerIn: parent
            width: Math.min(Math.max(280, root.width - 32), 480)

            ColumnLayout {
                width: parent.width
                spacing: 16

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14

                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 44
                        Layout.preferredHeight: 44
                        Layout.maximumWidth: 44
                        Layout.maximumHeight: 44
                        radius: root.cardRadius + 2
                        color: root.accentWash

                        PlayerIcon {
                            anchors.centerIn: parent
                            controller: root.controller
                            name: "play"
                            tint: root.controller.accentColor
                            sourceSize: 16
                            width: 16
                            height: 16
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Continue from %1?").arg(root.controller.resumeSheetLabel)
                            color: root.textColor
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("You stopped this episode at %1.")
                                  .arg(root.controller.resumeSheetLabel)
                            color: root.mutedColor
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    layoutDirection: Qt.RightToLeft
                    spacing: 8

                    PlayerButton {
                        id: continueButton
                        controller: root.controller
                        text: qsTr("Continue")
                        toolTip: qsTr("Resume from %1").arg(root.controller.resumeSheetLabel)
                        KeyNavigation.tab: startOverButton
                        KeyNavigation.backtab: dismissButton
                        onClicked: root.controller.answerResume(
                                       root.controller.resumeSheetGeneration,
                                       PlayerController.Continue)
                    }

                    PlayerButton {
                        id: startOverButton
                        controller: root.controller
                        text: qsTr("Start over")
                        toolTip: qsTr("Play this episode from the beginning")
                        KeyNavigation.tab: dismissButton
                        KeyNavigation.backtab: continueButton
                        onClicked: root.controller.answerResume(
                                       root.controller.resumeSheetGeneration,
                                       PlayerController.StartOver)
                    }

                    PlayerButton {
                        id: dismissButton
                        controller: root.controller
                        iconName: "close"
                        toolTip: qsTr("Keep playing from the beginning")
                        KeyNavigation.tab: continueButton
                        KeyNavigation.backtab: startOverButton
                        onClicked: root.controller.answerResume(
                                       root.controller.resumeSheetGeneration,
                                       PlayerController.Dismiss)
                    }
                }
            }
        }
    }

    // ── OpenSubtitles ──────────────────────────────────────────────
    FocusScope {
        id: searchLayer
        anchors.fill: parent
        visible: root.searchVisible
        enabled: visible
        z: 20
        focus: visible

        onVisibleChanged: {
            if (visible) Qt.callLater(() => queryField.forceActiveFocus())
        }

        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("OpenSubtitles")

        Keys.onEscapePressed: event => {
            root.dismissTop()
            event.accepted = true
        }
        // A key the sheet does not use must not reach the player behind it.
        Keys.onPressed: event => {
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
            id: searchPanel
            controller: root.controller
            title: qsTr("OpenSubtitles")
            padding: 20
            anchors.centerIn: parent
            width: Math.min(Math.max(0, root.width - 32), 520)
            height: Math.min(Math.max(0, root.height - 32), implicitHeight)
            clip: true

            ColumnLayout {
                width: parent.width
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Search OpenSubtitles by title or release name.")
                    color: root.mutedColor
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }

                TextField {
                    id: queryField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Title or release name")
                    placeholderTextColor: root.mutedColor
                    color: root.textColor
                    font.pixelSize: 13
                    selectByMouse: true
                    leftPadding: 12
                    rightPadding: 12
                    topPadding: 8
                    bottomPadding: 8
                    text: root.controller.subtitleSearchQuery
                    enabled: !root.controller.subtitleSearchBusy
                    onTextEdited: root.controller.setSubtitleSearchQuery(text)

                    background: Rectangle {
                        radius: root.cardRadius
                        color: root.fieldColor
                        border.width: 1
                        border.color: queryField.activeFocus ? root.controller.accentColor
                                                             : root.borderColor
                    }

                    Keys.onTabPressed: event => {
                        const first = languageChips.itemAt(0)
                        ;(first || searchButton).forceActiveFocus()
                        event.accepted = true
                    }
                    Keys.onBacktabPressed: event => {
                        closeButton.forceActiveFocus()
                        event.accepted = true
                    }
                    Keys.onReturnPressed: event => {
                        if (root.canSearch) root.controller.searchSubtitles()
                        event.accepted = true
                    }
                    Keys.onEnterPressed: event => {
                        if (root.canSearch) root.controller.searchSubtitles()
                        event.accepted = true
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Language")
                        color: root.mutedColor
                        font.pixelSize: 11
                    }

                    Flow {
                        id: languageFlow
                        Layout.fillWidth: true
                        spacing: 6

                        Repeater {
                            id: languageChips
                            model: root.subtitleLanguages

                            delegate: PlayerButton {
                                id: languageChip
                                required property var modelData
                                required property int index
                                controller: root.controller
                                compact: true
                                checkable: true
                                text: languageChip.modelData.name
                                toolTip: qsTr("Search subtitles in %1").arg(languageChip.modelData.name)
                                enabled: !root.controller.subtitleSearchBusy
                                checked: root.controller.subtitleSearchLanguage === languageChip.modelData.code
                                onClicked: {
                                    root.chooseLanguage(languageChip.modelData.code)
                                    checked = Qt.binding(() =>
                                        root.controller.subtitleSearchLanguage === languageChip.modelData.code)
                                }
                                Keys.onTabPressed: event => {
                                    const next = languageChips.itemAt(languageChip.index + 1)
                                    ;(next || searchButton).forceActiveFocus()
                                    event.accepted = true
                                }
                                Keys.onBacktabPressed: event => {
                                    const previous = languageChips.itemAt(languageChip.index - 1)
                                    ;(previous || queryField).forceActiveFocus()
                                    event.accepted = true
                                }
                            }
                        }
                    }
                }

                Item {
                    id: resultsArea
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: Math.min(Math.max(resultsList.contentHeight, 44), 280)
                    Layout.minimumHeight: 80

                    ListView {
                        id: resultsList
                        anchors.fill: parent
                        clip: true
                        spacing: 2
                        model: root.controller.subtitleResults
                        currentIndex: -1
                        activeFocusOnTab: true
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        Keys.onTabPressed: event => {
                            downloadButton.forceActiveFocus()
                            event.accepted = true
                        }
                        Keys.onBacktabPressed: event => {
                            searchButton.forceActiveFocus()
                            event.accepted = true
                        }
                        Keys.onUpPressed: event => {
                            if (currentIndex > 0) currentIndex -= 1
                            event.accepted = true
                        }
                        Keys.onDownPressed: event => {
                            if (currentIndex < count - 1) currentIndex += 1
                            event.accepted = true
                        }
                        Keys.onReturnPressed: event => {
                            root.downloadCurrent()
                            event.accepted = true
                        }
                        Keys.onEnterPressed: event => {
                            root.downloadCurrent()
                            event.accepted = true
                        }

                    delegate: Item {
                        id: resultRow
                        required property var model
                        required property int index
                        width: resultsList.width
                        height: Math.max(44, resultColumn.implicitHeight + 16)

                        Accessible.role: Accessible.ListItem
                        Accessible.name: resultRow.model.detail !== ""
                                         ? resultRow.model.label + ", " + resultRow.model.detail
                                         : resultRow.model.label

                        Rectangle {
                            anchors.fill: parent
                            radius: Math.max(3, root.cardRadius - 2)
                            color: resultsList.currentIndex === resultRow.index || resultRow.model.selected
                                   ? root.accentWash
                                   : (resultHover.hovered ? root.fieldColor : "transparent")
                        }

                        Column {
                            id: resultColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Label {
                                width: parent.width
                                text: resultRow.model.label
                                color: root.textColor
                                font.pixelSize: 13
                                elide: Text.ElideRight
                            }

                            Label {
                                width: parent.width
                                visible: resultRow.model.detail !== ""
                                text: resultRow.model.detail
                                color: root.mutedColor
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        HoverHandler {
                            id: resultHover
                        }

                        TapHandler {
                            onTapped: resultsList.currentIndex = resultRow.index
                            onDoubleTapped: {
                                resultsList.currentIndex = resultRow.index
                                root.downloadCurrent()
                            }
                        }
                    }
                }

                    Label {
                        anchors.centerIn: parent
                        width: parent.width - 24
                        visible: resultsList.count === 0
                                 && root.controller.subtitleSearchStatus === ""
                        text: root.controller.subtitleSearchBusy
                              ? qsTr("Searching…")
                              : qsTr("No subtitle files yet.")
                        color: root.mutedColor
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: root.controller.subtitleSearchStatus
                    color: root.textColor
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    BusyIndicator {
                        running: root.controller.subtitleSearchBusy
                                 || root.controller.subtitleDownloadBusy
                        visible: running
                        Layout.preferredWidth: 20
                        Layout.preferredHeight: 20
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    PlayerButton {
                        id: searchButton
                        controller: root.controller
                        text: qsTr("Search")
                        toolTip: qsTr("Search OpenSubtitles")
                        enabled: root.canSearch
                        Keys.onTabPressed: event => {
                            resultsList.forceActiveFocus()
                            event.accepted = true
                        }
                        Keys.onBacktabPressed: event => {
                            const last = languageChips.itemAt(languageChips.count - 1)
                            ;(last || queryField).forceActiveFocus()
                            event.accepted = true
                        }
                        onClicked: root.controller.searchSubtitles()
                    }

                    PlayerButton {
                        id: downloadButton
                        controller: root.controller
                        text: qsTr("Download and attach")
                        toolTip: qsTr("Download the selected subtitle and attach it")
                        enabled: root.canDownload
                        Keys.onTabPressed: event => {
                            closeButton.forceActiveFocus()
                            event.accepted = true
                        }
                        Keys.onBacktabPressed: event => {
                            resultsList.forceActiveFocus()
                            event.accepted = true
                        }
                        onClicked: root.downloadCurrent()
                    }

                    PlayerButton {
                        id: closeButton
                        controller: root.controller
                        text: qsTr("Close")
                        toolTip: qsTr("Close the subtitle search")
                        Keys.onTabPressed: event => {
                            queryField.forceActiveFocus()
                            event.accepted = true
                        }
                        Keys.onBacktabPressed: event => {
                            downloadButton.forceActiveFocus()
                            event.accepted = true
                        }
                        onClicked: root.controller.closeSubtitleSearch()
                    }
                }
            }
        }
    }

    // ── processing remedies ────────────────────────────────────────
    // Not modal and not dismissible: the rows live until one is chosen, a new
    // source supersedes them or the player closes, exactly as the old status
    // row did. Transport stays usable underneath.
    MouseArea {
        anchors.fill: remedyPanel
        visible: remedyPanel.visible
        z: remedyPanel.z - 1
        acceptedButtons: Qt.AllButtons
        onPressed: mouse => {
            root.controller.noteUserActivity()
            mouse.accepted = true
        }
    }

    PlayerPanel {
        id: remedyPanel
        visible: root.remediesVisible
        enabled: visible
        z: 10
        controller: root.controller
        title: qsTr("Processing stopped")
        padding: 16
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.max(24, Math.min(root.height * 0.16, 140))
        width: Math.min(Math.max(0, root.width - 32), 520)

        ColumnLayout {
            width: parent.width
            spacing: 10

            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.remedyMessage
                color: root.textColor
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }

            Flow {
                Layout.fillWidth: true
                layoutDirection: Qt.RightToLeft
                spacing: 8

                Repeater {
                    model: root.controller.remedyMenu

                    delegate: PlayerButton {
                        id: remedyButton
                        required property var model
                        required property int index
                        controller: root.controller
                        visible: remedyButton.model.heading !== true
                        text: remedyButton.model.label
                        toolTip: remedyButton.model.tooltip
                        enabled: visible && remedyButton.model.enabled !== false
                        onClicked: root.controller.activateMenuRow("remedies", remedyButton.index)
                    }
                }
            }
        }
    }
}

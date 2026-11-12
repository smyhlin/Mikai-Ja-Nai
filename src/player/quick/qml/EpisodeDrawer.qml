pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

// Episode drawer: the playlist level of the series menu as a bounded side
// panel in the same scene. Picking an episode commits through the typed row
// action and closes the drawer; the current episode is the model's own mark.
PlayerPanel {
    id: drawer

    property Item sceneItem: null
    property rect anchorRect: Qt.rect(0, 0, 0, 0)

    signal closeRequested()
    signal dismissKey()

    objectName: "player.episodes.drawer"
    readonly property string menuId: "series"
    readonly property var menuModel: controller.seriesMenu
    readonly property int rowHeight: 40
    readonly property int edgeMargin: 8
    readonly property color textColor: controller.darkTheme ? "#f4f4f5" : "#101014"
    readonly property color mutedColor: controller.darkTheme ? "#a1a1aa" : "#5a5a63"
    readonly property color hoverColor: Qt.rgba(controller.accentColor.r,
                                                controller.accentColor.g,
                                                controller.accentColor.b,
                                                controller.darkTheme ? 0.18 : 0.12)
    readonly property real sceneWidth: sceneItem ? sceneItem.width : 640
    readonly property real sceneHeight: sceneItem ? sceneItem.height : 720
    readonly property real panelWidth: Math.min(360, Math.max(1, sceneWidth - edgeMargin * 2))

    x: Math.max(edgeMargin, Math.min(anchorRect.x, sceneWidth - edgeMargin - width))
    y: Math.min(anchorRect.y + anchorRect.height + 8, sceneHeight - edgeMargin - height)
    width: panelWidth
    height: Math.max(1, Math.min(400, sceneHeight - edgeMargin - anchorRect.y - anchorRect.height - 8))
    padding: 8
    z: 20

    Accessible.role: Accessible.PopupMenu
    Accessible.name: qsTr("Episodes")

    function takeFocus() {
        if (!visible) return;
        if (list.count > 0) {
            list.forceActiveFocus();
            if (list.currentIndex < 0) {
                list.currentIndex = Math.max(0, list.count - 1);
                list.positionViewAtEnd();
            }
        } else {
            drawer.forceActiveFocus();
        }
    }

    function rowIsSelectable(row) {
        return menuModel !== null && menuModel.isSelectable(row);
    }

    function moveCursor(step) {
        if (list.count <= 0) return;
        let next = list.currentIndex < 0 ? (step > 0 ? -1 : list.count) : list.currentIndex;
        next += step;
        while (next >= 0 && next < list.count) {
            if (rowIsSelectable(next)) {
                list.currentIndex = next;
                list.positionViewAtIndex(next, ListView.Contain);
                return;
            }
            next += step;
        }
    }

    function jumpCursor(forward) {
        if (list.count <= 0) return;
        let next = forward ? 0 : list.count - 1;
        while (next >= 0 && next < list.count) {
            if (rowIsSelectable(next)) {
                list.currentIndex = next;
                list.positionViewAtIndex(next, ListView.Contain);
                return;
            }
            next += forward ? 1 : -1;
        }
    }

    function activateRow(row) {
        if (row < 0) return;
        controller.activateMenuRow(menuId, row);
        drawer.closeRequested();
    }

    function activateCurrent() {
        const row = list.currentIndex;
        if (row < 0) return;
        const item = list.itemAtIndex(row);
        if (!item || !item.enabled) return;
        drawer.activateRow(row);
    }

    onVisibleChanged: if (visible) takeFocus()

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape) {
            drawer.dismissKey();
            event.accepted = true;
        }
    }

    Item {
        id: drawerHeader
        width: drawer.width - drawer.padding * 2
        height: 46

        Text {
            anchors.left: parent.left
            anchors.right: closeButton.left
            anchors.rightMargin: 8
            anchors.top: parent.top
            elide: Text.ElideRight
            text: qsTr("Episodes")
            color: drawer.textColor
            font.pixelSize: 12
            font.weight: Font.DemiBold
            font.letterSpacing: 0.6
        }

        Text {
            anchors.left: parent.left
            anchors.right: closeButton.left
            anchors.rightMargin: 8
            anchors.top: parent.top
            anchors.topMargin: 18
            elide: Text.ElideRight
            text: drawer.controller.episodeLabel
            color: drawer.mutedColor
            font.pixelSize: 11
        }

        PlayerButton {
            id: closeButton
            controller: drawer.controller
            iconName: "close"
            toolTip: qsTr("Close")
            compact: true
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            onClicked: drawer.closeRequested()
        }
    }

    ListView {
        id: list
        y: drawerHeader.height
        width: drawer.width - drawer.padding * 2
        height: drawer.height - drawer.padding * 2 - drawerHeader.height
        model: drawer.menuModel
        clip: true
        reuseItems: true
        cacheBuffer: 800
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: false
        currentIndex: -1

        Accessible.role: Accessible.List

        Component.onCompleted: positionViewAtEnd()

        onModelChanged: positionViewAtEnd()

        Keys.onPressed: function(event) {
            switch (event.key) {
            case Qt.Key_Down:
                drawer.moveCursor(1); event.accepted = true; break;
            case Qt.Key_Up:
                drawer.moveCursor(-1); event.accepted = true; break;
            case Qt.Key_Home:
                drawer.jumpCursor(true); event.accepted = true; break;
            case Qt.Key_End:
                drawer.jumpCursor(false); event.accepted = true; break;
            case Qt.Key_Return:
            case Qt.Key_Enter:
            case Qt.Key_Space:
                drawer.activateCurrent(); event.accepted = true; break;
            case Qt.Key_Escape:
                drawer.dismissKey(); event.accepted = true; break;
            case Qt.Key_Tab:
            case Qt.Key_Backtab:
                event.accepted = true; break;
            }
        }

        delegate: Item {
            id: rowItem

            required property var model
            required property int index

            enabled: !rowItem.model.heading && rowItem.model.enabled
            width: list.width
            height: rowItem.model.heading ? 26 : drawer.rowHeight
            opacity: rowItem.model.heading || rowItem.model.enabled ? 1.0 : 0.5

            readonly property bool headingRole: rowItem.model.heading
            readonly property bool rowEnabled: rowItem.model.enabled
            readonly property string menuRowId: rowItem.model.id
            objectName: "player.episodes.row." + menuRowId

            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                radius: Math.min(6, drawer.controller.cornerRadius)
                color: !rowItem.headingRole
                       && (hoverHandler.hovered || list.currentIndex === rowItem.index
                           || rowItem.model.selected || tapHandler.pressed)
                       ? drawer.hoverColor : "transparent"
            }

            Rectangle {
                visible: rowItem.model.selected && !rowItem.headingRole
                width: 2
                height: Math.max(8, rowItem.height - 18)
                anchors.left: parent.left
                anchors.leftMargin: 3
                anchors.verticalCenter: parent.verticalCenter
                radius: 1
                color: drawer.controller.accentColor
            }

            Text {
                id: labelText
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideRight
                text: rowItem.model.label
                color: drawer.textColor
                font.pixelSize: rowItem.headingRole ? 11 : 13
                font.weight: rowItem.model.selected || rowItem.headingRole ? Font.DemiBold : Font.Normal
                font.letterSpacing: rowItem.headingRole ? 0.6 : 0
            }

            Text {
                visible: !rowItem.headingRole && rowItem.model.detail !== ""
                anchors.left: labelText.left
                anchors.right: labelText.right
                anchors.top: labelText.bottom
                anchors.topMargin: 2
                elide: Text.ElideRight
                text: rowItem.model.detail
                color: drawer.mutedColor
                font.pixelSize: 11
            }

            HoverHandler {
                id: hoverHandler
            }

            TapHandler {
                id: tapHandler
                enabled: !rowItem.headingRole && rowItem.rowEnabled
                onTapped: {
                    list.currentIndex = rowItem.index;
                    drawer.activateRow(rowItem.index);
                }
            }

            ToolTip {
                visible: hoverHandler.hovered && rowItem.model.tooltip !== ""
                text: rowItem.model.tooltip
            }

            Accessible.role: rowItem.headingRole ? Accessible.StaticText : Accessible.MenuItem
            Accessible.name: rowItem.model.label
            Accessible.description: rowItem.model.tooltip !== "" ? rowItem.model.tooltip : rowItem.model.detail
            Accessible.selected: rowItem.model.selected
            Accessible.selectable: !rowItem.headingRole && rowItem.rowEnabled
            Accessible.onPressAction: {
                if (rowItem.headingRole || !rowItem.rowEnabled) return;
                drawer.activateRow(rowItem.index);
            }
        }
    }
}

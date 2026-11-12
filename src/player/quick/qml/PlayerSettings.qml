pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Mikai.Player

// Player settings flyout: the settings menu level rendered as an in-scene
// panel. Adjustment rows (speed, scale mode, volume) keep the panel open so
// values can be changed repeatedly; rows that open another surface close it.
PlayerPanel {
    id: panel

    property Item sceneItem: null
    property rect anchorRect: Qt.rect(0, 0, 0, 0)

    signal closeRequested()
    signal dismissKey()

    objectName: "player.settings"
    readonly property string menuId: "settings"
    readonly property PlayerMenuModel menuModel: controller.settingsMenu
    readonly property int rowHeight: 32
    readonly property int edgeMargin: 8
    readonly property color textColor: controller.darkTheme ? "#f4f4f5" : "#101014"
    readonly property color mutedColor: controller.darkTheme ? "#a1a1aa" : "#5a5a63"
    readonly property color hoverColor: Qt.rgba(controller.accentColor.r,
                                                controller.accentColor.g,
                                                controller.accentColor.b,
                                                controller.darkTheme ? 0.18 : 0.12)
    readonly property real sceneWidth: sceneItem ? sceneItem.width : 640
    readonly property real sceneHeight: sceneItem ? sceneItem.height : 720
    readonly property real panelWidth: Math.min(340, Math.max(1, sceneWidth - edgeMargin * 2))
    readonly property int maxBodyHeight: Math.max(1, sceneHeight - edgeMargin * 2 - 60)
    width: panelWidth
    height: Math.min(sceneHeight - edgeMargin * 2,
                     padding * 2 + panelHeader.height + (list.count > 0
                                                    ? Math.min(list.contentHeight, maxBodyHeight)
                                                    : Math.max(44, emptyLabel.implicitHeight + 16)))
    padding: 8
    z: 20

    Accessible.role: Accessible.PopupMenu
    Accessible.name: qsTr("Player settings")

    function takeFocus() {
        if (!visible) return;
        if (list.count > 0) {
            list.forceActiveFocus();
            if (list.currentIndex < 0) list.currentIndex = 0;
        } else {
            panel.forceActiveFocus();
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

    function activateRow(row, action) {
        if (row < 0) return;
        controller.activateMenuRow(menuId, row);
        if (action === PlayerMenuModel.SetVolume || action === PlayerMenuModel.ToggleMute
                || action === PlayerMenuModel.SetSpeed
                || action === PlayerMenuModel.SetVideoScaleMode) return;
        panel.closeRequested();
    }

    function activateCurrent() {
        const row = list.currentIndex;
        if (row < 0) return;
        if (!panel.rowIsSelectable(row)) return;
        const action = panel.menuModel.data(panel.menuModel.index(row, 0),
                                           PlayerMenuModel.ActionRole);
        panel.activateRow(row, action);
    }

    function computeX() {
        const margin = edgeMargin;
        const maxX = Math.max(margin, sceneWidth - margin - width);
        let x = anchorRect.x;
        if (anchorRect.width === 0 && anchorRect.x > sceneWidth / 2) x = anchorRect.x - width;
        return Math.max(margin, Math.min(x, maxX));
    }

    function computeY() {
        const margin = edgeMargin;
        const maxY = Math.max(margin, sceneHeight - margin - height);
        let y = anchorRect.y - height - 6;
        if (y < margin) y = anchorRect.y + anchorRect.height + 6;
        return Math.max(margin, Math.min(y, maxY));
    }

    x: computeX()
    y: computeY()

    onVisibleChanged: if (visible) takeFocus()

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape) {
            panel.dismissKey();
            event.accepted = true;
        }
    }

    Item {
        id: panelHeader
        width: panel.width - panel.padding * 2
        height: 30

        Text {
            anchors.left: parent.left
            anchors.right: closeButton.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: qsTr("Settings")
            color: panel.textColor
            font.pixelSize: 12
            font.weight: Font.DemiBold
            font.letterSpacing: 0.6
        }

        PlayerButton {
            id: closeButton
            controller: panel.controller
            iconName: "close"
            toolTip: qsTr("Close")
            compact: true
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            onClicked: panel.closeRequested()
        }
    }

    ListView {
        id: list
        y: panelHeader.height
        width: panel.width - panel.padding * 2
        height: list.count > 0 ? Math.min(contentHeight, panel.maxBodyHeight)
                               : Math.max(44, emptyLabel.implicitHeight + 16)
        model: panel.menuModel
        clip: true
        reuseItems: true
        cacheBuffer: 600
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: false
        interactive: contentHeight > height

        Accessible.role: Accessible.List

        Keys.onPressed: function(event) {
            switch (event.key) {
            case Qt.Key_Down:
                panel.moveCursor(1); event.accepted = true; break;
            case Qt.Key_Up:
                panel.moveCursor(-1); event.accepted = true; break;
            case Qt.Key_Home:
                panel.jumpCursor(true); event.accepted = true; break;
            case Qt.Key_End:
                panel.jumpCursor(false); event.accepted = true; break;
            case Qt.Key_Return:
            case Qt.Key_Enter:
            case Qt.Key_Space:
                panel.activateCurrent(); event.accepted = true; break;
            case Qt.Key_Escape:
                panel.dismissKey(); event.accepted = true; break;
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
            height: rowItem.model.heading ? 24 : panel.rowHeight
            opacity: rowItem.model.heading || rowItem.model.enabled ? 1.0 : 0.5

            readonly property bool headingRole: rowItem.model.heading
            readonly property bool rowEnabled: rowItem.model.enabled
            readonly property int rowAction: rowItem.model.action
            readonly property string menuRowId: rowItem.model.id
            objectName: "player.settings.row." + menuRowId
            readonly property string menuRowLabel: rowItem.model.label

            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                radius: Math.min(6, panel.controller.cornerRadius)
                color: !rowItem.headingRole && (hoverHandler.hovered || list.currentIndex === rowItem.index)
                       ? panel.hoverColor : "transparent"
            }

            Rectangle {
                visible: rowItem.model.selected && !rowItem.headingRole
                width: 2
                height: Math.max(8, rowItem.height - 14)
                anchors.left: parent.left
                anchors.leftMargin: 3
                anchors.verticalCenter: parent.verticalCenter
                radius: 1
                color: panel.controller.accentColor
            }

            PlayerIcon {
                id: glyph
                controller: panel.controller
                visible: !rowItem.headingRole && rowItem.model.icon !== ""
                name: rowItem.model.icon
                width: 16
                height: 16
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                id: labelText
                anchors.left: parent.left
                anchors.leftMargin: glyph.visible ? 36 : 14
                anchors.right: detailText.visible ? detailText.left : parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideRight
                text: rowItem.model.label
                color: panel.textColor
                font.pixelSize: rowItem.headingRole ? 11 : 13
                font.weight: rowItem.headingRole || rowItem.model.selected ? Font.DemiBold : Font.Normal
                font.letterSpacing: rowItem.headingRole ? 0.6 : 0
            }

            Text {
                id: detailText
                visible: !rowItem.headingRole && rowItem.model.detail !== ""
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, list.width * 0.4)
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignRight
                text: rowItem.model.detail
                color: panel.mutedColor
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
                    panel.activateRow(rowItem.index, rowItem.rowAction);
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
            Accessible.checkable: false
            Accessible.onPressAction: {
                if (rowItem.headingRole || !rowItem.rowEnabled) return;
                panel.activateRow(rowItem.index, rowItem.rowAction);
            }
        }
    }

    Text {
        id: emptyLabel
        visible: list.count === 0
        y: panelHeader.height
        width: panel.width - panel.padding * 2
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: qsTr("Nothing to adjust")
        color: panel.mutedColor
        font.pixelSize: 12
    }
}

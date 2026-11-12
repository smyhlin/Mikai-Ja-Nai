pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import Mikai.Player
PlayerPanel {
    id: menu

    property Item overlayRoot: null
    property int level: 0
    property string menuId: ""
    property PlayerMenuModel menuModel: null
    property rect anchorRect: Qt.rect(0, 0, 0, 0)
    property bool cascadeRight: true
    property bool focusedSlot: false
    property string emptyText: qsTr("Nothing available")

    signal rowHovered(int row, bool submenu, string menuId, rect sceneRect)
    signal rowActivated(int row, bool submenu, int action, string value)
    signal levelDismissRequested()
    signal dismissKey()

    readonly property int rowHeight: 32
    readonly property int headingHeight: 26
    readonly property int edgeMargin: 8
    readonly property color menuTextColor: controller.darkTheme ? "#f4f4f5" : "#101014"
    readonly property color mutedColor: controller.darkTheme ? "#a1a1aa" : "#5a5a63"
    readonly property color hoverColor: Qt.rgba(controller.accentColor.r,
                                                controller.accentColor.g,
                                                controller.accentColor.b,
                                                controller.darkTheme ? 0.18 : 0.12)
    readonly property real sceneWidth: overlayRoot ? overlayRoot.width : 640
    readonly property real sceneHeight: overlayRoot ? overlayRoot.height : 720
    readonly property real viewportMargin: Math.min(edgeMargin,
                                                     Math.max(0, (sceneWidth - 1) / 2),
                                                     Math.max(0, (sceneHeight - 1) / 2))
    readonly property real panelWidth: Math.min(340, Math.max(1, sceneWidth - viewportMargin * 2))
    readonly property int maxBodyHeight: Math.max(1, sceneHeight - viewportMargin * 2 - padding * 2)

    width: panelWidth
    height: Math.min(sceneHeight - viewportMargin * 2, padding * 2 + bodyHost.height)
    padding: Math.min(8, sceneWidth / 4, sceneHeight / 4)
    visible: false
    z: 10 + level

    Accessible.role: Accessible.PopupMenu
    Accessible.name: qsTr("Menu")

    function takeFocus() {
        if (!visible) return;
        body.forceActiveFocus();
        if (body.count > 0 && body.currentIndex < 0) jumpCursor(true);
    }

    function rowSceneRect(row) {
        if (!overlayRoot) return Qt.rect(anchorRect.x, anchorRect.y, anchorRect.width, anchorRect.height);
        let item = body.itemAtIndex(row);
        if (!item && row >= 0 && row < body.count) {
            body.positionViewAtIndex(row, ListView.Contain);
            body.forceLayout();
            item = body.itemAtIndex(row);
        }
        if (!item) return Qt.rect(anchorRect.x, anchorRect.y, anchorRect.width, anchorRect.height);
        return item.mapToItem(overlayRoot, 0, 0, item.width, item.height);
    }

    function rowIsSelectable(row) {
        if (!menuModel || row < 0 || row >= menuModel.rowCount()) return false;
        const itemIndex = menuModel.index(row, 0);
        return !menuModel.data(itemIndex, PlayerMenuModel.HeadingRole)
                && menuModel.data(itemIndex, PlayerMenuModel.EnabledRole);
    }

    function moveCursor(step) {
        if (body.count <= 0) return;
        let next = body.currentIndex < 0 ? (step > 0 ? -1 : body.count) : body.currentIndex;
        next += step;
        while (next >= 0 && next < body.count) {
            if (rowIsSelectable(next)) {
                body.currentIndex = next;
                body.positionViewAtIndex(next, ListView.Contain);
                return;
            }
            next += step;
        }
    }

    function jumpCursor(forward) {
        if (body.count <= 0) return;
        let next = forward ? 0 : body.count - 1;
        while (next >= 0 && next < body.count) {
            if (rowIsSelectable(next)) {
                body.currentIndex = next;
                body.positionViewAtIndex(next, ListView.Contain);
                return;
            }
            next += forward ? 1 : -1;
        }
    }

    function activateCurrent() {
        const row = body.currentIndex;
        if (!rowIsSelectable(row)) return;
        const itemIndex = menuModel.index(row, 0);
        menu.rowActivated(row,
                          menuModel.data(itemIndex, PlayerMenuModel.SubmenuRole),
                          menuModel.data(itemIndex, PlayerMenuModel.ActionRole),
                          menuModel.data(itemIndex, PlayerMenuModel.ValueRole));
    }

    function openCurrentSubmenu() {
        const row = body.currentIndex;
        if (!rowIsSelectable(row)) return;
        const itemIndex = menuModel.index(row, 0);
        if (!menuModel.data(itemIndex, PlayerMenuModel.SubmenuRole)) return;
        menu.rowHovered(row, true,
                        menuModel.data(itemIndex, PlayerMenuModel.ValueRole),
                        rowSceneRect(row));
    }

    function computeX() {
        const margin = viewportMargin;
        const maxX = Math.max(margin, sceneWidth - margin - width);
        let x = anchorRect.x;
        if (level > 0 && anchorRect.width > 0) {
            x = cascadeRight
                    ? anchorRect.x + anchorRect.width + 4
                    : anchorRect.x - width - 4;
        } else if (anchorRect.width === 0 && anchorRect.x > sceneWidth / 2) {
            x = anchorRect.x - width;
        }
        return Math.max(margin, Math.min(x, maxX));
    }

    function computeY() {
        const margin = viewportMargin;
        const maxY = Math.max(margin, sceneHeight - margin - height);
        let y;
        if (level > 0) {
            y = anchorRect.y;
        } else {
            y = anchorRect.y - height - 6;
            if (y < margin) y = anchorRect.y + anchorRect.height + 6;
        }
        return Math.max(margin, Math.min(y, maxY));
    }

    x: computeX()
    y: computeY()

    onFocusedSlotChanged: if (focusedSlot) takeFocus()
    onMenuModelChanged: body.currentIndex = -1

    Item {
        id: bodyHost
        width: menu.width - menu.padding * 2
        height: body.count > 0 ? Math.min(body.contentHeight, menu.maxBodyHeight)
                               : Math.min(menu.maxBodyHeight, Math.max(48, emptyLabel.implicitHeight + 20))

        ListView {
            id: body
            width: bodyHost.width
            height: bodyHost.height
            model: menu.menuModel
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
                    menu.moveCursor(1); event.accepted = true; break;
                case Qt.Key_Up:
                    menu.moveCursor(-1); event.accepted = true; break;
                case Qt.Key_Home:
                    menu.jumpCursor(true); event.accepted = true; break;
                case Qt.Key_End:
                    menu.jumpCursor(false); event.accepted = true; break;
                case Qt.Key_Right:
                    menu.openCurrentSubmenu(); event.accepted = true; break;
                case Qt.Key_Left:
                    menu.levelDismissRequested(); event.accepted = true; break;
                case Qt.Key_Return:
                case Qt.Key_Enter:
                case Qt.Key_Space:
                    menu.activateCurrent(); event.accepted = true; break;
                case Qt.Key_Escape:
                    menu.dismissKey(); event.accepted = true; break;
                case Qt.Key_Tab:
                case Qt.Key_Backtab:
                    event.accepted = true; break;
                }
            }

            delegate: Item {
                id: rowItem
                required property int index
                required property var model

                width: body.width
                height: rowItem.model.heading ? menu.headingHeight : menu.rowHeight
                opacity: rowItem.model.heading || rowItem.model.enabled ? 1.0 : 0.5

                readonly property bool headingRole: rowItem.model.heading
                readonly property bool rowEnabled: rowItem.model.enabled
                readonly property bool rowSubmenu: rowItem.model.submenu
                readonly property int rowAction: rowItem.model.action
                readonly property string rowValue: rowItem.model.value
                readonly property rect sceneRect: menu.overlayRoot
                                                       ? rowItem.mapToItem(menu.overlayRoot, 0, 0, rowItem.width, rowItem.height)
                                                       : Qt.rect(0, 0, 0, 0)

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    radius: Math.min(6, menu.controller.cornerRadius)
                    color: !rowItem.headingRole
                           && (hoverHandler.hovered || body.currentIndex === rowItem.index || tapHandler.pressed)
                           ? menu.hoverColor : "transparent"
                }

                Rectangle {
                    visible: rowItem.model.selected && !rowItem.headingRole
                    width: 2
                    height: Math.max(8, rowItem.height - 14)
                    anchors.left: parent.left
                    anchors.leftMargin: 3
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 1
                    color: menu.controller.accentColor
                }

                PlayerIcon {
                    id: glyph
                    controller: menu.controller
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
                    anchors.leftMargin: glyph.visible ? 36 : 12
                    anchors.right: detailText.visible ? detailText.left
                                                      : (chevron.visible ? chevron.left
                                                         : (checkMark.visible ? checkMark.left : parent.right))
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: rowItem.model.label
                    color: menu.menuTextColor
                    font.pixelSize: rowItem.headingRole ? 11 : 13
                    font.weight: rowItem.headingRole || rowItem.model.selected ? Font.DemiBold : Font.Normal
                    font.letterSpacing: rowItem.headingRole ? 0.6 : 0
                }

                Text {
                    id: detailText
                    visible: !rowItem.headingRole && rowItem.model.detail !== ""
                    anchors.right: chevron.visible ? chevron.left
                                                   : (checkMark.visible ? checkMark.left : parent.right)
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, body.width * 0.45)
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                    text: rowItem.model.detail
                    color: menu.mutedColor
                    font.pixelSize: 11
                }

                Text {
                    id: checkMark
                    visible: rowItem.model.selected && !rowItem.headingRole && !rowItem.rowSubmenu
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: "\u2713"
                    color: menu.controller.accentColor
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }

                PlayerIcon {
                    id: chevron
                    controller: menu.controller
                    visible: rowItem.rowSubmenu && !rowItem.headingRole
                    name: "chevron-right"
                    width: 12
                    height: 12
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                }

                HoverHandler {
                    id: hoverHandler
                    onHoveredChanged: {
                        if (!hoverHandler.hovered || rowItem.headingRole) return;
                        if (rowItem.rowEnabled) body.currentIndex = rowItem.index;
                        menu.rowHovered(rowItem.index, rowItem.rowSubmenu && rowItem.rowEnabled,
                                        rowItem.rowValue, menu.rowSceneRect(rowItem.index));
                    }
                }

                TapHandler {
                    id: tapHandler
                    enabled: !rowItem.headingRole && rowItem.rowEnabled
                    onTapped: {
                        body.currentIndex = rowItem.index;
                        menu.rowActivated(rowItem.index, rowItem.rowSubmenu, rowItem.rowAction, rowItem.rowValue);
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
                Accessible.expandable: rowItem.rowSubmenu
                Accessible.onPressAction: {
                    if (rowItem.headingRole || !rowItem.rowEnabled) return;
                    menu.rowActivated(rowItem.index, rowItem.rowSubmenu, rowItem.rowAction, rowItem.rowValue);
                }
            }
        }

        Text {
            id: emptyLabel
            visible: body.count === 0
            width: bodyHost.width
            anchors.verticalCenter: parent.verticalCenter
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: menu.emptyText
            color: menu.mutedColor
            font.pixelSize: 12
        }
    }
}

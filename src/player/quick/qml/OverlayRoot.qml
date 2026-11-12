pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import Mikai.Player
Item {
    id: root

    required property PlayerController controller

    readonly property bool anyMenuVisible: menuStack.length > 0
                                           || tracks.visible || settings.visible || episodes.visible
    readonly property bool interactionActive: anyMenuVisible || outsideArea.containsMouse
    property var menuStack: []
    ListModel {
        id: renderedMenuStack
    }
    property string rootMenuId: ""
    property rect rootAnchor: Qt.rect(0, 0, 0, 0)
    property rect seriesAnchor: Qt.rect(0, 0, 0, 0)

    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Player menus")
    Accessible.ignored: !anyMenuVisible

    function setStack(next) {
        menuStack = next

        // Keep the visual model incremental. Repeater preserves existing
        // delegates for appended/collapsed levels, so a parent row keeps its
        // pointer target while a child cascade is created or removed.
        let common = 0
        const commonLimit = Math.min(renderedMenuStack.count, next.length)
        while (common < commonLimit) {
            const current = renderedMenuStack.get(common)
            const entry = next[common]
            if (current.menuKey !== entry.id || current.parentRow !== entry.parentRow)
                break
            ++common
        }
        while (renderedMenuStack.count > common)
            renderedMenuStack.remove(renderedMenuStack.count - 1)
        for (let i = common; i < next.length; ++i)
            renderedMenuStack.append(stackRoles(next[i]))
        for (let i = 0; i < common; ++i) {
            const entry = next[i]
            const current = renderedMenuStack.get(i)
            if (current.anchorX !== entry.anchor.x)
                renderedMenuStack.setProperty(i, "anchorX", entry.anchor.x)
            if (current.anchorY !== entry.anchor.y)
                renderedMenuStack.setProperty(i, "anchorY", entry.anchor.y)
            if (current.anchorWidth !== entry.anchor.width)
                renderedMenuStack.setProperty(i, "anchorWidth", entry.anchor.width)
            if (current.anchorHeight !== entry.anchor.height)
                renderedMenuStack.setProperty(i, "anchorHeight", entry.anchor.height)
            if (current.stackCascadeRight !== entry.cascadeRight)
                renderedMenuStack.setProperty(i, "stackCascadeRight", entry.cascadeRight)
        }
        controller.setMenuStackVisible(anyMenuVisible)
    }

    function stackRoles(entry) {
        return {
            menuKey: entry.id,
            anchorX: entry.anchor.x,
            anchorY: entry.anchor.y,
            anchorWidth: entry.anchor.width,
            anchorHeight: entry.anchor.height,
            stackCascadeRight: entry.cascadeRight,
            parentRow: entry.parentRow
        }
    }

    function openMenu(menuId, anchorRect) {
        if (!menuId || menuId.length === 0) return
        if (rootMenuId === "series" && menuId !== "series" && controller.seriesExpanded)
            controller.setSeriesExpanded(false)
        if (menuId === "series")
            controller.setSeriesExpanded(true)
        rootMenuId = menuId
        rootAnchor = anchorRect
        tracks.visible = menuId === "audio" || menuId === "subtitles"
        settings.visible = menuId === "settings"
        episodes.visible = menuId === "series" && controller.seriesExpanded
        if (tracks.visible) tracks.kind = menuId
        const model = (tracks.visible || settings.visible || episodes.visible)
                     ? null : controller.menuModel(menuId)
        const cascadeRight = anchorRect.width > 0 || anchorRect.x <= width / 2
        setStack(model ? [{ id: menuId, model: model, anchor: anchorRect,
                            cascadeRight: cascadeRight, parentRow: -1 }] : [])
        if (!model && !tracks.visible && !settings.visible && !episodes.visible)
            controller.setMenuStackVisible(false)
    }

    function dismissAll() {
        if (!anyMenuVisible) return false
        const wasSeries = rootMenuId === "series" && controller.seriesExpanded
        tracks.visible = false
        settings.visible = false
        episodes.visible = false
        setStack([])
        rootMenuId = ""
        if (wasSeries) controller.setSeriesExpanded(false)
        root.forceActiveFocus()
        return true
    }

    function openSubmenu(parentLevel, row, childId, sceneRect) {
        if (parentLevel < 0 || parentLevel >= menuStack.length || !childId) return
        const parent = menuStack[parentLevel]
        const model = controller.submenuModel(parent.id, row)
        if (!model) {
            if (parentLevel + 1 < menuStack.length)
                setStack(menuStack.slice(0, parentLevel + 1))
            return
        }
        const existing = menuStack[parentLevel + 1]
        const sameCachedChild = existing && existing.parentRow === row
                && existing.id === childId && existing.model === model
        if (sameCachedChild && existing.anchor.x === sceneRect.x
                && existing.anchor.y === sceneRect.y
                && existing.anchor.width === sceneRect.width
                && existing.anchor.height === sceneRect.height)
            return
        const margin = Math.min(8, Math.max(0, (width - 1) / 2),
                                Math.max(0, (height - 1) / 2))
        const panelWidth = Math.min(340, Math.max(1, width - margin * 2))
        const rightX = sceneRect.x + sceneRect.width + 4
        const leftX = sceneRect.x - panelWidth - 4
        let cascadeRight = parent.cascadeRight
        const preferredFits = cascadeRight
                ? rightX + panelWidth <= width - margin
                : leftX >= margin
        const oppositeFits = cascadeRight
                ? leftX >= margin
                : rightX + panelWidth <= width - margin
        if (!preferredFits && oppositeFits)
            cascadeRight = !cascadeRight
        const next = menuStack.slice(0, parentLevel + 1)
        next.push({ id: childId, model: model, anchor: sceneRect,
                    cascadeRight: cascadeRight, parentRow: row })
        setStack(next)
    }

    function collapseAfter(parentLevel) {
        if (parentLevel + 1 < menuStack.length)
            setStack(menuStack.slice(0, parentLevel + 1))
    }

    function dismissLevel(level) {
        if (level > 0) {
            setStack(menuStack.slice(0, level))
            return
        }
        dismissAll()
    }

    function onOpenRequested(menuId) {
        const anchor = menuId === "series" ? seriesAnchor : Qt.rect(width * 0.5, height * 0.5, 0, 0)
        openMenu(menuId, anchor)
    }

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape && dismissAll()) event.accepted = true
    }

    Connections {
        target: root.controller
        function onMenuOpenRequested(menuId) { root.onOpenRequested(menuId) }
        function onMenusCloseRequested() { root.dismissAll() }
    }

    // Only clicks outside the full menu corridor dismiss it. Pointer movement
    // through the small inter-panel gap never collapses a submenu, so the user
    // can move directly from a parent row to its child without a grace timer.
    MouseArea {
        id: outsideArea
        anchors.fill: parent
        z: 0
        visible: root.anyMenuVisible
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: mouse => {
            root.controller.noteUserActivity()
            root.dismissAll()
            mouse.accepted = true
        }
        Accessible.ignored: true
    }

    Repeater {
        model: renderedMenuStack
        delegate: CascadingMenu {
            id: panel
            required property int index
            required property string menuKey
            required property real anchorX
            required property real anchorY
            required property real anchorWidth
            required property real anchorHeight
            required property bool stackCascadeRight
            required property int parentRow
            controller: root.controller
            overlayRoot: root
            level: panel.index
            menuId: panel.menuKey
            menuModel: root.controller.menuModel(panel.menuKey)
            anchorRect: Qt.rect(panel.anchorX, panel.anchorY,
                                panel.anchorWidth, panel.anchorHeight)
            cascadeRight: panel.stackCascadeRight
            visible: true
            focusedSlot: panel.index === root.menuStack.length - 1
            onRowHovered: (row, hasSubmenu, childId, sceneRect) => {
                if (hasSubmenu) root.openSubmenu(panel.index, row, childId, sceneRect)
                else root.collapseAfter(panel.index)
            }
            onRowActivated: (row, hasSubmenu, action, value) => {
                if (hasSubmenu) {
                    root.openSubmenu(panel.index, row, value, panel.rowSceneRect(row))
                } else {
                    root.controller.activateMenuRow(panel.menuId, row)
                    if (action !== PlayerMenuModel.SetVolume
                            && action !== PlayerMenuModel.ToggleMute
                            && action !== PlayerMenuModel.SetSpeed
                            && action !== PlayerMenuModel.SetVideoScaleMode)
                        root.dismissAll()
                }
            }
            onLevelDismissRequested: root.dismissLevel(panel.index)
            onDismissKey: root.dismissAll()
        }
    }

    TrackPanels {
        id: tracks
        controller: root.controller
        sceneItem: root
        anchorRect: root.rootAnchor
        visible: false
        z: 20
        onCloseRequested: root.dismissAll()
        onDismissKey: root.dismissAll()
    }

    PlayerSettings {
        id: settings
        controller: root.controller
        sceneItem: root
        anchorRect: root.rootAnchor
        visible: false
        z: 20
        onCloseRequested: root.dismissAll()
        onDismissKey: root.dismissAll()
    }

    EpisodeDrawer {
        id: episodes
        controller: root.controller
        sceneItem: root
        anchorRect: root.seriesAnchor
        visible: false
        z: 20
        onCloseRequested: root.dismissAll()
        onDismissKey: root.dismissAll()
    }

    Component.onCompleted: root.controller.setMenuStackVisible(false)
}

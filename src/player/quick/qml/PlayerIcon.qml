import QtQuick
import QtQuick.Window
import Mikai.Player

// One themed glyph from the real PlayerIconProvider set, painted on demand by
// IconPainter at the window's device pixel ratio. No pixmap cache of its own, no
// frame capture, no shader: the provider returns a raster tagged with the DPR,
// so a HiDPI window gets a crisp glyph and an unknown name stays visibly empty
// (the provider warns instead of substituting a different symbol).
Item {
    id: root

    required property PlayerController controller

    property string name
    /// Chrome ink; a caller that wants an accent glyph passes the resolved
    /// controller.accentColor.
    property color tint: controller.darkTheme ? Qt.rgba(1, 1, 1, 0.9) : "#18181b"
    /// Logical glyph box, in the same units as width/height.
    property int sourceSize: 16

    width: sourceSize
    height: sourceSize

    readonly property real _dpr: Window.window ? Window.window.devicePixelRatio : 1.0

    Accessible.ignored: true

    Image {
        id: glyph
        // Integer placement keeps the glyph on the physical pixel grid at
        // fractional scale factors.
        x: Math.round((root.width - width) * root._dpr / 2) / root._dpr
        y: Math.round((root.height - height) * root._dpr / 2) / root._dpr
        width: Math.max(1, Math.round(root.sourceSize))
        height: width
        visible: root.name.length > 0
        source: visible
                ? "image://player-icons/" + root.name
                  + "?size=" + root.sourceSize
                  + "&dpr=" + root._dpr.toFixed(3)
                  + "&color=" + encodeURIComponent(root.tint.toString())
                : ""
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: false
    }
}

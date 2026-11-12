// On-demand glyph rasterisation for the Quick player chrome.
//
// The QML side asks for `image://player-icons/<name>?...` and receives a
// transparent premultiplied QImage drawn by the shell's existing IconPainter
// glyphs; no pixmap, no video frame and no application state is involved, so
// the request is safe on the image-loading thread. An unknown name or an
// unparsable parameter produces an explicit warning and a null image: a
// typo must never silently draw a different symbol.
//
// Usage from QML (PlayerIcon.qml):
//     Image {
//         source: "image://player-icons/captions?size=16&dpr=" + root.devicePixelRatio
//                 + "&color=w90"
//     }
// A hex colour must be percent-encoded, because a raw '#' starts a URL
// fragment: `&color=%23ff5c7a` arrives here as `#ff5c7a`.

#pragma once

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtQuick/QQuickImageProvider>

QT_BEGIN_NAMESPACE
class QQmlEngine;
QT_END_NAMESPACE

namespace mplayer {
namespace quick {

/// Image provider behind the `player-icons` authority.
///
/// Request grammar: `<name>[?key=value&...]` with these keys and no others -
///   size   logical glyph box in pixels, 1..512 (falls back to the requested
///          source size, then 16)
///   dpr    device pixel ratio of the target window, any positive number
///          (default 1); the returned raster is size*dpr pixels and carries
///          that ratio. Only the resulting allocation is bounded, not the
///          screen scale the viewer chose.
///   color  `#RRGGBB`, `#AARRGGBB`, a Qt colour name, or one of the theme
///          tokens w95,w90,w70,w55,w45,w35,w25,w15,w10,red,green (default
///          w90); a '#' must be percent-encoded as %23 in the URL
///
/// The accent is intentionally not a token: it is mutable process state owned
/// by the GUI thread, and this provider runs on the image-loading thread, so
/// the caller passes its resolved hex value instead.
class PlayerIconProvider : public QQuickImageProvider {
public:
    /// The stable `image://` authority for every player glyph.
    static const char* providerId();

    PlayerIconProvider();

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

/// Registers the provider under PlayerIconProvider::providerId(); the player
/// window calls this once, before loading PlayerView.qml.
void installPlayerIcons(QQmlEngine* engine);

}  // namespace quick
}  // namespace mplayer

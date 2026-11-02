#pragma once

#include <QByteArray>
#include <QImage>
#include <QMetaType>
#include <QPixmap>
#include <QSize>
#include <QString>

namespace mikai::gui {

struct DecodedImage {
    QImage image;
    QString error;
};

/// Decodes `bytes`, never larger than `target`. `target` is in **device**
/// pixels: a caller on a HiDPI label passes deviceSizeFor(logical, dpr) so the
/// decode lands at the resolution the label shows instead of at half of it.
DecodedImage decodeImage(const QByteArray& bytes, const QSize& target = {});

/// The device-pixel size of a logical box: what a raster must be decoded, scaled
/// or allocated at so a HiDPI label can blit it one raster pixel per device
/// pixel. A zero or negative ratio means "no scaling".
QSize deviceSizeFor(const QSize& logical, qreal dpr);

/// An empty pixmap for `logical` allocated at the device size and tagged with
/// `dpr`: paint it in logical coordinates and every device pixel is real.
QPixmap pixmapForDevice(const QSize& logical, qreal dpr);

}  // namespace mikai::gui

Q_DECLARE_METATYPE(mikai::gui::DecodedImage)

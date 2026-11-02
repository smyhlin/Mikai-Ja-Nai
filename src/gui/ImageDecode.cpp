#include "ImageDecode.hpp"

#include <QBuffer>
#include <QImageIOHandler>
#include <QImageReader>
#include <QtMath>

#include <utility>

namespace mikai::gui {

QSize deviceSizeFor(const QSize& logical, qreal dpr) {
    if (logical.isEmpty()) return {};
    const qreal ratio = dpr > 0.0 ? dpr : 1.0;
    return QSize(qMax(1, qRound(logical.width() * ratio)),
                 qMax(1, qRound(logical.height() * ratio)));
}

QPixmap pixmapForDevice(const QSize& logical, qreal dpr) {
    const qreal ratio = dpr > 0.0 ? dpr : 1.0;
    QPixmap pixmap(deviceSizeFor(logical, ratio));
    if (pixmap.isNull()) return pixmap;
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(ratio);
    return pixmap;
}

DecodedImage decodeImage(const QByteArray& bytes, const QSize& target) {
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return {{}, buffer.errorString()};
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize source = reader.effectiveSize();
    if (source.isValid() && !target.isEmpty() &&
        (source.width() > target.width() || source.height() > target.height())) {
        QSize scaled = source.scaled(target, Qt::KeepAspectRatioByExpanding);
        if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90)) {
            scaled.transpose();
        }
        reader.setScaledSize(scaled);
    }
    QImage image = reader.read();
    if (image.isNull()) return {{}, reader.errorString()};
    return {std::move(image), {}};
}

}

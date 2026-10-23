#include "ImageDecode.hpp"

#include <QBuffer>
#include <QImageIOHandler>
#include <QImageReader>

#include <utility>

namespace mikai::gui {

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

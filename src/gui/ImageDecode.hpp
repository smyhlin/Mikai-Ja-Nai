#pragma once

#include <QByteArray>
#include <QImage>
#include <QMetaType>
#include <QSize>
#include <QString>

namespace mikai::gui {

struct DecodedImage {
    QImage image;
    QString error;
};

DecodedImage decodeImage(const QByteArray& bytes, const QSize& target = {});

}

Q_DECLARE_METATYPE(mikai::gui::DecodedImage)

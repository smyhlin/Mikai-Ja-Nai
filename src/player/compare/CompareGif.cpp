#include "CompareGif.hpp"

#include <QCoreApplication>
#include <QPainter>
#include <QSaveFile>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <cstdint>

namespace mplayer::shell {
namespace {

class GifBlocks {
public:
    explicit GifBlocks(QIODevice& output) : output_(output) {}

    bool code(int value, int width) {
        bits_ |= static_cast<uint32_t>(value) << bitCount_;
        bitCount_ += width;
        while (bitCount_ >= 8) {
            if (!byte(static_cast<char>(bits_ & 255))) return false;
            bits_ >>= 8;
            bitCount_ -= 8;
        }
        return true;
    }

    bool finish() {
        if (bitCount_ > 0 && !byte(static_cast<char>(bits_ & 255))) return false;
        return flush() && output_.putChar(0);
    }

private:
    bool byte(char value) {
        block_[size_++] = value;
        return size_ < static_cast<int>(block_.size()) || flush();
    }
    bool flush() {
        if (size_ == 0) return true;
        const bool ok = output_.putChar(static_cast<char>(size_)) &&
                        output_.write(block_.data(), size_) == size_;
        size_ = 0;
        return ok;
    }

    QIODevice& output_;
    std::array<char, 255> block_{};
    int size_ = 0;
    uint32_t bits_ = 0;
    int bitCount_ = 0;
};

void appendWord(QByteArray& bytes, int value) {
    bytes.append(static_cast<char>(value & 255));
    bytes.append(static_cast<char>((value >> 8) & 255));
}

QImage fitFrame(const QImage& image, QSize size) {
    if (image.size() == size && image.format() == QImage::Format_RGB32) return image;
    QImage frame(size, QImage::Format_RGB32);
    if (frame.isNull()) return {};
    frame.fill(Qt::black);
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QSize fitted = image.size().scaled(size, Qt::KeepAspectRatio);
    painter.drawImage(QRect(QPoint((size.width() - fitted.width()) / 2,
                                  (size.height() - fitted.height()) / 2), fitted), image);
    return frame;
}

bool writeFrame(QIODevice& output, const QImage& frame, int delay,
                const std::atomic_bool& cancelled) {
    QByteArray header("\x21\xf9\x04\x04", 4);
    appendWord(header, delay);
    header.append("\0\0\x2c\0\0\0\0", 7);
    appendWord(header, frame.width());
    appendWord(header, frame.height());
    header.append('\0');
    header.append('\x08');
    if (output.write(header) != header.size()) return false;

    GifBlocks blocks(output);
    std::array<int, 8192> keys;
    std::array<int, 8192> values;
    keys.fill(-1);
    int next = 258;
    int width = 9;
    int prefix = -1;
    if (!blocks.code(256, width)) return false;
    for (int y = 0; y < frame.height(); ++y) {
        if (cancelled.load(std::memory_order_relaxed)) return false;
        const auto* row = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x) {
            const QRgb rgb = row[x];
            const int pixel = (qRed(rgb) & 0xe0) | ((qGreen(rgb) >> 3) & 0x1c) |
                              (qBlue(rgb) >> 6);
            if (prefix < 0) {
                prefix = pixel;
                continue;
            }
            const int key = (prefix << 8) | pixel;
            int slot = static_cast<int>((static_cast<uint32_t>(key) * 2654435761u) >> 19);
            while (keys[slot] != -1 && keys[slot] != key) slot = (slot + 1) & 8191;
            if (keys[slot] == key) {
                prefix = values[slot];
                continue;
            }
            if (!blocks.code(prefix, width)) return false;
            if (next < 4096) {
                keys[slot] = key;
                values[slot] = next++;
                // The decoder creates each entry one emitted code after the encoder.
                if (next > (1 << width) && width < 12) ++width;
            } else {
                if (!blocks.code(256, width)) return false;
                keys.fill(-1);
                next = 258;
                width = 9;
            }
            prefix = pixel;
        }
    }
    if (prefix >= 0 && !blocks.code(prefix, width)) return false;
    if (next == (1 << width) && width < 12) ++width;
    return blocks.code(257, width) && blocks.finish();
}

} // namespace

bool saveComparisonGif(const QString& path, const QImage& a, const QImage& b,
                       int frameDelayMs, const std::atomic_bool& cancelled, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (error != nullptr) error->clear();
    const QSize size(std::max(a.width(), b.width()), std::max(a.height(), b.height()));
    if (a.isNull() || b.isNull() || size.width() > 65535 || size.height() > 65535 ||
        frameDelayMs <= 0) {
        return fail(QCoreApplication::translate("CompareGif", "Invalid comparison images or frame delay."));
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
    QByteArray header("GIF89a", 6);
    appendWord(header, size.width());
    appendWord(header, size.height());
    header.append("\xf7\0\0", 3);
    // One fixed palette keeps identical colors stable while A and B alternate.
    for (int i = 0; i < 256; ++i) {
        header.append(static_cast<char>(((i >> 5) & 7) * 255 / 7));
        header.append(static_cast<char>(((i >> 2) & 7) * 255 / 7));
        header.append(static_cast<char>((i & 3) * 255 / 3));
    }
    header.append("\x21\xff\x0bNETSCAPE2.0\x03\x01\0\0\0", 19);
    if (file.write(header) != header.size()) return fail(file.errorString());
    const int delay = static_cast<int>(
        std::clamp<qint64>((static_cast<qint64>(frameDelayMs) + 5) / 10, 1, 65535));
    for (const QImage* image : {&a, &b}) {
        if (cancelled.load(std::memory_order_relaxed)) {
            return fail(QCoreApplication::translate("CompareGif", "GIF export cancelled."));
        }
        const QImage frame = fitFrame(*image, size);
        if (frame.isNull()) {
            return fail(QCoreApplication::translate("CompareGif", "Not enough memory for GIF export."));
        }
        if (!writeFrame(file, frame, delay, cancelled)) {
            return fail(cancelled.load(std::memory_order_relaxed)
                            ? QCoreApplication::translate("CompareGif", "GIF export cancelled.")
                            : file.errorString());
        }
    }
    if (cancelled.load(std::memory_order_relaxed)) {
        return fail(QCoreApplication::translate("CompareGif", "GIF export cancelled."));
    }
    if (!file.putChar(';') || !file.commit()) return fail(file.errorString());
    return true;
}

CompareGifExport::CompareGifExport(QObject* parent)
    : QObject(parent), cancelled_(std::make_shared<std::atomic_bool>(false)) {
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
            &CompareGifExport::cancel);
}

void CompareGifExport::start(const QString& path, const QImage& a, const QImage& b,
                             int frameDelayMs) {
    Q_ASSERT(!started_);
    Q_ASSERT(parent() == nullptr);
    started_ = true;
    connect(this, &CompareGifExport::finished, this, &QObject::deleteLater,
            Qt::QueuedConnection);
    const auto cancelled = cancelled_;
    QThreadPool::globalInstance()->start([this, path, a, b, frameDelayMs, cancelled] {
        QString error;
        const bool succeeded = saveComparisonGif(path, a, b, frameDelayMs, *cancelled, &error);
        emit finished(succeeded, error);
    });
}

void CompareGifExport::cancel() {
    cancelled_->store(true, std::memory_order_relaxed);
}

} // namespace mplayer::shell

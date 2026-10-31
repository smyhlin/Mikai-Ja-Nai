#include "CompareGif.hpp"

#include <QCoreApplication>
#include <QPainter>
#include <QSaveFile>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

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

struct ColorBin {
    uint64_t count = 0;
    uint64_t red = 0;
    uint64_t green = 0;
    uint64_t blue = 0;
};

struct PaletteColor {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};

constexpr int kColorBins = 32 * 32 * 32;

int colorBin(int red, int green, int blue) {
    return ((red >> 3) << 10) | ((green >> 3) << 5) | (blue >> 3);
}

int binChannel(int index, int channel) {
    return channel == 0 ? ((index >> 10) & 31)
         : channel == 1 ? ((index >> 5) & 31)
                        : (index & 31);
}

bool makePalette(const QImage& frame, const std::atomic_bool& cancelled,
                 std::array<PaletteColor, 256>& palette,
                 std::array<uint8_t, kColorBins>& nearest) {
    std::vector<ColorBin> histogram(kColorBins);
    for (int y = 0; y < frame.height(); ++y) {
        if (cancelled.load(std::memory_order_relaxed)) return false;
        const auto* row = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x) {
            const QRgb pixel = row[x];
            auto& bin = histogram[colorBin(qRed(pixel), qGreen(pixel), qBlue(pixel))];
            ++bin.count;
            bin.red += qRed(pixel);
            bin.green += qGreen(pixel);
            bin.blue += qBlue(pixel);
        }
    }

    std::vector<int> colors;
    colors.reserve(kColorBins);
    for (int i = 0; i < kColorBins; ++i) {
        if (histogram[i].count != 0) colors.push_back(i);
    }
    std::vector<std::vector<int>> boxes;
    boxes.push_back(std::move(colors));
    while (boxes.size() < palette.size()) {
        if (cancelled.load(std::memory_order_relaxed)) return false;
        size_t selected = boxes.size();
        int selectedChannel = 0;
        int bestRange = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if ((i & 31) == 0 && cancelled.load(std::memory_order_relaxed)) return false;
            if (boxes[i].size() < 2) continue;
            int low[3] = {31, 31, 31};
            int high[3] = {0, 0, 0};
            for (const int index : boxes[i]) {
                for (int channel = 0; channel < 3; ++channel) {
                    const int value = binChannel(index, channel);
                    low[channel] = std::min(low[channel], value);
                    high[channel] = std::max(high[channel], value);
                }
            }
            int channel = 0;
            if (high[1] - low[1] > high[channel] - low[channel]) channel = 1;
            if (high[2] - low[2] > high[channel] - low[channel]) channel = 2;
            if (high[channel] - low[channel] > bestRange) {
                selected = i;
                selectedChannel = channel;
                bestRange = high[channel] - low[channel];
            }
        }
        if (selected == boxes.size()) break;
        auto& box = boxes[selected];
        std::sort(box.begin(), box.end(), [selectedChannel](int left, int right) {
            return binChannel(left, selectedChannel) < binChannel(right, selectedChannel);
        });
        uint64_t total = 0;
        for (const int index : box) total += histogram[index].count;
        uint64_t accumulated = 0;
        size_t split = 0;
        for (; split + 1 < box.size(); ++split) {
            accumulated += histogram[box[split]].count;
            if (accumulated * 2 >= total) {
                ++split;
                break;
            }
        }
        if (split == 0 || split >= box.size()) split = box.size() / 2;
        std::vector<int> right(box.begin() + static_cast<std::ptrdiff_t>(split), box.end());
        box.erase(box.begin() + static_cast<std::ptrdiff_t>(split), box.end());
        boxes.push_back(std::move(right));
    }

    for (size_t i = 0; i < boxes.size(); ++i) {
        uint64_t count = 0, red = 0, green = 0, blue = 0;
        for (const int index : boxes[i]) {
            count += histogram[index].count;
            red += histogram[index].red;
            green += histogram[index].green;
            blue += histogram[index].blue;
        }
        palette[i] = {static_cast<uint8_t>((red + count / 2) / count),
                      static_cast<uint8_t>((green + count / 2) / count),
                      static_cast<uint8_t>((blue + count / 2) / count)};
    }

    for (int index = 0; index < kColorBins; ++index) {
        if ((index & 255) == 0 && cancelled.load(std::memory_order_relaxed)) return false;
        int red = ((index >> 10) & 31) * 8 + 4;
        int green = ((index >> 5) & 31) * 8 + 4;
        int blue = (index & 31) * 8 + 4;
        if (histogram[index].count != 0) {
            const auto& bin = histogram[index];
            red = static_cast<int>(bin.red / bin.count);
            green = static_cast<int>(bin.green / bin.count);
            blue = static_cast<int>(bin.blue / bin.count);
        }
        int bestDistance = std::numeric_limits<int>::max();
        uint8_t best = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            const int dr = red - palette[i].red;
            const int dg = green - palette[i].green;
            const int db = blue - palette[i].blue;
            const int distance = dr * dr + dg * dg + db * db;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = static_cast<uint8_t>(i);
            }
        }
        nearest[index] = best;
    }
    return true;
}

bool writeFrame(QIODevice& output, const QImage& frame, int delay,
                const std::atomic_bool& cancelled) {
    std::array<PaletteColor, 256> palette{};
    std::array<uint8_t, kColorBins> nearest{};
    if (!makePalette(frame, cancelled, palette, nearest)) return false;

    QByteArray header("\x21\xf9\x04\x04", 4);
    appendWord(header, delay);
    header.append("\0\0\x2c\0\0\0\0", 7);
    appendWord(header, frame.width());
    appendWord(header, frame.height());
    header.append('\x87'); // Local 256-color table.
    for (const auto& color : palette) {
        header.append(static_cast<char>(color.red));
        header.append(static_cast<char>(color.green));
        header.append(static_cast<char>(color.blue));
    }
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

    std::vector<int> currentError(static_cast<size_t>(frame.width() + 2) * 3, 0);
    std::vector<int> nextError(static_cast<size_t>(frame.width() + 2) * 3, 0);
    for (int y = 0; y < frame.height(); ++y) {
        if (cancelled.load(std::memory_order_relaxed)) return false;
        const auto* row = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x) {
            const QRgb source = row[x];
            const int offset = (x + 1) * 3;
            const int red = std::clamp(qRed(source) + currentError[offset] / 16, 0, 255);
            const int green = std::clamp(qGreen(source) + currentError[offset + 1] / 16, 0, 255);
            const int blue = std::clamp(qBlue(source) + currentError[offset + 2] / 16, 0, 255);
            const uint8_t pixel = nearest[colorBin(red, green, blue)];
            const auto& quantized = palette[pixel];
            const int error[3] = {red - quantized.red, green - quantized.green, blue - quantized.blue};
            for (int channel = 0; channel < 3; ++channel) {
                currentError[offset + 3 + channel] += error[channel] * 7;
                nextError[offset - 3 + channel] += error[channel] * 3;
                nextError[offset + channel] += error[channel] * 5;
                nextError[offset + 3 + channel] += error[channel];
            }
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
                if (next > (1 << width) && width < 12) ++width;
            } else {
                if (!blocks.code(256, width)) return false;
                keys.fill(-1);
                next = 258;
                width = 9;
            }
            prefix = pixel;
        }
        currentError.swap(nextError);
        std::fill(nextError.begin(), nextError.end(), 0);
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
    header.append("\x70\0\0", 3);
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

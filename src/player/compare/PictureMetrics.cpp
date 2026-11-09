#include "PictureMetrics.hpp"

#include <algorithm>
#include <cmath>

namespace mplayer::shell {

namespace {

QRect pictureBand(const QSize& size) {
    return QRect(size.width() / 8, size.height() / 8, size.width() * 3 / 4,
                 size.height() * 3 / 4);
}

/// Highest minus lowest level inside the picture band, over all channels.
int bandRange(const QImage& image) {
    if (image.isNull()) return 0;
    const QRect band = pictureBand(image.size());
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    int lowest = 255;
    int highest = 0;
    for (int y = band.top(); y < band.bottom(); y += 2) {
        const uchar* row = rgb.constScanLine(y);
        for (int x = band.left() * 3; x < (band.left() + band.width()) * 3; ++x) {
            lowest = std::min(lowest, int(row[x]));
            highest = std::max(highest, int(row[x]));
        }
    }
    return highest - lowest;
}

/// Mean level inside the picture band, over all channels.
double bandMean(const QImage& image) {
    if (image.isNull()) return 0.0;
    const QRect band = pictureBand(image.size());
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    quint64 total = 0;
    qint64 counted = 0;
    for (int y = band.top(); y < band.bottom(); y += 2) {
        const uchar* row = rgb.constScanLine(y);
        for (int x = band.left() * 3; x < (band.left() + band.width()) * 3; ++x) {
            total += row[x];
        }
        counted += qint64(band.width()) * 3;
    }
    return counted > 0 ? double(total) / double(counted) : 0.0;
}

}  // namespace

int pictureRange(const QImage& image) { return image.isNull() ? 0 : bandRange(image); }

bool carriesNoPicture(const QImage& image) {
    if (image.isNull()) return true;
    // A real frame has both tone and range; a black card with dither has
    // neither, and an inference failure that emits pure black has neither.
    return bandRange(image) < 8 && bandMean(image) < 6.0;
}

double meanDifference(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull() || a.size() != b.size()) return -1.0;
    const QRect band = pictureBand(a.size());
    const QImage left = a.convertToFormat(QImage::Format_RGB888);
    const QImage right = b.convertToFormat(QImage::Format_RGB888);
    double total = 0.0;
    qint64 counted = 0;
    for (int y = band.top(); y < band.bottom(); ++y) {
        const uchar* l = left.constScanLine(y);
        const uchar* r = right.constScanLine(y);
        const int bytes = band.width() * 3;
        for (int x = band.left() * 3; x < band.left() * 3 + bytes; ++x) {
            total += std::abs(int(l[x]) - int(r[x]));
        }
        counted += bytes;
    }
    return counted > 0 ? total / static_cast<double>(counted) : -1.0;
}

double edgeEnergy(const QImage& image) {
    if (image.isNull()) return -1.0;
    const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
    const QRect band = pictureBand(gray.size());
    double total = 0.0;
    qint64 counted = 0;
    for (int y = band.top() + 1; y < band.bottom() - 1; ++y) {
        const uchar* above = gray.constScanLine(y - 1);
        const uchar* row = gray.constScanLine(y);
        const uchar* below = gray.constScanLine(y + 1);
        for (int x = band.left() + 1; x < band.right() - 1; ++x) {
            const int laplacian =
                4 * int(row[x]) - int(above[x]) - int(below[x]) - int(row[x - 1]) -
                int(row[x + 1]);
            total += std::abs(laplacian) / 4.0;
            ++counted;
        }
    }
    return counted > 0 ? total / static_cast<double>(counted) : -1.0;
}

double referenceDetail(const QImage& image) {
    if (image.isNull()) return -1.0;
    const int longest = std::max(image.width(), image.height());
    if (longest <= kDetailReferenceEdge) return edgeEnergy(image);
    const QImage scaled = longest == image.width()
                              ? image.scaledToWidth(kDetailReferenceEdge,
                                                    Qt::SmoothTransformation)
                              : image.scaledToHeight(kDetailReferenceEdge,
                                                     Qt::SmoothTransformation);
    return edgeEnergy(scaled);
}

EffectVerdict judgeEffect(const QImage& baseline, const QImage& model, double noiseFloor) {
    EffectVerdict verdict;
    if (baseline.isNull() || model.isNull()) return verdict;
    if (baseline.size() != model.size()) {
        verdict.kind = EffectVerdict::Kind::GeometryMismatch;
        return verdict;
    }
    verdict.referenceEdges = referenceDetail(baseline);
    if (bandRange(baseline) < kMinimumReferenceRange) {
        verdict.kind = EffectVerdict::Kind::FlatFrame;
        return verdict;
    }
    if (verdict.referenceEdges >= 0.0 && verdict.referenceEdges < kMinimumReferenceDetail) {
        verdict.kind = EffectVerdict::Kind::FlatFrame;
        return verdict;
    }
    if (carriesNoPicture(model)) {
        verdict.kind = EffectVerdict::Kind::BlankModel;
        return verdict;
    }
    const double measuredFloor = std::max(0.0, noiseFloor) * kNoiseFloorMultiplier;
    verdict.floor = std::max(kMinimumModelEffectLevels, measuredFloor);
    verdict.effect = meanDifference(baseline, model);
    verdict.kind = verdict.effect > verdict.floor ? EffectVerdict::Kind::Ok
                                                  : EffectVerdict::Kind::NoEffect;
    return verdict;
}

}  // namespace mplayer::shell

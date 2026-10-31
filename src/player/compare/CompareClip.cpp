#include "CompareClip.hpp"

#include <QCoreApplication>
#include <QImageWriter>
#include <QBuffer>
#include <QPainter>
#include <QSaveFile>
#include <QThreadPool>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace mplayer::shell {
namespace {
constexpr qint64 kMaxDurationMs = 10000;
constexpr quint64 kMaxAtomSize = std::numeric_limits<quint32>::max();

void be16(QByteArray& out, quint16 v) { out.append(char(v >> 8)); out.append(char(v)); }
void be32(QByteArray& out, quint32 v) {
    out.append(char(v >> 24)); out.append(char(v >> 16)); out.append(char(v >> 8)); out.append(char(v));
}
void le24(QByteArray& out, quint32 v) { out.append(char(v)); out.append(char(v >> 8)); out.append(char(v >> 16)); }
void le32(QByteArray& out, quint32 v) {
    out.append(char(v)); out.append(char(v >> 8)); out.append(char(v >> 16)); out.append(char(v >> 24));
}
void le16(QByteArray& out, quint16 v) { out.append(char(v)); out.append(char(v >> 8)); }

QByteArray atom(const char type[5], const QByteArray& payload) {
    QByteArray out;
    if (quint64(payload.size()) + 8 > kMaxAtomSize) return out;
    be32(out, quint32(payload.size() + 8));
    out.append(type, 4);
    out.append(payload);
    return out;
}
QByteArray fullBox(const char type[5], quint8 version, quint32 flags, const QByteArray& data) {
    QByteArray payload;
    payload.append(char(version));
    payload.append(char(flags >> 16)); payload.append(char(flags >> 8)); payload.append(char(flags));
    payload.append(data);
    return atom(type, payload);
}
QByteArray container(const char type[5], const QByteArray& children) { return atom(type, children); }
QByteArray riffChunk(const char type[5], const QByteArray& payload) {
    if (quint64(payload.size()) > std::numeric_limits<quint32>::max()) return {};
    QByteArray out(type, 4);
    le32(out, quint32(payload.size()));
    out.append(payload);
    if (payload.size() & 1) out.append('\0');
    return out;
}

QImage fitFrame(const QImage& source, QSize canvas) {
    if (source.size() == canvas && source.format() == QImage::Format_RGB32) return source;
    QImage frame(canvas, QImage::Format_RGB32);
    if (frame.isNull()) return {};
    frame.fill(Qt::black);
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QSize fitted = source.size().scaled(canvas, Qt::KeepAspectRatio);
    painter.drawImage(QRect(QPoint((canvas.width() - fitted.width()) / 2,
                                  (canvas.height() - fitted.height()) / 2), fitted), source);
    return frame;
}

bool encode(const QImage& image, const char* format, QByteArray& bytes, QString* error) {
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        if (error) *error = buffer.errorString();
        return false;
    }
    QImageWriter writer(&buffer, format);
    writer.setQuality(QByteArray(format) == "webp" ? 100 : 95);
    if (!writer.write(image)) {
        if (error) *error = writer.errorString();
        return false;
    }
    return true;
}

bool validWebP(const QByteArray& bytes, QByteArray& frameChunks, bool& hasAlpha) {
    if (bytes.size() < 20 || bytes.left(4) != "RIFF" || bytes.mid(8, 4) != "WEBP") return false;
    const quint32 riffSize = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + 4));
    if (quint64(riffSize) + 8 != quint64(bytes.size())) return false;
    int pos = 12;
    bool image = false;
    bool seenExtendedHeader = false;
    hasAlpha = false;
    while (pos < bytes.size()) {
        if (bytes.size() - pos < 8) return false;
        const QByteArray tag = bytes.mid(pos, 4);
        const quint32 len = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + pos + 4));
        const quint64 padded = quint64(len) + (len & 1u);
        if (padded > quint64(bytes.size() - pos - 8)) return false;
        if ((len & 1u) && bytes[pos + 8 + int(len)] != char(0)) return false;
        if (tag == "VP8 " || tag == "VP8L") {
            if (image || (tag == "VP8L" && hasAlpha)) return false;
            frameChunks.append(bytes.constData() + pos, int(8 + padded));
            image = true;
        } else if (tag == "ALPH") {
            if (image || hasAlpha) return false;
            frameChunks.append(bytes.constData() + pos, int(8 + padded));
            hasAlpha = true;
        } else if (tag == "VP8X") {
            if (seenExtendedHeader || pos != 12 || len != 10) return false;
            seenExtendedHeader = true;
        } else {
            return false;
        }
        pos += int(8 + padded);
    }
    return pos == bytes.size() && image;
}

bool writeMp4(QIODevice& out, const QImage frames[2], int delay,
              const std::atomic_bool& cancelled, QString* error) {
    QByteArray samples[2];
    for (int i = 0; i < 2; ++i) {
        if (cancelled.load(std::memory_order_relaxed)) return false;
        if (!encode(frames[i], "JPEG", samples[i], error)) return false;
        if (samples[i].isEmpty() || quint64(samples[i].size()) > std::numeric_limits<quint32>::max()) {
            if (error) *error = QCoreApplication::translate("CompareClip", "JPEG sample exceeds MP4 format limits.");
            return false;
        }
    }
    const quint32 timescale = 1000;
    const quint32 frameTicks = quint32(delay);
    const quint32 sampleCount = quint32(kMaxDurationMs / delay);
    QByteArray ftypPayload("isom", 4);
    be32(ftypPayload, 0x200);
    ftypPayload.append("isomiso2mp41", 12);
    const QByteArray ftyp = atom("ftyp", ftypPayload);
    const quint64 firstOffset64 = quint64(ftyp.size()) + 8;
    const quint64 payloadBytes = quint64(samples[0].size()) * ((sampleCount + 1) / 2) +
                                 quint64(samples[1].size()) * (sampleCount / 2);
    if (firstOffset64 + payloadBytes > std::numeric_limits<quint32>::max()) {
        if (error) *error = QCoreApplication::translate("CompareClip", "MP4 exceeds the 32-bit chunk offset limit.");
        return false;
    }
    QByteArray stsdData;
    be32(stsdData, 1);
    QByteArray jpegEntry;
    be32(jpegEntry, 0);
    be16(jpegEntry, 0);
    be16(jpegEntry, 1); // data reference index
    jpegEntry.append(QByteArray(16, '\0'));
    be16(jpegEntry, quint16(frames[0].width()));
    be16(jpegEntry, quint16(frames[0].height()));
    be32(jpegEntry, 0x00480000);
    be32(jpegEntry, 0x00480000);
    be32(jpegEntry, 0);
    be16(jpegEntry, 1);
    jpegEntry.append(QByteArray(32, '\0'));
    be16(jpegEntry, 24);
    be16(jpegEntry, 0xffff);
    stsdData.append(atom("jpeg", jpegEntry));
    QByteArray sttsData;
    be32(sttsData, 1);
    be32(sttsData, sampleCount);
    be32(sttsData, frameTicks);
    QByteArray stscData;
    be32(stscData, 1);
    be32(stscData, 1);
    be32(stscData, 1);
    be32(stscData, 1);
    QByteArray stszData;
    be32(stszData, 0);
    be32(stszData, sampleCount);
    QByteArray stcoData;
    be32(stcoData, sampleCount);
    quint64 sampleOffset = firstOffset64;
    for (quint32 i = 0; i < sampleCount; ++i) {
        const QByteArray& sample = samples[i & 1u];
        be32(stcoData, quint32(sampleOffset));
        be32(stszData, quint32(sample.size()));
        sampleOffset += quint64(sample.size());
    }
    if (quint64(stcoData.size()) + 16 > kMaxAtomSize) {
        if (error) *error = QCoreApplication::translate("CompareClip", "MP4 chunk table exceeds supported atom limits.");
        return false;
    }
    QByteArray stblChildren;
    stblChildren.append(fullBox("stsd", 0, 0, stsdData));
    stblChildren.append(fullBox("stts", 0, 0, sttsData));
    stblChildren.append(fullBox("stsc", 0, 0, stscData));
    stblChildren.append(fullBox("stsz", 0, 0, stszData));
    stblChildren.append(fullBox("stco", 0, 0, stcoData));

    QByteArray dref;
    be32(dref, 1);
    dref.append(fullBox("url ", 0, 1, {}));
    const QByteArray dinf = container("dinf", fullBox("dref", 0, 0, dref));
    QByteArray minfChildren = fullBox("vmhd", 0, 1, QByteArray(8, '\0'));
    minfChildren.append(dinf);
    minfChildren.append(container("stbl", stblChildren));
    const quint32 durationTicks = frameTicks * sampleCount;
    QByteArray mdhd;
    be32(mdhd, 0);
    be32(mdhd, 0);
    be32(mdhd, timescale);
    be32(mdhd, durationTicks);
    be16(mdhd, 0x55c4);
    be16(mdhd, 0);
    QByteArray hdlr;
    be32(hdlr, 0);
    hdlr.append("vide", 4);
    hdlr.append(QByteArray(12, '\0'));
    hdlr.append("VideoHandler\0", 13);
    QByteArray mdia;
    mdia.append(fullBox("mdhd", 0, 0, mdhd));
    mdia.append(fullBox("hdlr", 0, 0, hdlr));
    mdia.append(container("minf", minfChildren));
    QByteArray tkhd;
    be32(tkhd, 0);
    be32(tkhd, 0);
    be32(tkhd, 1);
    be32(tkhd, 0);
    be32(tkhd, durationTicks);
    tkhd.append(QByteArray(8, '\0'));
    be16(tkhd, 0);
    be16(tkhd, 0);
    be16(tkhd, 0);
    be16(tkhd, 0);
    be32(tkhd, 0x00010000);
    be32(tkhd, 0);
    be32(tkhd, 0);
    be32(tkhd, 0);
    be32(tkhd, 0x00010000);
    be32(tkhd, 0);
    be32(tkhd, 0);
    be32(tkhd, 0);
    be32(tkhd, 0x40000000);
    be32(tkhd, quint32(frames[0].width()) << 16);
    be32(tkhd, quint32(frames[0].height()) << 16);
    QByteArray trakContents = fullBox("tkhd", 0, 7, tkhd);
    trakContents.append(container("mdia", mdia));
    const QByteArray trak = container("trak", trakContents);
    QByteArray mvhd;
    be32(mvhd, 0);
    be32(mvhd, 0);
    be32(mvhd, timescale);
    be32(mvhd, durationTicks);
    be32(mvhd, 0x00010000);
    be16(mvhd, 0x0100);
    mvhd.append(QByteArray(10, '\0'));
    be32(mvhd, 0x00010000);
    be32(mvhd, 0);
    be32(mvhd, 0);
    be32(mvhd, 0);
    be32(mvhd, 0x00010000);
    be32(mvhd, 0);
    be32(mvhd, 0);
    be32(mvhd, 0);
    be32(mvhd, 0x40000000);
    mvhd.append(QByteArray(24, '\0'));
    be32(mvhd, 2);
    QByteArray moovContents = fullBox("mvhd", 0, 0, mvhd);
    moovContents.append(trak);
    const QByteArray moov = container("moov", moovContents);
    if (moov.isEmpty() || quint64(moov.size()) + firstOffset64 + payloadBytes > kMaxAtomSize) {
        if (error) *error = QCoreApplication::translate("CompareClip", "MP4 metadata exceeds supported atom limits.");
        return false;
    }
    QByteArray mdatHeader;
    be32(mdatHeader, quint32(8 + payloadBytes));
    mdatHeader.append("mdat", 4);
    if (out.write(ftyp) != ftyp.size() || out.write(mdatHeader) != mdatHeader.size()) return false;
    for (quint32 i = 0; i < sampleCount; ++i) {
        if (cancelled.load(std::memory_order_relaxed)) return false;
        const QByteArray& sample = samples[i & 1u];
        if (out.write(sample) != sample.size()) return false;
    }
    return !cancelled.load(std::memory_order_relaxed) && out.write(moov) == moov.size();
}

bool writeWebP(QIODevice& out, const QImage frames[2], int delay,
               const std::atomic_bool& cancelled, QString* error) {
    QByteArray frameData[2];
    bool alpha[2]{};
    for (int i = 0; i < 2; ++i) {
        if (cancelled.load(std::memory_order_relaxed)) return false;
        QByteArray still;
        if (!encode(frames[i], "webp", still, error) || !validWebP(still, frameData[i], alpha[i])) {
            if (error && error->isEmpty()) *error = QCoreApplication::translate("CompareClip", "Qt WebP writer produced unsupported or malformed chunks.");
            return false;
        }
    }
    QByteArray vp8x(10, '\0');
    vp8x[0] = char((alpha[0] || alpha[1] ? 0x10 : 0) | 0x02);
    vp8x[4] = char((frames[0].width() - 1) & 0xff);
    vp8x[5] = char((frames[0].width() - 1) >> 8);
    vp8x[6] = char((frames[0].width() - 1) >> 16);
    vp8x[7] = char((frames[0].height() - 1) & 0xff);
    vp8x[8] = char((frames[0].height() - 1) >> 8);
    vp8x[9] = char((frames[0].height() - 1) >> 16);

    QByteArray anim(4, '\0');
    le16(anim, 0);
    QByteArray body = riffChunk("VP8X", vp8x);
    body.append(riffChunk("ANIM", anim));
    for (int i = 0; i < 2; ++i) {
        if (cancelled.load(std::memory_order_relaxed)) return false;
        QByteArray payload;
        le24(payload, 0);
        le24(payload, 0);
        le24(payload, quint32(frames[i].width() - 1));
        le24(payload, quint32(frames[i].height() - 1));
        le24(payload, quint32(delay));
        payload.append(char(0));
        payload.append(frameData[i]);
        body.append(riffChunk("ANMF", payload));
    }
    if (body.isEmpty() || quint64(body.size()) + 4 > std::numeric_limits<quint32>::max()) {
        if (error) *error = QCoreApplication::translate("CompareClip", "Animated WebP exceeds the RIFF size limit.");
        return false;
    }
    QByteArray riff("WEBP", 4);
    riff.append(body);
    QByteArray header("RIFF", 4);
    le32(header, quint32(riff.size()));
    header.append(riff);
    return out.write(header) == header.size() && !cancelled.load(std::memory_order_relaxed);
}
} // namespace

bool saveComparisonClip(const QString& path, const QImage& a, const QImage& b, int frameDelayMs,
                        CompareClipFormat format, const std::atomic_bool& cancelled, QString* error) {
    const auto fail = [error](const QString& message) { if (error) *error = message; return false; };
    if (error) error->clear();
    if (a.isNull() || b.isNull() || frameDelayMs <= 0 || qint64(frameDelayMs) * 2 > kMaxDurationMs)
        return fail(QCoreApplication::translate("CompareClip", "Comparison images or frame delay are invalid; maximum clip duration is 10 seconds."));
    const QSize size(std::max(a.width(), b.width()), std::max(a.height(), b.height()));
    if (size.width() <= 0 || size.height() <= 0)
        return fail(QCoreApplication::translate("CompareClip", "Image dimensions are invalid."));
    if (format == CompareClipFormat::Mp4 && (size.width() > 65535 || size.height() > 65535))
        return fail(QCoreApplication::translate("CompareClip", "MP4 image dimensions exceed the 65535-pixel sample-entry limit."));
    if (format == CompareClipFormat::AnimatedWebP && (size.width() > 0xffffff || size.height() > 0xffffff || frameDelayMs > 0xffffff))
        return fail(QCoreApplication::translate("CompareClip", "Image dimensions or frame duration exceed WebP limits."));
    if (format != CompareClipFormat::Mp4 && format != CompareClipFormat::AnimatedWebP)
        return fail(QCoreApplication::translate("CompareClip", "Unsupported comparison clip format."));
    QImage frames[2] = {fitFrame(a, size), fitFrame(b, size)};
    if (frames[0].isNull() || frames[1].isNull()) return fail(QCoreApplication::translate("CompareClip", "Not enough memory to prepare comparison frames."));
    if (cancelled.load(std::memory_order_relaxed)) return fail(QCoreApplication::translate("CompareClip", "Clip export cancelled."));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
    const bool ok = format == CompareClipFormat::Mp4 ? writeMp4(file, frames, frameDelayMs, cancelled, error)
                                                     : writeWebP(file, frames, frameDelayMs, cancelled, error);
    if (!ok) {
        if (cancelled.load(std::memory_order_relaxed)) return fail(QCoreApplication::translate("CompareClip", "Clip export cancelled."));
        if (!error || error->isEmpty()) return fail(file.errorString());
        return false;
    }
    if (cancelled.load(std::memory_order_relaxed)) return fail(QCoreApplication::translate("CompareClip", "Clip export cancelled."));
    if (!file.commit()) return fail(file.errorString());
    return true;
}

CompareClipExport::CompareClipExport(QObject* parent)
    : QObject(parent), cancelled_(std::make_shared<std::atomic_bool>(false)) {
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &CompareClipExport::cancel);
}
void CompareClipExport::start(const QString& path, const QImage& a, const QImage& b, int frameDelayMs, CompareClipFormat format) {
    Q_ASSERT(!started_); Q_ASSERT(parent() == nullptr); started_ = true;
    connect(this, &CompareClipExport::finished, this, &QObject::deleteLater, Qt::QueuedConnection);
    const auto cancelled = cancelled_;
    QThreadPool::globalInstance()->start([this, path, a, b, frameDelayMs, format, cancelled] {
        QString error;
        const bool succeeded = saveComparisonClip(path, a, b, frameDelayMs, format, *cancelled, &error);
        emit finished(succeeded, error);
    });
}
void CompareClipExport::cancel() { cancelled_->store(true, std::memory_order_relaxed); }
} // namespace mplayer::shell

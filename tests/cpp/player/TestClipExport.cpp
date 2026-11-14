#include "TestClipExport.hpp"
#include "ClipExport.hpp"

#include "MpvApi.hpp"
#include "MpvHandle.hpp"
#include "PlayerController.hpp"
#include "player/quick/ClipSelection.hpp"

#include <QApplication>
#include <QWindow>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QtEndian>
#include <QVector>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

using mplayer::shell::ClipExport;
using mplayer::shell::ClipExportRequest;
using mplayer::MpvApi;
using mplayer::MpvBundle;

namespace {

MpvBundle envBundle() {
    const QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (module.isEmpty()) return {};
    const QFileInfo info(module);
    MpvBundle bundle;
    bundle.root = info.absolutePath();
    bundle.modulePath = info.absoluteFilePath();
    bundle.descriptorId = QStringLiteral("env-standard-runtime");
    bundle.protocolVersion = 1;
    return bundle;
}

QString fixtureClip() {
    return QDir::toNativeSeparators(QStringLiteral(
        MIKAI_SOURCE_DIR "/tests/fixtures/media/motion_1920x1080_24fps_8bit_hevc.mp4"));
}

QString audioFixtureClip() {
    return QDir::toNativeSeparators(QStringLiteral(
        MIKAI_SOURCE_DIR "/tests/fixtures/media/cut_saturated_black_320x180_24fps_8bit_hevc.mp4"));
}

bool writeToneWav(const QString& path) {
    constexpr quint32 sampleRate = 48000;
    constexpr quint16 channels = 1;
    constexpr quint16 bitsPerSample = 16;
    constexpr quint32 sampleCount = sampleRate * 8;
    constexpr quint32 dataSize = sampleCount * channels * (bitsPerSample / 8);
    QByteArray wav;
    wav.reserve(int(44 + dataSize));
    wav.append("RIFF", 4);
    const auto appendU32 = [&wav](quint32 value) {
        char bytes[4];
        qToLittleEndian(value, reinterpret_cast<uchar*>(bytes));
        wav.append(bytes, 4);
    };
    const auto appendU16 = [&wav](quint16 value) {
        char bytes[2];
        qToLittleEndian(value, reinterpret_cast<uchar*>(bytes));
        wav.append(bytes, 2);
    };
    appendU32(36 + dataSize);
    wav.append("WAVEfmt ", 8);
    appendU32(16);
    appendU16(1);
    appendU16(channels);
    appendU32(sampleRate);
    appendU32(sampleRate * channels * (bitsPerSample / 8));
    appendU16(channels * (bitsPerSample / 8));
    appendU16(bitsPerSample);
    wav.append("data", 4);
    appendU32(dataSize);
    constexpr double pi = 3.14159265358979323846;
    for (quint32 i = 0; i < sampleCount; ++i) {
        const double phase = 2.0 * pi * 1320.0 * i / sampleRate;
        const qint16 sample = qint16(std::sin(phase) * 10000.0);
        appendU16(quint16(sample));
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(wav) == wav.size();
}

QByteArray existingBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool finishExport(QSignalSpy& finished, int timeoutMs = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (finished.isEmpty() && timer.elapsed() < timeoutMs) {
        finished.wait(50);
    }
    return !finished.isEmpty();
}

struct DecodedPcm {
    QVector<float> monoSamples;
    quint32 sampleRate = 0;
};

bool decodeWavToMono(const QByteArray& wav, DecodedPcm& decoded, QString& error) {
    const auto malformed = [&error](const QString& detail) {
        error = QStringLiteral("unsupported or malformed PCM WAV: %1").arg(detail);
        return false;
    };
    if (wav.size() < 12 || wav.mid(0, 4) != "RIFF" || wav.mid(8, 4) != "WAVE")
        return malformed(QStringLiteral("missing RIFF/WAVE header"));

    const auto byteAt = [&wav](qsizetype offset) {
        return quint32(static_cast<unsigned char>(wav.at(offset)));
    };
    const auto u16 = [&byteAt](qsizetype offset) {
        return quint16(byteAt(offset) | (byteAt(offset + 1) << 8));
    };
    const auto u32 = [&byteAt](qsizetype offset) {
        return byteAt(offset) | (byteAt(offset + 1) << 8) |
               (byteAt(offset + 2) << 16) | (byteAt(offset + 3) << 24);
    };
    const quint64 riffEnd = quint64(u32(4)) + 8;
    if (riffEnd > quint64(wav.size()) || riffEnd < 12)
        return malformed(QStringLiteral("RIFF size exceeds available data"));

    quint16 format = 0;
    quint16 formatTag = 0;
    quint16 channels = 0;
    quint16 bitsPerSample = 0;
    quint16 validBitsPerSample = 0;
    quint16 blockAlign = 0;
    quint32 sampleRate = 0;
    quint32 channelMask = 0;
    quint32 fmtBytes = 0;
    qsizetype dataOffset = -1;
    quint32 dataSize = 0;
    bool haveFormat = false;
    bool extensible = false;
    quint64 offset = 12;
    while (offset + 8 <= riffEnd) {
        const qsizetype chunkOffset = qsizetype(offset);
        const quint32 chunkSize = u32(chunkOffset + 4);
        const quint64 bodyOffset = offset + 8;
        const quint64 bodyEnd = bodyOffset + chunkSize;
        if (bodyEnd > riffEnd)
            return malformed(QStringLiteral("chunk exceeds RIFF bounds"));
        if (wav.mid(chunkOffset, 4) == "fmt ") {
            if (chunkSize < 16) return malformed(QStringLiteral("short fmt chunk"));
            format = u16(qsizetype(bodyOffset));
            formatTag = format;
            channels = u16(qsizetype(bodyOffset + 2));
            sampleRate = u32(qsizetype(bodyOffset + 4));
            blockAlign = u16(qsizetype(bodyOffset + 12));
            bitsPerSample = u16(qsizetype(bodyOffset + 14));
            validBitsPerSample = bitsPerSample;
            fmtBytes = chunkSize;
            extensible = format == 0xfffe;
            if (extensible) {
                if (chunkSize < 40)
                    return malformed(QStringLiteral("WAVE_FORMAT_EXTENSIBLE fmt chunk is shorter than 40 bytes"));
                const quint16 extensionBytes = u16(qsizetype(bodyOffset + 16));
                if (extensionBytes < 22 || quint32(extensionBytes) > chunkSize - 18)
                    return malformed(QStringLiteral("invalid WAVE_FORMAT_EXTENSIBLE cbSize"));
                validBitsPerSample = u16(qsizetype(bodyOffset + 18));
                channelMask = u32(qsizetype(bodyOffset + 20));
                const quint32 subtype = u32(qsizetype(bodyOffset + 24));
                constexpr quint8 canonicalGuidTail[] = {
                    0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
                bool canonicalGuid = true;
                for (qsizetype i = 0; i < qsizetype(sizeof(canonicalGuidTail)); ++i) {
                    if (byteAt(qsizetype(bodyOffset + 28) + i) != canonicalGuidTail[i]) {
                        canonicalGuid = false;
                        break;
                    }
                }
                if (canonicalGuid && (subtype == 1 || subtype == 3))
                    format = quint16(subtype);
                else
                    format = 0xffff;
            }
            haveFormat = true;
        } else if (wav.mid(chunkOffset, 4) == "data" && dataOffset < 0) {
            dataOffset = qsizetype(bodyOffset);
            dataSize = chunkSize;
        }
        offset = bodyEnd + (chunkSize & 1U);
        if (offset > riffEnd) return malformed(QStringLiteral("missing chunk padding"));
    }
    if (offset != riffEnd) return malformed(QStringLiteral("trailing incomplete chunk"));
    if (!haveFormat || dataOffset < 0)
        return malformed(QStringLiteral("missing fmt or data chunk"));
    if (channels == 0 || sampleRate == 0)
        return malformed(QStringLiteral("zero channels or sample rate"));
    const auto unsupportedFormat = [&]() {
        return malformed(QStringLiteral("unsupported WAV encoding: format=%1, bitdepth=%2, channels=%3, "
                                        "blockAlign=%4, rate=%5, fmtbytes=%6")
                              .arg(formatTag)
                              .arg(bitsPerSample)
                              .arg(channels)
                              .arg(blockAlign)
                              .arg(sampleRate)
                              .arg(fmtBytes));
    };
    const int bytesPerSample = bitsPerSample / 8;
    if ((format != 1 || bitsPerSample != 16) &&
        (format != 3 || bitsPerSample != 32))
        return unsupportedFormat();
    if (extensible) {
        if (validBitsPerSample != bitsPerSample)
            return unsupportedFormat();
        if (channelMask != 0) {
            quint32 mask = channelMask;
            quint16 maskChannels = 0;
            while (mask != 0) {
                maskChannels = quint16(maskChannels + quint16(mask & 1U));
                mask >>= 1;
            }
            if (maskChannels != channels)
                return unsupportedFormat();
        }
    }
    if (bitsPerSample % 8 != 0 || blockAlign != channels * bytesPerSample)
        return unsupportedFormat();
    if (dataSize % blockAlign != 0)
        return malformed(QStringLiteral("data chunk ends in a partial frame"));

    const quint32 frameCount = dataSize / blockAlign;
    decoded.monoSamples.resize(qsizetype(frameCount));
    decoded.sampleRate = sampleRate;
    for (quint32 frame = 0; frame < frameCount; ++frame) {
        double sum = 0.0;
        for (quint16 channel = 0; channel < channels; ++channel) {
            const qsizetype sampleOffset = dataOffset + qsizetype(frame) * blockAlign +
                                           qsizetype(channel) * bytesPerSample;
            float sample = 0.0f;
            if (format == 1) {
                const quint16 bits = u16(sampleOffset);
                qint16 value = 0;
                std::memcpy(&value, &bits, sizeof(value));
                sample = float(value) / 32768.0f;
            } else {
                const quint32 bits = u32(sampleOffset);
                std::memcpy(&sample, &bits, sizeof(sample));
            }
            if (!std::isfinite(sample))
                return malformed(QStringLiteral("non-finite float sample"));
            sum += sample;
        }
        decoded.monoSamples[qsizetype(frame)] = float(sum / channels);
    }
    return true;
}

bool decodeAudioToPcm(const QString& inputPath, const QString& wavPath,
                      DecodedPcm& pcm, QString& error) {
    try {
        bool loaded = false;
        bool ended = false;
        bool eof = false;
        {
            mplayer::MpvHandle decoder({
                {"config", "no"}, {"vo", "null"}, {"ao", "pcm"},
                {"ao-pcm-waveheader", "yes"}, {"ao-pcm-file", wavPath.toStdString()},
                {"terminal", "no"}, {"input-default-bindings", "no"}
            });
            decoder.setEventSink([&](mpv_event* event) {
                if (event == nullptr) return;
                if (event->event_id == MPV_EVENT_FILE_LOADED) loaded = true;
                if (event->event_id == MPV_EVENT_END_FILE) {
                    ended = true;
                    const auto* end = static_cast<const mpv_event_end_file*>(event->data);
                    eof = end != nullptr && end->reason == MPV_END_FILE_REASON_EOF &&
                          end->error >= 0;
                    if (!eof) {
                        error = end != nullptr && end->error < 0
                            ? QStringLiteral("PCM decode ended with mpv error %1").arg(end->error)
                            : QStringLiteral("PCM decode ended before EOF");
                    }
                }
            });
            decoder.loadFile(inputPath.toStdString());
            QElapsedTimer timer;
            timer.start();
            while (!ended && timer.elapsed() < 30000) {
                decoder.drainEvents();
                if (!ended) QThread::msleep(10);
            }
        }
        if (!loaded) {
            error = QStringLiteral("PCM decoder did not report FILE_LOADED");
            return false;
        }
        if (!eof) {
            if (error.isEmpty()) error = QStringLiteral("PCM decoder timed out before EOF");
            return false;
        }
        QFile wavFile(wavPath);
        if (!wavFile.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("could not open decoded PCM WAV output");
            return false;
        }
        const QByteArray wav = wavFile.readAll();
        return decodeWavToMono(wav, pcm, error);
    } catch (const std::exception& exception) {
        error = QString::fromUtf8(exception.what());
        return false;
    }
}

double goertzelPower(const DecodedPcm& pcm, int firstSample, int sampleCount,
                     double frequency) {
    const double omega = 2.0 * 3.14159265358979323846 * frequency / pcm.sampleRate;
    const double coefficient = 2.0 * std::cos(omega);
    double previous = 0.0;
    double beforePrevious = 0.0;
    for (int i = 0; i < sampleCount; ++i) {
        const double current = pcm.monoSamples[firstSample + i] +
                               coefficient * previous - beforePrevious;
        beforePrevious = previous;
        previous = current;
    }
    return previous * previous + beforePrevious * beforePrevious -
           coefficient * previous * beforePrevious;
}

bool decodeAndInspect(const QString& path, double& duration, int& width, int& height,
                      bool& audio, QString& error) {
    try {
        mplayer::MpvHandle decoder({{"config", "no"}, {"vo", "null"}, {"ao", "null"},
                                    {"terminal", "no"}, {"input-default-bindings", "no"},
                                    {"keep-open", "yes"}});
        bool loaded = false;
        bool ended = false;
        bool eof = false;
        decoder.setEventSink([&](mpv_event* event) {
            if (event == nullptr) return;
            if (event->event_id == MPV_EVENT_FILE_LOADED) loaded = true;
            if (event->event_id == MPV_EVENT_END_FILE) {
                ended = true;
                const auto* end = static_cast<const mpv_event_end_file*>(event->data);
                eof = end != nullptr && end->reason == MPV_END_FILE_REASON_EOF && end->error >= 0;
                if (!eof) {
                    error = end != nullptr && end->error < 0
                        ? QStringLiteral("decode ended with mpv error %1").arg(end->error)
                        : QStringLiteral("decode ended before EOF (reason %1)")
                              .arg(end != nullptr ? int(end->reason) : -1);
                }
            }
        });
        decoder.loadFile(path.toStdString());
        mplayer::MpvApi& api = mplayer::MpvApi::instance();
        QElapsedTimer timer;
        timer.start();
        while (!ended && !eof && timer.elapsed() < 30000) {
            decoder.drainEvents();
            int reached = 0;
            if (api.functions().getProperty(decoder.raw(), "eof-reached",
                                           MPV_FORMAT_FLAG, &reached) >= 0 && reached != 0)
                eof = true;
            if (!ended && !eof) QThread::msleep(10);
        }
        if (!loaded) {
            error = QStringLiteral("decoder did not report FILE_LOADED");
            return false;
        }
        if (!eof) {
            if (error.isEmpty()) error = QStringLiteral("decoder timed out before EOF");
            return false;
        }
        const auto readRequiredNumber = [&](const char* property, double& value) {
            const int rc = api.functions().getProperty(decoder.raw(), property, MPV_FORMAT_DOUBLE, &value);
            if (rc < 0) {
                const char* detail = api.functions().errorString(rc);
                error = QStringLiteral("could not inspect decoded %1: %2")
                    .arg(QString::fromLatin1(property),
                         detail != nullptr ? QString::fromUtf8(detail) : QString::number(rc));
                return false;
            }
            return true;
        };
        double decodedWidth = 0.0;
        double decodedHeight = 0.0;
        if (!readRequiredNumber("video-params/w", decodedWidth) ||
            !readRequiredNumber("video-params/h", decodedHeight) ||
            !readRequiredNumber("duration", duration)) return false;
        if (!std::isfinite(decodedWidth) || !std::isfinite(decodedHeight) ||
            decodedWidth < 1.0 || decodedHeight < 1.0 ||
            decodedWidth > std::numeric_limits<int>::max() ||
            decodedHeight > std::numeric_limits<int>::max() ||
            !std::isfinite(duration) || duration <= 0.0) {
            error = QStringLiteral("decoded video dimensions or duration are invalid");
            return false;
        }
        width = int(decodedWidth);
        height = int(decodedHeight);

        double sampleRate = 0.0;
        const bool hasAudioParams =
            api.functions().getProperty(decoder.raw(), "audio-params/samplerate",
                                        MPV_FORMAT_DOUBLE, &sampleRate) >= 0 &&
            std::isfinite(sampleRate) && sampleRate > 0.0;
        bool selectedAudioTrack = false;
        mpv_node tracks{};
        if (api.functions().getProperty(decoder.raw(), "track-list", MPV_FORMAT_NODE, &tracks) >= 0 &&
            tracks.format == MPV_FORMAT_NODE_ARRAY && tracks.u.list != nullptr) {
            for (int i = 0; i < tracks.u.list->num && !selectedAudioTrack; ++i) {
                const mpv_node& track = tracks.u.list->values[i];
                if (track.format != MPV_FORMAT_NODE_MAP || track.u.list == nullptr) continue;
                bool audioType = false;
                bool selected = false;
                for (int field = 0; field < track.u.list->num; ++field) {
                    const char* key = track.u.list->keys[field];
                    const mpv_node& value = track.u.list->values[field];
                    if (key != nullptr && std::strcmp(key, "type") == 0 &&
                        value.format == MPV_FORMAT_STRING && value.u.string != nullptr)
                        audioType = std::strcmp(value.u.string, "audio") == 0;
                    else if (key != nullptr && std::strcmp(key, "selected") == 0 &&
                             value.format == MPV_FORMAT_FLAG)
                        selected = value.u.flag != 0;
                }
                selectedAudioTrack = audioType && selected;
            }
        }
        api.functions().freeNodeContents(&tracks);
        audio = selectedAudioTrack && hasAudioParams;
        if (selectedAudioTrack != hasAudioParams) {
            error = QStringLiteral("decoded audio track and audio parameters do not agree");
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = QString::fromUtf8(exception.what());
        return false;
    }
}

}  // namespace

void TestClipExport::initTestCase() {
    if (MpvApi::instance().loaded()) return;
    const MpvBundle bundle = envBundle();
    if (bundle.modulePath.isEmpty()) return;
    QString reason;
    if (!MpvApi::instance().load(bundle, &reason)) selectionError_ = reason;
}

bool TestClipExport::requireRuntime() {
    if (MpvApi::instance().loaded()) return true;
    if (!selectionError_.isEmpty()) {
        QTest::qFail(qPrintable(QStringLiteral("cannot select %1: %2")
                                    .arg(envBundle().modulePath, selectionError_)),
                     __FILE__, __LINE__);
        return false;
    }
    QTest::qSkip("set MIKAI_MPV_MODULE to the libmpv-2.dll of a real runtime to run this",
                 __FILE__, __LINE__);
    return false;
}

void TestClipExport::clipSelectionNeverEscapesTheSourceOrExceedsOneMinute() {
    using namespace mplayer::quick;
    auto selection = initialClipSelection(1800.0, 900.0);
    QVERIFY(selection.valid());
    QCOMPARE(selection.windowStart, 780.0);
    QCOMPARE(selection.windowEnd, 1020.0);
    selection = boundedClipSelection(selection, -1000.0, 9000.0);
    QVERIFY(selection.valid());
    QCOMPARE(selection.start, 780.0);
    QCOMPARE(selection.length(), 60.0);
    selection = initialClipSelection(4.0, 3.9);
    QVERIFY(selection.valid());
    selection = boundedClipSelection(selection, -1.0, 100.0);
    QCOMPARE(selection.start, 0.0);
    QCOMPARE(selection.end, 4.0);
    QVERIFY(!initialClipSelection(0.05, 0.0).valid());
}

void TestClipExport::movingTheClipPreservesItsDurationAtBothWindowEdges() {
    using namespace mplayer::quick;
    const auto initial = initialClipSelection(1800.0, 900.0);
    const auto last = shiftedClipSelection(initial, 10000.0);
    QCOMPARE(last.end, initial.windowEnd);
    QCOMPARE(last.length(), initial.length());
    const auto first = shiftedClipSelection(last, -20000.0);
    QCOMPARE(first.start, initial.windowStart);
    QCOMPARE(first.length(), initial.length());
    QVERIFY(first.valid());
    QCOMPARE(first.windowEnd, initial.windowEnd);
}

void TestClipExport::crossingAnEditedBoundaryKeepsTheRequestedBoundary() {
    using namespace mplayer::quick;
    const auto initial = initialClipSelection(120.0, 40.0);
    const auto start = boundedClipSelection(initial, 70.0, initial.end, ClipBoundary::Start);
    QCOMPARE(start.start, 70.0);
    QVERIFY(start.end > start.start);
    QVERIFY(start.valid());
    const auto end = boundedClipSelection(start, start.start, 10.0, ClipBoundary::End);
    QCOMPARE(end.end, 10.0);
    QVERIFY(end.start < end.end);
    QVERIFY(end.valid());
}

void TestClipExport::nonFiniteEditsLeaveTheClipSelectionUnchanged() {
    using namespace mplayer::quick;
    const auto initial = initialClipSelection(120.0, 40.0);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto edited = boundedClipSelection(initial, nan, 90.0);
    QCOMPARE(edited.start, initial.start);
    QCOMPARE(edited.end, initial.end);
    const auto moved = shiftedClipSelection(initial, std::numeric_limits<double>::infinity());
    QCOMPARE(moved.start, initial.start);
    QCOMPARE(moved.end, initial.end);
    QVERIFY(!initialClipSelection(nan, 0.0).valid());
}

void TestClipExport::rejectsCutMomentWhenThereIsNoVideoTimeline() {
    mplayer::quick::PlayerController controller(QStringLiteral("Clip test"),
                                                QStringLiteral("unused.mp4"));
    QVERIFY(!controller.hasVideo());
    QVERIFY(!std::isfinite(controller.duration()) || controller.duration() <= 0.0);
    controller.openClipWindow();

    QCOMPARE(controller.statusText(),
             QStringLiteral("A seekable video timeline is not available"));
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        if (window->objectName() == QStringLiteral("clipWindow"))
            QFAIL("Cut moment opened without a video timeline");
    }
}

void TestClipExport::rejectsNonFiniteAndInvalidRanges() {
    const auto rejected = [](double start, double end) {
        ClipExport exporter;
        QSignalSpy finished(&exporter, &ClipExport::finished);
        ClipExportRequest request;
        request.source = QStringLiteral("unused.mp4");
        request.outputPath = QStringLiteral("unused-output.mp4");
        request.startSeconds = start;
        request.endSeconds = end;
        request.sourceSize = QSize(640, 360);
        exporter.start(request);
        if (!finishExport(finished, 3000)) return false;
        return !finished.constFirst().at(0).toBool() &&
               !finished.constFirst().at(2).toString().isEmpty();
    };
    QVERIFY(rejected(std::numeric_limits<double>::quiet_NaN(), 3.0));
    QVERIFY(rejected(0.0, std::numeric_limits<double>::infinity()));
    QVERIFY(rejected(3.0, 2.0));
    QVERIFY(rejected(-0.01, 2.0));
    QVERIFY(rejected(0.0, 60.001));
}

void TestClipExport::videoOnlySourceProducesBoundedPlayableMp4() {
    if (!requireRuntime()) return;
    QVERIFY2(QFileInfo::exists(fixtureClip()), qPrintable(fixtureClip()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("video-only.mp4"));

    ClipExport exporter;
    QSignalSpy finished(&exporter, &ClipExport::finished);
    ClipExportRequest request;
    request.source = fixtureClip();
    request.outputPath = path;
    request.startSeconds = 2.0;
    request.endSeconds = 4.0;
    request.sourceSize = QSize(1920, 1080);
    request.maximumHeight = 480;
    exporter.start(request);
    QVERIFY2(finishExport(finished), "video-only encoding did not finish");
    QVERIFY2(finished.constFirst().at(0).toBool(), qPrintable(finished.constFirst().at(2).toString()));

    double duration = 0.0;
    int width = 0;
    int height = 0;
    bool audio = false;
    QString decodeError;
    QVERIFY2(decodeAndInspect(path, duration, width, height, audio, decodeError),
             qPrintable(QStringLiteral("separate mpv decode failed: %1").arg(decodeError)));
    QVERIFY2(!audio, "a video-only source unexpectedly acquired an audio stream");
    QVERIFY2(duration >= 1.90 && duration <= 2.15,
             qPrintable(QStringLiteral("expected a two-second clip, got %1 s").arg(duration)));
    QCOMPARE(height, 480);
    QCOMPARE(width, 854);
}

void TestClipExport::aacSourcePreservesAudio() {
    if (!requireRuntime()) return;
    QVERIFY2(QFileInfo::exists(audioFixtureClip()), qPrintable(audioFixtureClip()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("with-audio.mp4"));
    QFile existing(path);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("replace only after encoding completes"), qint64(37));
    existing.close();

    ClipExport exporter;
    QSignalSpy finished(&exporter, &ClipExport::finished);
    ClipExportRequest request;
    request.source = audioFixtureClip();
    request.outputPath = path;
    request.replaceExisting = true;
    request.startSeconds = 2.0;
    request.endSeconds = 4.0;
    request.sourceSize = QSize(320, 180);
    request.maximumHeight = 480;
    exporter.start(request);
    QVERIFY2(finishExport(finished), "AAC-source encoding did not finish");
    QVERIFY2(finished.constFirst().at(0).toBool(), qPrintable(finished.constFirst().at(2).toString()));

    double duration = 0.0;
    int width = 0;
    int height = 0;
    bool audio = false;
    QString decodeError;
    QVERIFY2(decodeAndInspect(path, duration, width, height, audio, decodeError),
             qPrintable(QStringLiteral("separate mpv decode failed: %1").arg(decodeError)));
    QVERIFY(audio);
    QVERIFY2(duration >= 1.90 && duration <= 2.15,
             qPrintable(QStringLiteral("expected a two-second clip, got %1 s").arg(duration)));
    QCOMPARE(width, 320);
    QCOMPARE(height, 180);
}

void TestClipExport::selectedAudioTrackExportPreservesSignal() {
    if (!requireRuntime()) return;
    const QString source = QDir::toNativeSeparators(QStringLiteral(
        MIKAI_SOURCE_DIR "/tests/fixtures/media/two_audio_tones_320x180.mp4"));
    QVERIFY2(QFileInfo::exists(source), qPrintable(source));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const auto verifyTrack = [&](qint64 trackId, int expectedFrequency,
                                 const QString& name) -> bool {
        const QString outputPath = directory.filePath(name + QStringLiteral(".mp4"));
        ClipExport exporter;
        QSignalSpy finished(&exporter, &ClipExport::finished);
        ClipExportRequest request;
        request.source = source;
        request.outputPath = outputPath;
        request.startSeconds = 0.5;
        request.endSeconds = 1.5;
        request.sourceSize = QSize(320, 180);
        request.maximumHeight = 480;
        request.audioTrackId = trackId;
        exporter.start(request);
        if (!finishExport(finished)) {
            QTest::qFail(qPrintable(name + QStringLiteral(" export did not finish")),
                         __FILE__, __LINE__);
            return false;
        }
        if (!finished.constFirst().at(0).toBool()) {
            QTest::qFail(qPrintable(finished.constFirst().at(2).toString()),
                         __FILE__, __LINE__);
            return false;
        }

        const QString wavPath = directory.filePath(name + QStringLiteral(".wav"));
        DecodedPcm pcm;
        QString decodeError;
        if (!decodeAudioToPcm(outputPath, wavPath, pcm, decodeError)) {
            QTest::qFail(qPrintable(QStringLiteral("selected audio decode failed: %1")
                                         .arg(decodeError)),
                         __FILE__, __LINE__);
            return false;
        }
        const int firstSample = int(pcm.sampleRate / 10);
        const int sampleCount = int(pcm.sampleRate / 2);
        // Skip the first 0.1 s to avoid AAC encoder priming.
        if (pcm.monoSamples.size() < firstSample + sampleCount) {
            QTest::qFail(qPrintable(QStringLiteral("decoded PCM is too short: %1 mono samples")
                                         .arg(pcm.monoSamples.size())),
                         __FILE__, __LINE__);
            return false;
        }
        const double wanted = goertzelPower(pcm, firstSample, sampleCount, expectedFrequency);
        const double other = goertzelPower(pcm, firstSample, sampleCount,
                                           expectedFrequency == 440 ? 880 : 440);
        if (!(wanted > other * 4.0)) {
            QTest::qFail(qPrintable(QStringLiteral("%1Hz signal did not dominate for track %2 "
                                                   "(wanted power %3, other power %4)")
                                         .arg(expectedFrequency).arg(trackId).arg(wanted).arg(other)),
                         __FILE__, __LINE__);
            return false;
        }
        return true;
    };

    if (!verifyTrack(1, 440, QStringLiteral("english-track"))) return;
    QVERIFY(verifyTrack(2, 880, QStringLiteral("ukrainian-track")));
}

void TestClipExport::externalAudioOverridesEmbeddedAudio() {
    if (!requireRuntime()) return;
    QVERIFY2(QFileInfo::exists(audioFixtureClip()), qPrintable(audioFixtureClip()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString audioPath = directory.filePath(QStringLiteral("тон external audio.wav"));
    QVERIFY2(writeToneWav(audioPath), "could not create the external PCM WAV fixture");
    const QString outputPath = directory.filePath(QStringLiteral("external-audio.mp4"));

    ClipExport exporter;
    QSignalSpy finished(&exporter, &ClipExport::finished);
    ClipExportRequest request;
    request.source = audioFixtureClip();
    request.externalAudioPath = audioPath;
    request.outputPath = outputPath;
    request.startSeconds = 2.0;
    request.endSeconds = 4.0;
    request.sourceSize = QSize(320, 180);
    request.maximumHeight = 480;
    exporter.start(request);
    QVERIFY2(finishExport(finished), "external-audio encoding did not finish");
    QVERIFY2(finished.constFirst().at(0).toBool(), qPrintable(finished.constFirst().at(2).toString()));

    double duration = 0.0;
    int width = 0;
    int height = 0;
    bool audio = false;
    QString decodeError;
    QVERIFY2(decodeAndInspect(outputPath, duration, width, height, audio, decodeError),
             qPrintable(QStringLiteral("separate mpv decode failed: %1").arg(decodeError)));
    QVERIFY2(audio, "external-audio export did not contain a selected audio stream");
    QVERIFY2(duration >= 1.90 && duration <= 2.15,
             qPrintable(QStringLiteral("expected a two-second clip, got %1 s").arg(duration)));
}

void TestClipExport::missingExternalAudioPreservesExistingDestination() {
    if (!requireRuntime()) return;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString outputPath = directory.filePath(QStringLiteral("existing.mp4"));
    QFile destination(outputPath);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    QCOMPARE(destination.write("keep-existing-bytes"), qint64(19));
    destination.close();

    ClipExport exporter;
    QSignalSpy finished(&exporter, &ClipExport::finished);
    ClipExportRequest request;
    request.source = audioFixtureClip();
    request.externalAudioPath = directory.filePath(QStringLiteral("missing external.wav"));
    request.outputPath = outputPath;
    request.replaceExisting = true;
    request.startSeconds = 2.0;
    request.endSeconds = 4.0;
    request.sourceSize = QSize(320, 180);
    exporter.start(request);
    QVERIFY(finishExport(finished));
    QVERIFY(!finished.constFirst().at(0).toBool());
    QCOMPARE(existingBytes(outputPath), QByteArray("keep-existing-bytes"));
}

void TestClipExport::sourceFailurePreservesExistingDestination() {
    if (!requireRuntime()) return;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("existing.mp4"));
    QFile destination(path);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    QCOMPARE(destination.write("keep-existing-bytes"), qint64(19));
    destination.close();

    ClipExport exporter;
    QSignalSpy finished(&exporter, &ClipExport::finished);
    ClipExportRequest request;
    request.source = directory.filePath(QStringLiteral("missing-source.mp4"));
    request.outputPath = path;
    request.replaceExisting = true;
    request.startSeconds = 0.0;
    request.endSeconds = 1.0;
    request.sourceSize = QSize(640, 360);
    exporter.start(request);
    QVERIFY(finishExport(finished));
    QVERIFY(!finished.constFirst().at(0).toBool());
    QCOMPARE(existingBytes(path), QByteArray("keep-existing-bytes"));
}

void TestClipExport::noConsentPreservesExistingDestination() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("existing.mp4"));
    QFile destination(path);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    QCOMPARE(destination.write("keep-existing-bytes"), qint64(19));
    destination.close();

    ClipExport exporter;
    QSignalSpy finished(&exporter, &ClipExport::finished);
    ClipExportRequest request;
    request.source = QStringLiteral("unused-source.mp4");
    request.outputPath = path;
    request.startSeconds = 0.0;
    request.endSeconds = 1.0;
    request.sourceSize = QSize(640, 360);
    exporter.start(request);
    QVERIFY(finishExport(finished, 3000));
    QVERIFY(!finished.constFirst().at(0).toBool());
    QCOMPARE(existingBytes(path), QByteArray("keep-existing-bytes"));
}

void TestClipExport::danglingSymlinkIsPreservedWithoutConsent() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString target = directory.filePath(QStringLiteral("missing-target.mp4"));
    const QString link = directory.filePath(QStringLiteral("dangling.mp4"));
    if (!QFile::link(target, link) || !QFileInfo(link).isSymLink()) {
        QSKIP("Creating symbolic links is not permitted/supported on this host/user.");
    }
    QVERIFY(!QFileInfo(link).exists());

    ClipExport exporter;
    QSignalSpy finished(&exporter, &ClipExport::finished);
    ClipExportRequest request;
    request.source = QStringLiteral("unused-source.mp4");
    request.outputPath = link;
    request.startSeconds = 0.0;
    request.endSeconds = 1.0;
    request.sourceSize = QSize(640, 360);
    exporter.start(request);
    QVERIFY(finishExport(finished, 3000));
    QVERIFY(!finished.constFirst().at(0).toBool());
    QVERIFY(QFileInfo(link).isSymLink());
    QVERIFY(!QFileInfo(link).exists());
    QCOMPARE(QFileInfo(link).symLinkTarget(), target);
}

void TestClipExport::cancellationPreservesDestinationAndRemovesPartialOutput() {
    if (!requireRuntime()) return;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("existing.mp4"));
    QFile destination(path);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    QCOMPARE(destination.write("keep-existing-bytes"), qint64(19));
    destination.close();

    ClipExport exporter;
    QSignalSpy started(&exporter, &ClipExport::encodingStarted);
    QSignalSpy finished(&exporter, &ClipExport::finished);
    ClipExportRequest request;
    request.source = fixtureClip();
    request.outputPath = path;
    request.replaceExisting = true;
    request.startSeconds = 0.0;
    request.endSeconds = 12.0;
    request.sourceSize = QSize(1920, 1080);
    request.maximumHeight = 1080;
    exporter.start(request);
    QElapsedTimer timer;
    timer.start();
    while (started.isEmpty() && finished.isEmpty() && timer.elapsed() < 10000) started.wait(50);
    QVERIFY2(!started.isEmpty(), "encoding never became active, so cancellation was not exercised");
    exporter.cancel();
    QVERIFY(finishExport(finished));
    QVERIFY(!finished.constFirst().at(0).toBool());
    QCOMPARE(existingBytes(path), QByteArray("keep-existing-bytes"));
}

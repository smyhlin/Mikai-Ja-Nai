#pragma once

#include <native/bundle/NativePaths.hpp>
#include <native/status/NativeTypes.hpp>

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace mplayer::native {

enum class AnimeJanaiBuildLogEvent {
    None,
    TensorRtBuildStarted,
    TensorRtBuildFailed,
    SlotConfigured
};

inline constexpr qsizetype kAnimeJanaiBuildLogTailBytes = 16 * 1024;

inline QByteArray readAnimeJanaiBuildLogContents(QStringView path, QStringView ownedRoot) {
    const QFileInfo info(path.toString());
    const QString root = ownedRoot.toString();
    const QString absolutePath = info.absoluteFilePath();
    if (!info.isAbsolute() || !info.fileName().endsWith(QLatin1String(".engine.build.log"),
                                                        Qt::CaseInsensitive) ||
        !info.isFile() || info.isSymLink() || root.isEmpty() ||
        !pathWithinRoot(root, absolutePath) || !pathHasNoReparsePoints(root, absolutePath)) {
        return {};
    }

    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0) return {};
    const qint64 fileSize = file.size();
    const qint64 firstSize = qMin<qint64>(fileSize, kAnimeJanaiBuildLogTailBytes / 2);
    QByteArray contents = file.read(firstSize);
    if (fileSize > firstSize) {
        const qint64 lastStart = qMax<qint64>(firstSize, fileSize - kAnimeJanaiBuildLogTailBytes / 2);
        if (!file.seek(lastStart)) return {};
        contents += file.read(fileSize - lastStart);
    }
    if (file.error() != QFile::NoError) return {};
    return contents;
}

inline QStringList animeJanaiBuildLogErrorLines(const QString& contents) {
    const QStringList sourceLines = contents.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QStringList errors;
    const auto isBoilerplate = [](const QString& line) {
        return line.startsWith(QLatin1String("&&&&")) ||
               line.startsWith(QLatin1String("[aji] build process exit code:"),
                               Qt::CaseInsensitive) ||
               line.contains(QLatin1String("Network And Config setup failed"),
                             Qt::CaseInsensitive) ||
               line.contains(QLatin1String("Building engine failed"), Qt::CaseInsensitive) ||
               line.contains(QLatin1String("Failed to create engine from model or file"),
                             Qt::CaseInsensitive) ||
               line.contains(QLatin1String("Engine set up failed"), Qt::CaseInsensitive);
    };
    for (const QString& line : sourceLines) {
        const qsizetype marker = line.indexOf(QLatin1String("[E]"), 0, Qt::CaseInsensitive);
        if (marker < 0) continue;
        const QString error = line.mid(marker).trimmed();
        if (!isBoilerplate(error) && !errors.contains(error)) errors.append(error);
    }
    while (errors.size() > 3) errors.removeAt(1);
    return errors;
}

inline QStringList readAnimeJanaiBuildLogTail(QStringView path, QStringView ownedRoot) {
    const QByteArray contents = readAnimeJanaiBuildLogContents(path, ownedRoot);
    if (contents.isEmpty()) return {};
    const QString text = QString::fromUtf8(contents);
    return animeJanaiBuildLogErrorLines(text);
}

/// The newest engine build log beside the model when that build ran out of GPU memory.
/// The fork reports a bare createExecutionContext failure for this case, so the log is the
/// only place the cause appears. Only the newest log is consulted: an older build's memory
/// failure must not be attributed to the build that just ran.
inline QString animeJanaiOutOfMemoryEngineBuildEvidence(QStringView modelDir,
                                                       QStringView ownedRoot) {
    const QString root = ownedRoot.toString();
    const QString directory = QFileInfo(modelDir.toString()).absoluteFilePath();
    if (root.isEmpty() || modelDir.isEmpty() || !QFileInfo(directory).isDir() ||
        !pathWithinRoot(root, directory) || !pathHasNoReparsePoints(root, directory)) {
        return {};
    }

    QFileInfo newestLog;
    const QFileInfoList logs =
        QDir(directory).entryInfoList({QStringLiteral("*.engine.build.log")}, QDir::Files);
    for (const QFileInfo& log : logs) {
        if (!newestLog.exists() || log.lastModified() > newestLog.lastModified()) newestLog = log;
    }
    if (!newestLog.exists()) return {};

    const QByteArray contents =
        readAnimeJanaiBuildLogContents(newestLog.absoluteFilePath(), ownedRoot);
    if (contents.isEmpty()) return {};
    const QString text = QString::fromUtf8(contents);
    const QLatin1String marker("out of memory");
    if (!text.contains(marker, Qt::CaseInsensitive)) return {};

    QStringList evidence;
    for (const QString& line : animeJanaiBuildLogErrorLines(text)) {
        if (line.contains(marker, Qt::CaseInsensitive) && !evidence.contains(line)) {
            evidence.append(line);
        }
    }
    if (evidence.isEmpty()) {
        for (const QString& line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            if (line.contains(marker, Qt::CaseInsensitive)) {
                evidence.append(line.trimmed());
                break;
            }
        }
    }
    return evidence.join(QLatin1Char('\n'));
}

inline QString animeJanaiBuildFailureEnginePath(QStringView prefix, QStringView line) {
    if (prefix.compare(QLatin1String("animejanai"), Qt::CaseInsensitive) == 0) {
        if (!line.startsWith(QLatin1String("[aji] "))) return {};
        line = line.sliced(6);
    } else if (prefix.compare(QLatin1String("aji"), Qt::CaseInsensitive) != 0) {
        return {};
    }
    const QLatin1String marker("background engine build failed: ");
    const QLatin1String suffix(" (see .build.log)");
    if (!line.startsWith(marker, Qt::CaseInsensitive) ||
        !line.endsWith(suffix, Qt::CaseInsensitive)) return {};
    const QString path = line.mid(marker.size(), line.size() - marker.size() - suffix.size()).toString();
    if (!QFileInfo(path).isAbsolute() ||
        !path.endsWith(QLatin1String(".engine"), Qt::CaseInsensitive) ||
        path.contains(QLatin1Char('\n')) || path.contains(QLatin1Char('\r'))) return {};
    return path;
}

inline AnimeJanaiBuildLogEvent classifyAnimeJanaiBuildLog(QStringView prefix,
                                                          QStringView line,
                                                          Backend backend,
                                                          int slot) {
    QString text = prefix.toString();
    text += QLatin1Char(' ');
    text += line.toString();
    const QString lower = text.toLower();

    if (backend == Backend::TensorRT &&
        (lower.contains(QLatin1String("building tensorrt engine for ")) ||
         lower.contains(QLatin1String("building engine:")) ||
         lower.contains(QLatin1String("background engine build:")))) {
        return AnimeJanaiBuildLogEvent::TensorRtBuildStarted;
    }

    if (backend == Backend::TensorRT && !animeJanaiBuildFailureEnginePath(prefix, line).isEmpty()) {
        return AnimeJanaiBuildLogEvent::TensorRtBuildFailed;
    }

    const QString marker = QStringLiteral("configured slot %1:").arg(slot);
    if (lower.contains(marker) && lower.contains(QLatin1String(" -> "))) {
        return AnimeJanaiBuildLogEvent::SlotConfigured;
    }
    return AnimeJanaiBuildLogEvent::None;
}

enum class AnimeJanaiBuildGateState { WaitingForConfiguredSlot, Building, Ready, Failed };

class AnimeJanaiBuildGate {
public:
    AnimeJanaiBuildLogEvent observeLog(QStringView prefix, QStringView line, Backend backend,
                                       int slot) {
        if (failed()) return AnimeJanaiBuildLogEvent::None;
        const AnimeJanaiBuildLogEvent event =
            classifyAnimeJanaiBuildLog(prefix, line, backend, slot);
        if (event == AnimeJanaiBuildLogEvent::TensorRtBuildStarted) {
            state_ = AnimeJanaiBuildGateState::Building;
            rifeActive_ = false;
            backgroundBuildFinished_ = false;
        } else if (event == AnimeJanaiBuildLogEvent::TensorRtBuildFailed) {
            state_ = AnimeJanaiBuildGateState::Failed;
            failureLogPath_ = animeJanaiBuildFailureEnginePath(prefix, line) + QStringLiteral(".build.log");
        } else if (event == AnimeJanaiBuildLogEvent::SlotConfigured) {
            if (backend != Backend::TensorRT) return AnimeJanaiBuildLogEvent::None;
            state_ = AnimeJanaiBuildGateState::Ready;
            rifeActive_ = false;
        } else if (backend == Backend::TensorRT) {
            const QStringView text = line.trimmed();
            if (text.compare(QLatin1String("background engine build finished; reconfiguring"),
                             Qt::CaseInsensitive) == 0) {
                backgroundBuildFinished_ = buildActive();
                rifeActive_ = false;
            } else if (text.startsWith(QLatin1String("RIFE active: "), Qt::CaseInsensitive) &&
                       text.endsWith(QLatin1String(" interpolation"), Qt::CaseInsensitive)) {
                // Async builds can configure passthrough before their model is ready.
                rifeActive_ = !buildActive() || backgroundBuildFinished_;
            } else if (rifeActive_ &&
                       text.startsWith(QLatin1String("No chain active for "),
                                       Qt::CaseInsensitive) &&
                       text.endsWith(QStringLiteral(" slot %1 (passthrough)").arg(slot),
                                     Qt::CaseInsensitive)) {
                // Interpolation-only slots omit the spatial "Configured slot" log.
                state_ = AnimeJanaiBuildGateState::Ready;
                rifeActive_ = false;
                return AnimeJanaiBuildLogEvent::SlotConfigured;
            }
        }
        return event;
    }

    void fail() { state_ = AnimeJanaiBuildGateState::Failed; }

    bool buildActive() const { return state_ == AnimeJanaiBuildGateState::Building; }
    bool failed() const { return state_ == AnimeJanaiBuildGateState::Failed; }
    bool readyToMeasure() const { return state_ == AnimeJanaiBuildGateState::Ready; }
    QString failureLogPath() const { return failureLogPath_; }

private:
    AnimeJanaiBuildGateState state_ = AnimeJanaiBuildGateState::WaitingForConfiguredSlot;
    bool rifeActive_ = false;
    bool backgroundBuildFinished_ = false;
    QString failureLogPath_;
};

}  // namespace mplayer::native

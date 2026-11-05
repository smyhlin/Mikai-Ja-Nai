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
    /// TensorRT-RTX prepares a cached engine in-process before its first use in
    /// a session; the fork defers the configure until that warm-up finishes.
    TensorRtEnginePrepStarted,
    /// A TensorRT-RTX in-session engine warm-up failed. Upstream then loads the
    /// engine on the caller thread, so this is evidence to surface and keep,
    /// not a terminal verdict; the load reports the authoritative error.
    TensorRtEngineWarmupFailed,
    TensorRtBuildFailed,
    SlotConfigured,
    /// The dispatcher replaced the requested TensorRT-RTX backend with the
    /// classic TensorRT one. That is a named failure of the requested backend,
    /// never an RTX success.
    BackendSubstituted
};

/// Build-progress events that mean the core is preparing an engine right now:
/// the caller pauses playback, holds a frame and can time engine preparation.
inline constexpr bool animeJanaiEnginePrepEvent(AnimeJanaiBuildLogEvent event) {
    return event == AnimeJanaiBuildLogEvent::TensorRtBuildStarted ||
           event == AnimeJanaiBuildLogEvent::TensorRtEnginePrepStarted;
}

/// Events that prove the requested backend did not produce a usable engine.
inline constexpr bool animeJanaiBuildFailureEvent(AnimeJanaiBuildLogEvent event) {
    return event == AnimeJanaiBuildLogEvent::TensorRtBuildFailed ||
           event == AnimeJanaiBuildLogEvent::BackendSubstituted;
}

inline constexpr qsizetype kAnimeJanaiBuildLogTailBytes = 16 * 1024;

/// The classic backend writes `<engine>.build.log`; TensorRT-RTX writes
/// `<engine>.rtxengine.build.log` for builds and `<engine>.rtxengine.warm.log`
/// for in-session warm-ups.
inline bool isAnimeJanaiEngineLogName(const QString& fileName) {
    const QString lower = fileName.toLower();
    return lower.endsWith(QLatin1String(".engine.build.log")) ||
           lower.endsWith(QLatin1String(".rtxengine.build.log")) ||
           lower.endsWith(QLatin1String(".rtxengine.warm.log"));
}

/// Both backends name their serialized engine with the same trailing token, so a
/// path is only accepted when it carries one of those exact suffixes.
inline bool isAnimeJanaiEnginePath(const QString& path) {
    return path.endsWith(QLatin1String(".engine"), Qt::CaseInsensitive) ||
           path.endsWith(QLatin1String(".rtxengine"), Qt::CaseInsensitive);
}

inline QByteArray readAnimeJanaiBuildLogContents(QStringView path, QStringView ownedRoot) {
    const QFileInfo info(path.toString());
    const QString root = ownedRoot.toString();
    const QString absolutePath = info.absoluteFilePath();
    if (!info.isAbsolute() || !isAnimeJanaiEngineLogName(info.fileName()) ||
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
        QDir(directory).entryInfoList({QStringLiteral("*.engine.build.log"),
                                       QStringLiteral("*.rtxengine.build.log"),
                                       QStringLiteral("*.rtxengine.warm.log")},
                                      QDir::Files);
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

/// The engine log a failed build or warm-up wrote, when the fork named one.
/// Classic builds report ".build.log"; TensorRT-RTX in-process builds report
/// either that or the in-session ".warm.log".
inline QString animeJanaiBuildFailureLogPath(QStringView prefix, QStringView line) {
    if (prefix.compare(QLatin1String("animejanai"), Qt::CaseInsensitive) == 0) {
        if (!line.startsWith(QLatin1String("[aji] "))) return {};
        line = line.sliced(6);
    } else if (prefix.compare(QLatin1String("aji"), Qt::CaseInsensitive) != 0) {
        return {};
    }
    struct Marker {
        QLatin1String text;
        QLatin1String suffix;
        QLatin1String logSuffix;
    };
    static const Marker markers[] = {
        {QLatin1String("background engine build failed: "), QLatin1String(" (see .build.log)"),
         QLatin1String(".build.log")},
        {QLatin1String("background engine warm-up failed: "), QLatin1String(" (see .warm.log)"),
         QLatin1String(".warm.log")},
    };
    for (const Marker& marker : markers) {
        if (!line.startsWith(marker.text, Qt::CaseInsensitive) ||
            !line.endsWith(marker.suffix, Qt::CaseInsensitive)) continue;
        const QString path =
            line.mid(marker.text.size(), line.size() - marker.text.size() - marker.suffix.size())
                .toString();
        if (!QFileInfo(path).isAbsolute() || !isAnimeJanaiEnginePath(path) ||
            path.contains(QLatin1Char('\n')) || path.contains(QLatin1Char('\r'))) return {};
        return path + marker.logSuffix;
    }
    return {};
}

inline QString animeJanaiBuildFailureEnginePath(QStringView prefix, QStringView line) {
    const QString logPath = animeJanaiBuildFailureLogPath(prefix, line);
    if (logPath.isEmpty()) return {};
    if (logPath.endsWith(QLatin1String(".build.log"), Qt::CaseInsensitive))
        return logPath.first(logPath.size() - 10);
    if (logPath.endsWith(QLatin1String(".warm.log"), Qt::CaseInsensitive))
        return logPath.first(logPath.size() - 9);
    return {};
}

/// The passthrough note a latched build failure logs on a later configure. It
/// names the engine log and states that the chain is not running, which is the
/// only failure signal left once the fork stops failing the filter.
inline QString animeJanaiEngineFailedLogPath(QStringView line) {
    const QStringView trimmed = line.trimmed();
    if (!trimmed.startsWith(QLatin1String("engine "), Qt::CaseInsensitive) ||
        !trimmed.contains(QLatin1String(" failed for "), Qt::CaseInsensitive))
        return {};
    const QLatin1StringView tail("); playing without this chain");
    if (!trimmed.endsWith(tail, Qt::CaseInsensitive)) return {};
    const qsizetype see = trimmed.lastIndexOf(QLatin1String("(see "), -1, Qt::CaseInsensitive);
    if (see < 0) return {};
    const QString path =
        trimmed.mid(see + 5, trimmed.size() - (see + 5) - tail.size()).toString();
    if (!QFileInfo(path).isAbsolute() || !isAnimeJanaiEngineLogName(path) ||
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
    const bool tensorRtFamily = isTensorRtFamily(backend);

    // The upstream dispatcher keeps playback alive on the classic backend when
    // the requested TensorRT-RTX one fails to load or create. The app must not
    // report that as the RTX backend running.
    if (backend == Backend::TensorRTRTX &&
        lower.contains(QLatin1String("tensorrt-rtx failed")) &&
        lower.contains(QLatin1String("using tensorrt instead"))) {
        return AnimeJanaiBuildLogEvent::BackendSubstituted;
    }

    if (tensorRtFamily &&
        (lower.contains(QLatin1String("building tensorrt engine for ")) ||
         lower.contains(QLatin1String("building tensorrt-rtx engine for ")) ||
         lower.contains(QLatin1String("building engine:")) ||
         lower.contains(QLatin1String("background engine build:")))) {
        return AnimeJanaiBuildLogEvent::TensorRtBuildStarted;
    }

    if (tensorRtFamily && lower.contains(QLatin1String("preparing tensorrt-rtx engine for "))) {
        return AnimeJanaiBuildLogEvent::TensorRtEnginePrepStarted;
    }

    if (tensorRtFamily) {
        const QString failureLogPath = animeJanaiBuildFailureLogPath(prefix, line);
        if (failureLogPath.endsWith(QLatin1String(".warm.log"), Qt::CaseInsensitive))
            return AnimeJanaiBuildLogEvent::TensorRtEngineWarmupFailed;
        // TensorRT-RTX latches a failed engine to passthrough and never reports
        // it again through a filter error, so its own note is the last failure
        // signal. The classic backend keeps its existing behaviour.
        const bool latchedRtxFailure =
            backend == Backend::TensorRTRTX && !animeJanaiEngineFailedLogPath(line).isEmpty();
        if (!failureLogPath.isEmpty() || latchedRtxFailure)
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
        if (animeJanaiEnginePrepEvent(event)) {
            state_ = AnimeJanaiBuildGateState::Building;
            rifeActive_ = false;
            backgroundBuildFinished_ = false;
        } else if (event == AnimeJanaiBuildLogEvent::BackendSubstituted) {
            // Nothing was built: the requested backend never ran.
            state_ = AnimeJanaiBuildGateState::Failed;
            backendSubstituted_ = true;
            failureLogPath_.clear();
        } else if (event == AnimeJanaiBuildLogEvent::TensorRtEngineWarmupFailed) {
            // Not terminal: the same cached engine is loaded on the caller
            // thread next, and that load owns the real verdict.
            warmupLogPath_ = animeJanaiBuildFailureLogPath(prefix, line);
        } else if (event == AnimeJanaiBuildLogEvent::TensorRtBuildFailed) {
            state_ = AnimeJanaiBuildGateState::Failed;
            failureLogPath_ = animeJanaiBuildFailureLogPath(prefix, line);
            if (failureLogPath_.isEmpty())
                failureLogPath_ = animeJanaiEngineFailedLogPath(line);
        } else if (event == AnimeJanaiBuildLogEvent::SlotConfigured) {
            if (!isTensorRtFamily(backend)) return AnimeJanaiBuildLogEvent::None;
            state_ = AnimeJanaiBuildGateState::Ready;
            rifeActive_ = false;
        } else if (isTensorRtFamily(backend)) {
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
    /// The requested backend was replaced by another one rather than failing to
    /// build; there is no build log to read in that case.
    bool backendSubstituted() const { return backendSubstituted_; }
    QString failureLogPath() const { return failureLogPath_; }
    /// The in-session warm-up log named by a warm-up failure, still readable
    /// when a later error reports the real load failure.
    QString warmupLogPath() const { return warmupLogPath_; }

private:
    AnimeJanaiBuildGateState state_ = AnimeJanaiBuildGateState::WaitingForConfiguredSlot;
    bool backendSubstituted_ = false;
    QString warmupLogPath_;
    bool rifeActive_ = false;
    bool backgroundBuildFinished_ = false;
    QString failureLogPath_;
};

}  // namespace mplayer::native

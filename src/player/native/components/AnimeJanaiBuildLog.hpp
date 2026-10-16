#pragma once

#include <native/status/NativeTypes.hpp>

#include <QString>
#include <QStringView>

namespace mplayer::native {

enum class AnimeJanaiBuildLogEvent { None, TensorRtBuildStarted, SlotConfigured };

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

private:
    AnimeJanaiBuildGateState state_ = AnimeJanaiBuildGateState::WaitingForConfiguredSlot;
    bool rifeActive_ = false;
    bool backgroundBuildFinished_ = false;
};

}  // namespace mplayer::native

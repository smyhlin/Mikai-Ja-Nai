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
        } else if (event == AnimeJanaiBuildLogEvent::SlotConfigured) {
            if (backend != Backend::TensorRT) return AnimeJanaiBuildLogEvent::None;
            state_ = AnimeJanaiBuildGateState::Ready;
        }
        return event;
    }

    void fail() { state_ = AnimeJanaiBuildGateState::Failed; }

    bool buildActive() const { return state_ == AnimeJanaiBuildGateState::Building; }
    bool failed() const { return state_ == AnimeJanaiBuildGateState::Failed; }
    bool readyToMeasure() const { return state_ == AnimeJanaiBuildGateState::Ready; }

private:
    AnimeJanaiBuildGateState state_ = AnimeJanaiBuildGateState::WaitingForConfiguredSlot;
};

}  // namespace mplayer::native

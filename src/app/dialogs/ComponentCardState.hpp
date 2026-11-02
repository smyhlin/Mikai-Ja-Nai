#pragma once

#include <QStringList>
#include <QtGlobal>

#include <optional>

namespace mikai::app::components {

enum class Readiness {
    Unknown,
    Missing,
    Downloading,
    Downloaded,
    Verifying,
    Verified,
    Failed,
};

struct TransferAxis {
    int active = 0;
    int queued = 0;
    int failed = 0;
    int cancelled = 0;
    int installed = 0;
    int unmetered = 0;
    qint64 received = 0;
    qint64 total = 0;
};

struct InventoryAxis {
    bool known = false;
    int installed = 0;
    int verified = 0;
    int total = 0;
};

struct CardState {
    bool verificationRunning = false;
    TransferAxis transfer;
    InventoryAxis inventory;
    QStringList missing;
};

inline Readiness cardReadiness(const CardState& state) {
    if (state.transfer.failed > 0) return Readiness::Failed;
    if (state.verificationRunning) return Readiness::Verifying;
    if (state.transfer.active > 0 || state.transfer.queued > 0)
        return Readiness::Downloading;
    if (state.inventory.known) {
        if (state.inventory.total > 0 && state.inventory.verified >= state.inventory.total)
            return Readiness::Verified;
        if (state.inventory.installed > 0) return Readiness::Downloaded;
        if (state.transfer.installed > 0) return Readiness::Downloaded;
        if (state.inventory.total > 0) return Readiness::Missing;
    }
    if (state.transfer.installed > 0) return Readiness::Downloaded;
    return Readiness::Unknown;
}

/// `std::nullopt` means indeterminate. Failed or cancelled runs answer zero in
/// every case, and an in-flight transfer is capped at 99: only a published,
/// disk-confirmed terminal item may read 100.
inline std::optional<int> componentProgressPercent(const CardState& state) {
    if (state.transfer.failed > 0 || state.transfer.cancelled > 0) return 0;
    if (state.verificationRunning) return std::nullopt;
    if (state.transfer.active > 0 || state.transfer.queued > 0) {
        if (state.transfer.total <= 0) return std::nullopt;
        const qint64 percent = state.transfer.received * 100 / state.transfer.total;
        return static_cast<int>(qBound(qint64{0}, percent, qint64{99}));
    }
    if (state.transfer.installed > 0) {
        const bool confirmed = state.inventory.known && state.inventory.installed > 0 &&
                               state.inventory.installed >= state.transfer.installed;
        return confirmed ? 100 : 0;
    }
    return std::nullopt;
}

struct ProgressBarDecision {
    bool visible = false;
    bool indeterminate = false;
    int percent = 0;
    bool failed = false;
};

inline ProgressBarDecision componentProgressBar(const CardState& state) {
    ProgressBarDecision decision;
    const bool hasItems = state.transfer.active > 0 || state.transfer.queued > 0 ||
                          state.transfer.failed > 0 || state.transfer.cancelled > 0 ||
                          state.transfer.installed > 0;
    if (!hasItems && !state.verificationRunning) return decision;
    decision.visible = true;
    if (cardReadiness(state) == Readiness::Failed) {
        decision.failed = true;
        return decision;
    }
    const std::optional<int> percent = componentProgressPercent(state);
    if (!percent.has_value()) {
        decision.indeterminate = true;
        return decision;
    }
    decision.percent = *percent;
    return decision;
}

}  // namespace mikai::app::components

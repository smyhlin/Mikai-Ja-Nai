#pragma once

#include <QStringList>
#include <QtGlobal>

#include <optional>

namespace mikai::app::components {

/// The one displayed truth a component card has, derived from three explicit
/// axes rather than from whichever member was touched last:
///
///  * Readiness - what the user may conclude about the card;
///  * Transfer  - the in-memory acquisition run (queued/active/terminal items);
///  * Inventory - what the disk actually holds and what hashing proved.
///
/// The card text, badge and progress bar all read this value, so "installed"
/// and "0/5" can no longer come from two different sources in one frame.
enum class Readiness {
    Unknown,     ///< Nothing is known yet: no inventory, no run.
    Missing,     ///< The inventory is authoritative and holds nothing.
    Downloading, ///< A transfer is active or queued.
    Downloaded,  ///< Bytes reached the store, but hashing has not proved them.
    Verifying,   ///< Hashing is running right now.
    Verified,    ///< The inventory is complete and proved by digest.
    Failed,      ///< At least one item failed; never rendered as complete.
};

/// The acquisition axis. Byte figures include only Active and Installed items:
/// a failed or cancelled transfer never contributes bytes, so an all-failed run
/// cannot add up to a full bar.
struct TransferAxis {
    int active = 0;
    int queued = 0;
    int failed = 0;
    int cancelled = 0;
    int installed = 0;
    /// Active items that reported no byte accounting (a byte-less phase event).
    /// They keep the label truthful without inventing a percentage.
    int unmetered = 0;
    qint64 received = 0;
    qint64 total = 0;
};

/// The disk axis. `known` is false until an inventory snapshot landed; a card
/// may not claim 100 % against an inventory it never read.
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
    /// Stable names the closure/inventory reports missing.
    QStringList missing;
};

/// What a card should say about readiness. Ordered deliberately: a failure is
/// never hidden behind activity, and verification is never reported as a
/// download.
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

/// The percentage a card bar may show, or `std::nullopt` for indeterminate.
///
/// Rules, in order:
///  * a verifying card is indeterminate - never a full bar;
///  * an active/queued transfer with a known total reports its own bytes;
///  * an active/queued transfer with no total is indeterminate;
///  * 100 % needs at least one terminal Installed item AND a disk inventory
///    that agrees, so a fresh "0/5" label can never sit beside a full bar;
///  * anything else stays at zero; a failed or cancelled run never reads 100.
inline std::optional<int> componentProgressPercent(const CardState& state) {
    const bool transferActivity =
        state.transfer.active > 0 || state.transfer.queued > 0;
    if (state.verificationRunning) return std::nullopt;
    if (transferActivity) {
        if (state.transfer.total <= 0) return std::nullopt;
        const qint64 percent = state.transfer.received * 100 / state.transfer.total;
        return static_cast<int>(qBound<qint64>(0, percent, 100));
    }
    if (state.transfer.installed > 0) {
        const bool confirmed = state.inventory.known && state.inventory.installed > 0 &&
                               state.inventory.installed >= state.transfer.installed;
        return confirmed ? 100 : 0;
    }
    if (state.transfer.failed > 0 || state.transfer.cancelled > 0) return 0;
    return std::nullopt;
}

/// The complete bar decision, so the widget never invents a third state.
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
    if (!hasItems && !state.verificationRunning) {
        // Nothing has happened for this card: no meter, no fabricated 100 %.
        return decision;
    }
    decision.visible = true;
    const Readiness readiness = cardReadiness(state);
    if (readiness == Readiness::Failed) {
        decision.failed = true;
        decision.percent = 0;
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

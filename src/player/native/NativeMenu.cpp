#include "NativeMenu.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>

namespace mplayer {
namespace native {
namespace menu {

namespace {

/// Every literal handed here MUST be wrapped in QT_TRANSLATE_NOOP, which lupdate
/// reads; at run time it is the literal itself.
QString translated(const char* message) {
    return QCoreApplication::translate("NativeMenu", message);
}

/// Backend names are proper nouns and are never translated; the stable name is what
/// a document carries.
QString backendLabel(Backend backend) {
    switch (backend) {
        case Backend::TensorRT: return QStringLiteral("TensorRT");
        case Backend::DirectML: return QStringLiteral("DirectML");
    }
    return QString();
}

QString variantLabel(RifeVariant variant) {
    switch (variant) {
        case RifeVariant::Full: return translated(QT_TRANSLATE_NOOP("NativeMenu", "Full"));
        case RifeVariant::Lite: return translated(QT_TRANSLATE_NOOP("NativeMenu", "Lite"));
    }
    return QString();
}

/// The human-readable state; the enum stays logic and `stableName()` stays the
/// protocol.
QString uiStateLabel(NativeState state) {
    switch (state) {
        case NativeState::Off: return translated(QT_TRANSLATE_NOOP("NativeMenu", "Off"));
        case NativeState::Probe: return translated(QT_TRANSLATE_NOOP("NativeMenu", "Probe"));
        case NativeState::Preparing:
            return translated(QT_TRANSLATE_NOOP("NativeMenu", "Preparing"));
        case NativeState::NeedsPreparation:
            return translated(QT_TRANSLATE_NOOP("NativeMenu", "Needs preparation"));
        case NativeState::Active: return translated(QT_TRANSLATE_NOOP("NativeMenu", "Active"));
        case NativeState::Suspended:
            return translated(QT_TRANSLATE_NOOP("NativeMenu", "Suspended"));
        case NativeState::Failed: return translated(QT_TRANSLATE_NOOP("NativeMenu", "Failed"));
        case NativeState::Unsupported:
            return translated(QT_TRANSLATE_NOOP("NativeMenu", "Unsupported"));
        case NativeState::Deactivated:
            return translated(QT_TRANSLATE_NOOP("NativeMenu", "Deactivated"));
    }
    return QString();
}

/// The one open hop behind every RIFE row: the graphs are acquired and byte-verified,
/// but the conversion-to-original-weight lineage is open, so they are not Ready.
QString rifeLineageReason() {
    return translated(QT_TRANSLATE_NOOP(
        "NativeMenu",
        "Not Ready: the graphs are acquired and byte-verified, but the "
        "conversion-to-original-weight lineage is still open, so they cannot be exposed "
        "as Ready."));
}

/// Prefers the selection's own fact, then the producer's engine reason, then the
/// runtime's.
QString refusalReason(const Inputs& in, const NativeSelectionView& view) {
    if (!view.availability.reason.isEmpty()) {
        return view.availability.reason;
    }
    if (!in.availability.engineReason.isEmpty()) {
        return in.availability.engineReason;
    }
    if (!in.runtime.reason.isEmpty()) {
        return in.runtime.reason;
    }
    return translated(QT_TRANSLATE_NOOP(
        "NativeMenu",
        "No native processing engine is available in this build."));
}

/// Only the filter's own snapshot says Active; eligibility is not activation.
bool snapshotActive(const Inputs& in) {
    return in.status.snapshot.has_value() && in.status.snapshot->state == NativeState::Active;
}

/// The fact blocking a capability row: engine first, then runtime, then the option's
/// own state.
QString capabilityReason(const Inputs& in, const AvailabilityState& option) {
    if (!option.isUsable() && option.hasMandatoryReason()) {
        return option.reason;
    }
    if (!in.availability.engineReason.isEmpty()) {
        return in.availability.engineReason;
    }
    if (!in.runtime.reason.isEmpty()) {
        return in.runtime.reason;
    }
    return translated(QT_TRANSLATE_NOOP(
        "NativeMenu",
        "No native processing engine is available in this build."));
}

QString joinStatusParts(const QStringList& parts) {
    QStringList present;
    for (const QString& part : parts) {
        if (!part.isEmpty()) {
            present.append(part);
        }
    }
    return present.join(QStringLiteral(" · "));
}

}  // namespace

QString actionName(Action action) {
    switch (action) {
        case Action::None: return QString();
        case Action::TurnOff: return QStringLiteral("off");
        case Action::TurnOn: return QStringLiteral("on");
        case Action::SetBackend: return QStringLiteral("backend");
        case Action::SetRecipe: return QStringLiteral("recipe");
    }
    return QString();
}

Action actionFromName(const QString& name) {
    if (name == QLatin1String("off")) return Action::TurnOff;
    if (name == QLatin1String("on")) return Action::TurnOn;
    if (name == QLatin1String("backend")) return Action::SetBackend;
    if (name == QLatin1String("recipe")) return Action::SetRecipe;
    return Action::None;
}

QString defaultToggleShortcut() {
    // Offered only through the stored binding path; the player installs it only if
    // the shared collision check accepts it.
    return QStringLiteral("Ctrl+Shift+J");
}

QString toggleShortcut(const NativePreferences& preferences) {
    // A stored decision wins even when "unbound": the default is only for a store
    // that holds no decision at all.
    if (preferences.toggleProcessingShortcutStored) {
        return preferences.toggleProcessingShortcut;
    }
    return preferences.toggleProcessingShortcut.isEmpty() ? defaultToggleShortcut()
                                                          : preferences.toggleProcessingShortcut;
}

bool ownsLevel(const QString& levelId) {
    return levelId == QLatin1String(kNativeLevelId) || levelId == QLatin1String(kRifeLevelId);
}

QString categoryTooltip(const QString& levelId, const Inputs& in) {
    if (levelId == QLatin1String(kNativeLevelId)) {
        const NativeSelectionView view = evaluateNativeSelection(in.preferences, in.availability);
        return translated(QT_TRANSLATE_NOOP("NativeMenu", "Native processing (AnimeJaNai). %1"))
            .arg(refusalReason(in, view));
    }
    if (levelId == QLatin1String(kRifeLevelId)) {
        return translated(QT_TRANSLATE_NOOP(
                   "NativeMenu",
                   "Frame interpolation. ")) +
               rifeLineageReason();
    }
    return QString();
}

QString currentRowId(const NativePreferences& preferences) {
    if (!preferences.enabled) {
        return QStringLiteral("native/off");
    }
    if (!preferences.recipeId.isEmpty()) {
        return QStringLiteral("native/recipe/") + preferences.recipeId;
    }
    return QStringLiteral("native/on");
}

QVector<Row> nativeRows(const Inputs& in) {
    QVector<Row> rows;
    const NativeSelectionView view = evaluateNativeSelection(in.preferences, in.availability);
    const QString refusal = refusalReason(in, view);
    // Only Off - which changes no pixels - is choosable, and every row that would run
    // something says why; eligibility is not activation.
    const bool applicable = view.requestAvailable;
    const bool engineReady = in.availability.engineAvailable;
    const bool running = snapshotActive(in);

    Row off;
    off.id = QStringLiteral("native/off");
    off.label = translated(QT_TRANSLATE_NOOP("NativeMenu", "Off (Original picture)"));
    off.tooltip = translated(QT_TRANSLATE_NOOP(
        "NativeMenu",
        "Turn native processing off. The Anime4K shaders, the stream and the current "
        "media are left exactly as they are."));
    off.action = Action::TurnOff;
    off.current = !in.preferences.enabled;
    rows.append(off);

    Row on;
    on.id = QStringLiteral("native/on");
    on.label = translated(QT_TRANSLATE_NOOP("NativeMenu", "Native processing (AnimeJaNai)"));
    on.action = Action::TurnOn;
    // Exactly one row wears the mark: "On" only when no specific recipe is asked for.
    on.current = in.preferences.enabled && in.preferences.recipeId.isEmpty();
    on.disabled = !applicable;
    on.tooltip = applicable
                     ? translated(QT_TRANSLATE_NOOP(
                           "NativeMenu",
                           "Run the requested native chain."))
                     : translated(QT_TRANSLATE_NOOP(
                           "NativeMenu",
                           "Request native processing. It cannot run here: %1 The picture "
                           "stays on the Original pipeline."))
                           .arg(refusal);
    rows.append(on);

    if (in.availability.backends.isEmpty()) {
        Row none;
        none.id = QStringLiteral("native/backend/none");
        none.label = translated(QT_TRANSLATE_NOOP("NativeMenu", "No backend available"));
        none.disabled = true;
        none.tooltip = capabilityReason(in, AvailabilityState());
        rows.append(none);
    } else {
        for (const Backend backend : in.availability.backends) {
            Row row;
            row.id = QStringLiteral("native/backend/") + stableName(backend);
            row.label = translated(QT_TRANSLATE_NOOP(
                            "NativeMenu",
                            "Backend: %1"))
                            .arg(backendLabel(backend));
            row.value = stableName(backend);
            row.action = Action::SetBackend;
            // Backend rows carry no current mark; only the engine can block one a
            // producer vouched for.
            row.disabled = !engineReady;
            row.tooltip = row.disabled
                              ? translated(QT_TRANSLATE_NOOP("NativeMenu", "Not available: %1"))
                                    .arg(capabilityReason(in, AvailabilityState()))
                              : translated(QT_TRANSLATE_NOOP(
                                    "NativeMenu",
                                    "Run native processing on %1."))
                                    .arg(backendLabel(backend));
            rows.append(row);
        }
    }

    if (in.availability.recipes.isEmpty()) {
        // Reported empty with the producer's reason, never filled with fabricated models.
        Row empty;
        empty.id = QStringLiteral("native/no-models");
        empty.label = translated(QT_TRANSLATE_NOOP("NativeMenu", "No native models available"));
        empty.disabled = true;
        empty.tooltip = in.availability.engineReason.isEmpty()
                            ? translated(QT_TRANSLATE_NOOP(
                                  "NativeMenu",
                                  "No model catalogue has been provisioned, and no engine "
                                  "exists in this build to run one."))
                            : in.availability.engineReason;
        rows.append(empty);
    } else {
        for (const NativeRecipeOption& option : in.availability.recipes) {
            Row row;
            row.id = QStringLiteral("native/recipe/") + option.id;
            row.label = option.label.isEmpty() ? option.id : option.label;
            row.value = option.id;
            row.action = Action::SetRecipe;
            row.current = in.preferences.enabled && in.preferences.recipeId == option.id;
            row.disabled = !engineReady || !option.availability.isUsable();
            row.tooltip = row.disabled
                              ? translated(QT_TRANSLATE_NOOP("NativeMenu", "Not available: %1"))
                                    .arg(capabilityReason(in, option.availability))
                              : translated(QT_TRANSLATE_NOOP(
                                    "NativeMenu",
                                    "Run the native recipe “%1”."))
                                    .arg(row.label);
            rows.append(row);
        }
    }

    // A stored request the catalogue does not offer keeps its current mark; dropping
    // it would say the choice is gone. It is not choosable and claims no activation.
    if (in.preferences.enabled && !in.preferences.recipeId.isEmpty() &&
        in.availability.findRecipe(in.preferences.recipeId) == nullptr) {
        Row unknown;
        unknown.id = QStringLiteral("native/recipe/") + in.preferences.recipeId;
        unknown.label = translated(QT_TRANSLATE_NOOP(
                           "NativeMenu",
                           "Requested recipe “%1” is not offered"))
                            .arg(in.preferences.recipeId);
        unknown.action = Action::None;
        unknown.current = true;
        unknown.disabled = true;
        unknown.tooltip = translated(QT_TRANSLATE_NOOP(
                                         "NativeMenu",
                                         "The saved recipe “%1” is the current request but "
                                         "cannot be applied: %2"))
                              .arg(in.preferences.recipeId, refusal);
        rows.append(unknown);
    }

    // Requested and effective are separate; a requested-but-not-applied choice stays visible.
    if (in.preferences.enabled) {
        Row effective;
        effective.id = QStringLiteral("native/effective");
        effective.disabled = true;
        // Effective is the filter's own word; eligibility says *could* run, not does.
        effective.label = running ? translated(QT_TRANSLATE_NOOP(
                                       "NativeMenu",
                                       "Effective: native"))
                                  : translated(QT_TRANSLATE_NOOP(
                                        "NativeMenu",
                                        "Effective: Original"));
        effective.tooltip =
            running
                ? translated(QT_TRANSLATE_NOOP(
                      "NativeMenu",
                      "The filter reports this request is running."))
                : translated(QT_TRANSLATE_NOOP(
                      "NativeMenu",
                      "The request is stored but cannot be applied: %1"))
                      .arg(refusal);
        rows.append(effective);
    }

    return rows;
}

QVector<Row> rifeRows(const Inputs& in) {
    Q_UNUSED(in);
    // The full/lite axis composed with the ensemble flag; sites are named by the
    // model name the backend asks for, never a row index.
    struct Variant {
        RifeVariant variant;
        bool ensemble;
    };
    static const Variant kVariants[] = {
        {RifeVariant::Full, false},
        {RifeVariant::Lite, false},
        {RifeVariant::Full, true},
        {RifeVariant::Lite, true},
    };
    QVector<Row> rows;
    for (const Variant& entry : kVariants) {
        const QString model = rifeModelName(RifeVersion::V4_14, entry.variant, entry.ensemble);
        Row row;
        row.id = QStringLiteral("rife/") + model;
        row.label = entry.ensemble
                        ? translated(QT_TRANSLATE_NOOP(
                              "NativeMenu",
                              "RIFE v4.14 · %1 · Ensemble"))
                              .arg(variantLabel(entry.variant))
                        : translated(QT_TRANSLATE_NOOP(
                              "NativeMenu",
                              "RIFE v4.14 · %1"))
                              .arg(variantLabel(entry.variant));
        row.disabled = true;
        row.tooltip = rifeLineageReason();
        rows.append(row);
    }
    return rows;
}

QVector<Row> levelRows(const QString& levelId, const Inputs& in) {
    if (levelId == QLatin1String(kNativeLevelId)) {
        return nativeRows(in);
    }
    if (levelId == QLatin1String(kRifeLevelId)) {
        return rifeRows(in);
    }
    return {};
}

StatusLines statusLines(const Inputs& in) {
    StatusLines lines;
    const std::optional<StatusSnapshot>& snapshot = in.status.snapshot;
    const bool filterReports = snapshot.has_value() && snapshot->state != NativeState::Off;
    lines.visible = in.preferences.enabled || filterReports;
    if (!lines.visible) {
        return lines;
    }

    lines.state = snapshot.has_value() ? uiStateLabel(snapshot->state)
                                       : translated(QT_TRANSLATE_NOOP("NativeMenu", "Unavailable"));

    const NativeEffectivePlan* effective =
        snapshot.has_value() && snapshot->effective.has_value() ? &*snapshot->effective : nullptr;
    const NativeCadenceCounters* counters =
        snapshot.has_value() && snapshot->counters.has_value() ? &*snapshot->counters : nullptr;

    if (effective != nullptr) {
        lines.provider = effective->provider;
        lines.model = effective->model;
        if (effective->sourceSize.isValid() && effective->outputSize.isValid()) {
            lines.geometry = QStringLiteral("%1x%2 → %3x%4")
                                 .arg(effective->sourceSize.w)
                                 .arg(effective->sourceSize.h)
                                 .arg(effective->outputSize.w)
                                 .arg(effective->outputSize.h);
        }
        // Rendered only when both rates are stated; an absent rate stays empty, never 1/1.
        if (effective->sourceFps.has_value() && effective->sourceFps->isValid() &&
            effective->outputFps.has_value() && effective->outputFps->isValid()) {
            lines.fps = QStringLiteral("%1/%2 → %3/%4")
                            .arg(effective->sourceFps->num)
                            .arg(effective->sourceFps->den)
                            .arg(effective->outputFps->num)
                            .arg(effective->outputFps->den);
        }
        if (effective->interpolation.has_value() &&
            effective->interpolation->factor.isValid()) {
            lines.interpolation = effective->interpolation->factor.toString();
        }
    }
    if (counters != nullptr) {
        lines.dropped = QString::number(counters->dropped);
    }
    // A member the snapshot omits stays empty: a missing geometry is unknown, not zero.

    lines.active = snapshotActive(in);

    // The reader's own answer, in order: the snapshot's reason, then the reason
    // the reader published for the view it could not read. A preference verdict
    // is never substituted for either - it describes what was requested, not
    // what the filter is doing, and a chip that swapped one for the other would
    // show a reason no core ever reported.
    QString reason;
    if (snapshot.has_value() && !snapshot->reason.isEmpty()) {
        reason = snapshot->reason;
    } else if (!in.status.availability.reason.isEmpty()) {
        reason = in.status.availability.reason;
    } else {
        reason = refusalReason(in, evaluateNativeSelection(in.preferences, in.availability));
    }
    lines.reason = lines.active ? QString() : reason;

    const QString interpolation =
        lines.interpolation.isEmpty()
            ? QString()
            : translated(QT_TRANSLATE_NOOP("NativeMenu", "interp %1")).arg(lines.interpolation);
    const QString dropped =
        lines.dropped.isEmpty()
            ? QString()
            : translated(QT_TRANSLATE_NOOP("NativeMenu", "dropped %1")).arg(lines.dropped);

    if (lines.active) {
        lines.headline = joinStatusParts(
            {translated(QT_TRANSLATE_NOOP("NativeMenu", "Native")), lines.provider, lines.model,
             lines.geometry, lines.fps, interpolation, dropped});
    } else if (snapshot.has_value()) {
        // The plan is shown next to the state and the reason even when it is not
        // running: a Suspended or Failed snapshot still reports the chain that
        // was applied, and dropping it here would answer "nothing is running"
        // with a blank where the model's identity belongs. State and reason
        // still lead, so the plan never reads as Active.
        lines.headline = joinStatusParts(
            {translated(QT_TRANSLATE_NOOP("NativeMenu", "Native %1")).arg(lines.state), lines.reason,
             lines.provider, lines.model, lines.geometry, lines.fps, interpolation, dropped});
    } else {
        lines.headline = translated(QT_TRANSLATE_NOOP(
                             "NativeMenu",
                             "Native requested · Original active — %1"))
                             .arg(lines.reason);
    }
    return lines;
}

}  // namespace menu
}  // namespace native
}  // namespace mplayer

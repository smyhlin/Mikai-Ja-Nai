#include "NativePrefs.hpp"

#include "native/bundle/NativeCapabilities.hpp"
#include "native/bundle/NativePaths.hpp"
#include "native/models/NativeProfiles.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/status/NativeStatus.hpp"
#include "native/models/NativeUserRecipes.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaType>
#include <QtCore/QSettings>
#include <QtCore/QVariant>

namespace mplayer {
namespace native {
namespace {

/// Every literal handed here MUST be wrapped in QT_TRANSLATE_NOOP, which lupdate
/// reads; at run time it is the literal itself.
QString translated(const char* message) {
    return QCoreApplication::translate("NativePrefs", message);
}

/// False when the value is not a yes/no at all, which differs from a stored
/// `false`: the caller falls back and reports.
bool readStoredBool(const QVariant& raw, bool* value) {
    if (raw.metaType().id() == QMetaType::Bool) {
        *value = raw.toBool();
        return true;
    }
    const QString text = raw.toString().trimmed().toLower();
    if (text == QLatin1String("true") || text == QLatin1String("1")) {
        *value = true;
        return true;
    }
    if (text == QLatin1String("false") || text == QLatin1String("0")) {
        *value = false;
        return true;
    }
    return false;
}

NativePreferenceIssue issue(const char* key, const char* reasonKey, const QString& reason) {
    NativePreferenceIssue result;
    result.key = QLatin1String(key);
    result.reasonKey = QLatin1String(reasonKey);
    result.reason = reason;
    return result;
}

/// The fact that a recipe's spatial stage cannot run on the queried factory. A
/// role is what the factory answers, so the reason names the role and not a stage.
AvailabilityState spatialRoleUnavailable() {
    return unavailableAvailability(
        QStringLiteral("native.recipe.spatialUnsupported"),
        translated(QT_TRANSLATE_NOOP(
            "NativePrefs",
            "The queried runtime factory answered no spatial role, so a spatial recipe "
            "cannot run on it.")));
}

/// As above for the original 11-channel RIFE temporal stage.
AvailabilityState rifeRoleUnavailable() {
    return unavailableAvailability(
        QStringLiteral("native.recipe.rifeUnsupported"),
        translated(QT_TRANSLATE_NOOP(
            "NativePrefs",
            "The queried runtime factory answered no RIFE role, so this recipe's "
            "interpolation stage cannot run on it.")));
}

/// The rows offered while the engine is not available: the curated leaves and every
/// stored recipe each carry the one named gap. The leaves come from NativeProfiles
/// and the rest from the store, so no row is invented here.
QVector<NativeRecipeOption> unavailableRecipeRows(const QString& reasonKey,
                                                  const QString& reason) {
    QVector<NativeRecipeOption> rows = builtinProfileOptions(reasonKey, reason);
    const AvailabilityState gap = unavailableAvailability(reasonKey, reason);
    QString storedReason;
    for (const Recipe& recipe : UserRecipeStore().recipes(&storedReason)) {
        NativeRecipeOption option;
        option.id = recipe.id;
        option.label = recipe.label.isEmpty() ? recipe.id : recipe.label;
        option.availability = gap;
        option.rifeModel =
            recipe.temporal.has_value() ? recipe.temporal->modelId() : QString();
        rows.append(option);
    }
    return rows;
}

/// What one stored recipe's own stages need from the queried factory: a spatial
/// stage needs the spatial role, a temporal stage the RIFE role. Every role
/// answered makes the row a selectable request; a role the observation did not
/// answer leaves it named unavailable, and no stage is dropped or substituted to
/// make it fit.
AvailabilityState storedRecipeState(const Recipe& recipe,
                                    const NativeFactoryObservation& factory) {
    if (!recipe.spatial.isEmpty() && !factory.spatial) {
        return spatialRoleUnavailable();
    }
    if (recipe.temporal.has_value() && !factory.rife) {
        return rifeRoleUnavailable();
    }
    return readyAvailability();
}

}  // namespace

QString nativeSettingsGroup() { return QStringLiteral("player/native"); }

bool NativePreferences::isDefault() const {
    return !enabled && backend == Backend::TensorRT && recipeId.isEmpty() &&
           customModelsDir.isEmpty();
}

NativePreferences loadNativePreferences() {
    NativePreferences preferences;

    QSettings settings;
    settings.beginGroup(nativeSettingsGroup());

    // An absent schema is the pre-versioning default; an unknown one is not guessed at.
    if (settings.contains(QLatin1String(kNativeSchemaKey))) {
        bool ok = false;
        const int schema = settings.value(QLatin1String(kNativeSchemaKey)).toInt(&ok);
        preferences.storedSchema = ok ? schema : 0;
        if (!ok || schema != kNativePreferencesSchema) {
            preferences.schemaRecognised = false;
            preferences.issues.append(issue(
                kNativeSchemaKey, "native.schemaUnknown",
                translated(QT_TRANSLATE_NOOP(
                    "NativePrefs",
                    "These native preferences were saved by another version of Mikai and "
                    "were left unchanged."))));
            settings.endGroup();
            for (const NativePreferenceIssue& entry : preferences.issues) {
                qWarning().noquote() << "mikai: native preference" << entry.key
                                     << "was not applied:" << entry.reason;
            }
            return preferences;
        }
    }

    if (settings.contains(QLatin1String(kNativeEnabledKey))) {
        bool value = false;
        if (readStoredBool(settings.value(QLatin1String(kNativeEnabledKey)), &value)) {
            preferences.enabled = value;
        } else {
            preferences.issues.append(issue(
                kNativeEnabledKey, "native.enabledInvalid",
                translated(QT_TRANSLATE_NOOP(
                    "NativePrefs",
                    "The saved native-processing switch was not a yes/no value; it was read "
                    "as off."))));
        }
    }

    if (settings.contains(QLatin1String(kNativeBackendKey))) {
        const QString stored = settings.value(QLatin1String(kNativeBackendKey)).toString().trimmed();
        const std::optional<Backend> parsed = backendFromName(stored);
        if (parsed.has_value()) {
            preferences.backend = *parsed;
        } else {
            preferences.issues.append(issue(
                kNativeBackendKey, "native.backendUnknown",
                translated(QT_TRANSLATE_NOOP(
                    "NativePrefs",
                    "The saved backend “%1” is not one this build knows; the default was "
                    "used."))
                    .arg(stored)));
        }
    }

    // Kept verbatim: whether the catalogue offers it is an availability question.
    if (settings.contains(QLatin1String(kNativeRecipeKey))) {
        preferences.recipeId =
            settings.value(QLatin1String(kNativeRecipeKey)).toString().trimmed();
    }

    // Read-only: never created or written; an unconvincing one is ignored and reported.
    if (settings.contains(QLatin1String(kNativeCustomModelsDirKey))) {
        const QString stored =
            settings.value(QLatin1String(kNativeCustomModelsDirKey)).toString().trimmed();
        if (!stored.isEmpty()) {
            const QString canonical = canonicalPath(stored);
            if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) {
                preferences.issues.append(issue(
                    kNativeCustomModelsDirKey, "native.modelsDirInvalid",
                    translated(QT_TRANSLATE_NOOP(
                        "NativePrefs",
                        "The saved custom models directory “%1” was not found; it was "
                        "ignored."))
                        .arg(stored)));
            } else {
                preferences.customModelsDir = canonical;
            }
        }
    }

    settings.endGroup();

    for (const NativePreferenceIssue& entry : preferences.issues) {
        qWarning().noquote() << "mikai: native preference" << entry.key
                             << "was not applied:" << entry.reason;
    }
    return preferences;
}

bool NativePreferencePatch::isEmpty() const {
    return !enabled.has_value() && !backend.has_value() && !recipeId.has_value() &&
           !customModelsDir.has_value();
}

bool saveNativePreferences(const NativePreferencePatch& patch, QString* reason) {
    // A patch naming no field writes nothing, not even the schema, so an untouched
    // native block survives a General save.
    if (patch.isEmpty()) {
        if (reason != nullptr) {
            reason->clear();
        }
        return true;
    }

    // Refused here and not only in the dialog, so another build's block stays intact.
    {
        QSettings probe;
        probe.beginGroup(nativeSettingsGroup());
        if (probe.contains(QLatin1String(kNativeSchemaKey))) {
            bool ok = false;
            const int schema = probe.value(QLatin1String(kNativeSchemaKey)).toInt(&ok);
            probe.endGroup();
            if (!ok || schema != kNativePreferencesSchema) {
                if (reason != nullptr) {
                    *reason = translated(QT_TRANSLATE_NOOP(
                        "NativePrefs",
                        "These native preferences belong to another version of Mikai and were "
                        "left unchanged."));
                }
                return false;
            }
        } else {
            probe.endGroup();
        }
    }

    QSettings settings;
    settings.beginGroup(nativeSettingsGroup());
    settings.setValue(QLatin1String(kNativeSchemaKey), kNativePreferencesSchema);
    if (patch.enabled.has_value()) {
        settings.setValue(QLatin1String(kNativeEnabledKey), *patch.enabled);
    }
    if (patch.backend.has_value()) {
        settings.setValue(QLatin1String(kNativeBackendKey), stableName(*patch.backend));
    }
    if (patch.recipeId.has_value()) {
        if (patch.recipeId->isEmpty()) {
            settings.remove(QLatin1String(kNativeRecipeKey));
        } else {
            settings.setValue(QLatin1String(kNativeRecipeKey), *patch.recipeId);
        }
    }
    if (patch.customModelsDir.has_value()) {
        if (patch.customModelsDir->isEmpty()) {
            settings.remove(QLatin1String(kNativeCustomModelsDirKey));
        } else {
            settings.setValue(QLatin1String(kNativeCustomModelsDirKey), *patch.customModelsDir);
        }
    }
    settings.endGroup();

    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (reason != nullptr) {
            *reason = translated(QT_TRANSLATE_NOOP(
                          "NativePrefs",
                          "The native preferences could not be saved."));
        }
        return false;
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return true;
}

/// True once the viewer chose "Skip and never ask" on the startup
/// missing-components prompt. Any truthy stored form counts: the flag is a
/// deliberate opt-out, never a value the code needs to parse back.
bool nativeComponentsPromptDismissed() {
    QSettings settings;
    settings.beginGroup(nativeSettingsGroup());
    const QVariant stored = settings.value(QLatin1String(kNativeComponentsPromptDismissedKey));
    settings.endGroup();
    return stored.toBool();
}

/// Records the "Skip and never ask" dismissal. The flag outlives a preferences
/// reset on purpose: clearing the Processing tab's request fields must not
/// silently re-arm a prompt the viewer explicitly muted.
void dismissNativeComponentsPrompt() {
    QSettings settings;
    settings.beginGroup(nativeSettingsGroup());
    settings.setValue(QLatin1String(kNativeComponentsPromptDismissedKey), true);
    settings.endGroup();
    settings.sync();
}

void clearNativePreferences() {
    QSettings settings;
    settings.beginGroup(nativeSettingsGroup());
    settings.remove(QLatin1String(kNativeSchemaKey));
    settings.remove(QLatin1String(kNativeEnabledKey));
    settings.remove(QLatin1String(kNativeBackendKey));
    settings.remove(QLatin1String(kNativeRecipeKey));
    settings.remove(QLatin1String(kNativeCustomModelsDirKey));
    settings.endGroup();
    settings.sync();
}

const NativeRecipeOption* NativeAvailability::findRecipe(const QString& id) const {
    for (const NativeRecipeOption& option : recipes) {
        if (option.id == id) {
            return &option;
        }
    }
    return nullptr;
}

NativeAvailability nativeAvailabilityFor(const RuntimeSelection& runtime,
                                         const NativeFactoryObservation* factory) {
    NativeAvailability available;

    // Named in precedence order. 1: the module this process actually pinned; a
    // missing or refused native runtime is the selection's own observed fact,
    // never a descriptor label.
    if (!runtime.loaded || runtime.kind != BundleKind::Native) {
        available.engineReasonKey = QStringLiteral("native.runtimeUnavailable");
        available.engineReason =
            runtime.reason.isEmpty()
                ? translated(QT_TRANSLATE_NOOP(
                      "NativePrefs",
                      "The native runtime is not the module this launch selected."))
                : runtime.reason;
        available.recipes =
            unavailableRecipeRows(available.engineReasonKey, available.engineReason);
        return available;
    }

    // 2: the observation for THIS source. Without one nothing has been executed,
    // so no backend or role is claimed and the request stays a request.
    if (factory == nullptr || !factory->observed) {
        available.engineReasonKey = QStringLiteral("native.capabilitiesUnconfirmed");
        available.engineReason = translated(QT_TRANSLATE_NOOP(
            "NativePrefs",
            "The native runtime is loaded, but no factory observation for this source "
            "has confirmed which roles it can run."));
        available.recipes =
            unavailableRecipeRows(available.engineReasonKey, available.engineReason);
        return available;
    }

    // 3: a query that ran and refused names its own fact - the protocol, the
    // device or the backend it was actually checked against - reported verbatim
    // rather than re-worded here.
    if (!factory->isUsable()) {
        available.engineReasonKey = factory->reasonKey.isEmpty()
                                        ? QStringLiteral("native.factoryUnavailable")
                                        : factory->reasonKey;
        available.engineReason =
            factory->reason.isEmpty()
                ? translated(QT_TRANSLATE_NOOP(
                      "NativePrefs",
                      "The queried runtime factory did not answer success for this "
                      "source."))
                : factory->reason;
        available.recipes =
            unavailableRecipeRows(available.engineReasonKey, available.engineReason);
        return available;
    }

    // 4: an executed success. The engine is available because the runtime factory
    // really ran against the adopted source and answered at least one role, and the
    // backend offered is the one it was checked against. This is eligibility, not
    // activation and not artifact readiness: only the filter's own status reports a
    // chain running.
    available.engineAvailable = true;
    const NativeCapabilities capabilities = nativeCapabilitiesFrom(*factory);
    if (capabilities.hasTensorRT) {
        available.backends.append(Backend::TensorRT);
    }
    if (capabilities.hasDirectML) {
        available.backends.append(Backend::DirectML);
    }

    // The curated leaves are the three spatial request templates, so each is
    // selectable exactly when the queried factory answered the spatial role. The
    // rows and their labels come from NativeProfiles; only their availability is
    // the fact observed here.
    const AvailabilityState leafState =
        factory->spatial ? readyAvailability() : spatialRoleUnavailable();
    available.recipes = builtinProfileOptions(leafState.reasonKey, leafState.reason);
    for (NativeRecipeOption& option : available.recipes) {
        option.availability = leafState;
    }

    // A stored recipe needs the roles its own stages name. The store caches its
    // documents, so a tick does not re-read them.
    QString storedReason;
    for (const Recipe& recipe : UserRecipeStore().recipes(&storedReason)) {
        NativeRecipeOption option;
        option.id = recipe.id;
        option.label = recipe.label.isEmpty() ? recipe.id : recipe.label;
        option.availability = storedRecipeState(recipe, *factory);
        option.rifeModel =
            recipe.temporal.has_value() ? recipe.temporal->modelId() : QString();
        available.recipes.append(option);
    }
    return available;
}

NativeAvailability currentNativeAvailability(const NativeFactoryObservation* factory) {
    return nativeAvailabilityFor(currentSelection(), factory);
}

NativeSelectionView evaluateNativeSelection(const NativePreferences& preferences,
                                            const NativeAvailability& available) {
    NativeSelectionView view;
    view.requestedEnabled = preferences.enabled;
    view.backend = preferences.backend;
    view.recipeId = preferences.recipeId;

    if (!preferences.enabled) {
        view.availability = unavailableAvailability(QStringLiteral("native.disabled"),
                                                    translated(QT_TRANSLATE_NOOP(
                                                        "NativePrefs",
                                                        "Native processing is off.")));
        return view;
    }

    if (!available.engineAvailable) {
        view.availability = unavailableAvailability(
            available.engineReasonKey.isEmpty() ? QStringLiteral("native.engineUnavailable")
                                                : available.engineReasonKey,
            available.engineReason.isEmpty()
                ? translated(QT_TRANSLATE_NOOP(
                      "NativePrefs",
                      "The native processing engine is not available."))
                : available.engineReason);
        return view;
    }

    if (preferences.recipeId.isEmpty()) {
        view.availability = unavailableAvailability(QStringLiteral("native.recipeMissing"),
                                                    translated(QT_TRANSLATE_NOOP(
                                                        "NativePrefs",
                                                        "No native recipe is selected.")));
        return view;
    }

    const NativeRecipeOption* recipe = available.findRecipe(preferences.recipeId);
    if (recipe == nullptr) {
        view.availability = unavailableAvailability(
            QStringLiteral("native.recipeUnknown"),
            translated(QT_TRANSLATE_NOOP(
                "NativePrefs",
                "The saved recipe “%1” is not in the current catalogue."))
                .arg(preferences.recipeId));
        return view;
    }
    if (!recipe->availability.isUsable()) {
        view.availability = recipe->availability.hasMandatoryReason()
                                ? recipe->availability
                                : unavailableAvailability(
                                      QStringLiteral("native.recipeUnavailable"),
                                      translated(QT_TRANSLATE_NOOP(
                                          "NativePrefs",
                                          "The recipe “%1” is not available."))
                                          .arg(recipe->id));
        return view;
    }

    if (!available.backends.contains(preferences.backend)) {
        view.availability = unavailableAvailability(
            QStringLiteral("native.backendUnsupported"),
            translated(QT_TRANSLATE_NOOP(
                "NativePrefs",
                "The backend %1 is not offered by this runtime."))
                .arg(stableName(preferences.backend)));
        return view;
    }

    // Eligible, not active: only the filter's own status snapshot can report it running.
    view.requestAvailable = true;
    view.availability = readyAvailability();
    return view;
}

}  // namespace native
}  // namespace mplayer

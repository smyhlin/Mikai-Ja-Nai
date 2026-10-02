#include "NativePrefs.hpp"

#include "NativePaths.hpp"
#include "NativeProfiles.hpp"
#include "NativeRuntimeSelection.hpp"
#include "NativeUserRecipes.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaType>
#include <QtCore/QSettings>
#include <QtCore/QVariant>
#include <QtGui/QKeySequence>

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

}  // namespace

QString nativeSettingsGroup() { return QStringLiteral("player/native"); }

QString nativeShortcutsSettingsGroup() { return QStringLiteral("player/native/shortcuts"); }

bool NativePreferences::isDefault() const {
    return !enabled && backend == Backend::TensorRT && recipeId.isEmpty() &&
           customModelsDir.isEmpty() && toggleProcessingShortcut.isEmpty();
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

    settings.beginGroup(nativeShortcutsSettingsGroup());
    // Presence, not value: a cleared binding was still set by the user and must not
    // re-offer a default.
    preferences.toggleProcessingShortcutStored =
        settings.contains(QLatin1String(kNativeToggleProcessingKey));
    if (settings.contains(QLatin1String(kNativeToggleProcessingKey))) {
        const QString stored =
            settings.value(QLatin1String(kNativeToggleProcessingKey)).toString().trimmed();
        if (!stored.isEmpty()) {
            const QKeySequence sequence =
                QKeySequence::fromString(stored, QKeySequence::PortableText);
            const QString normalized = sequence.toString(QKeySequence::PortableText);
            if (normalized.isEmpty()) {
                preferences.issues.append(issue(
                    kNativeToggleProcessingKey, "native.shortcutInvalid",
                    translated(QT_TRANSLATE_NOOP(
                        "NativePrefs",
                        "The saved native shortcut did not name a key; it was cleared."))));
            } else {
                preferences.toggleProcessingShortcut = normalized;
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
           !customModelsDir.has_value() && !toggleProcessingShortcut.has_value();
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

    if (patch.toggleProcessingShortcut.has_value()) {
        settings.beginGroup(nativeShortcutsSettingsGroup());
        if (patch.toggleProcessingShortcut->isEmpty()) {
            // Stored present-and-empty, not removed: "cleared on purpose" must survive
            // or the default comes back.
            settings.setValue(QLatin1String(kNativeToggleProcessingKey), QString());
        } else {
            settings.setValue(QLatin1String(kNativeToggleProcessingKey),
                              *patch.toggleProcessingShortcut);
        }
        settings.endGroup();
    }

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

void clearNativePreferences() {
    QSettings settings;
    settings.beginGroup(nativeSettingsGroup());
    settings.remove(QLatin1String(kNativeSchemaKey));
    settings.remove(QLatin1String(kNativeEnabledKey));
    settings.remove(QLatin1String(kNativeBackendKey));
    settings.remove(QLatin1String(kNativeRecipeKey));
    settings.remove(QLatin1String(kNativeCustomModelsDirKey));
    settings.endGroup();
    settings.beginGroup(nativeShortcutsSettingsGroup());
    settings.remove(QLatin1String(kNativeToggleProcessingKey));
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

NativeAvailability nativeAvailabilityFor(const RuntimeSelection& runtime) {
    NativeAvailability available;
    // No engine is ever claimed in this build; the reason is the runtime selection's
    // own observed condition, never a descriptor label.
    available.engineAvailable = false;

    if (!runtime.loaded || runtime.kind != BundleKind::Native) {
        available.engineReasonKey = QStringLiteral("native.runtimeUnavailable");
        available.engineReason =
            runtime.reason.isEmpty()
                ? translated(QT_TRANSLATE_NOOP(
                      "NativePrefs",
                      "The native runtime is not the module this launch selected."))
                : runtime.reason;
    } else {
        available.engineReasonKey = QStringLiteral("native.capabilitiesUnconfirmed");
        available.engineReason = translated(QT_TRANSLATE_NOOP(
            "NativePrefs",
            "The native runtime is loaded, but nothing has confirmed which capabilities "
            "it can run."));
    }

    // Left empty deliberately: no producer vouches for a backend, and a vocabulary is
    // not a capability.
    available.backends.clear();

    // Curated profile leaves come first so a requested leaf is found whatever order
    // the stored recipes follow; each carries the runtime gap, not a fake engine.
    available.recipes = builtinProfileOptions(available.engineReasonKey, available.engineReason);

    // Stored recipes are offered as requested choices, not Ready: each carries the
    // runtime gap. The store caches its documents, so a tick does not re-read them.
    const UserRecipeStore storedRecipes;
    QString recipesReason;
    for (const Recipe& recipe : storedRecipes.recipes(&recipesReason)) {
        NativeRecipeOption option;
        option.id = recipe.id;
        option.label = recipe.label.isEmpty() ? recipe.id : recipe.label;
        option.availability =
            unavailableAvailability(available.engineReasonKey, available.engineReason);
        available.recipes.append(option);
    }
    return available;
}

NativeAvailability currentNativeAvailability() {
    return nativeAvailabilityFor(currentSelection());
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

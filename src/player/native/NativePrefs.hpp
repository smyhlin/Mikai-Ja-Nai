// Additive and versioned: a save touches only this module's keys, loading is
// read-only, and another version's block reads as the off default and is left
// byte-for-byte alone. A requested choice this build cannot offer is kept and named
// unavailable, never reset to a fake Ready; only the filter's snapshot activates.

#pragma once

#include <optional>

#include <QtCore/QString>
#include <QtCore/QVector>

#include "NativeCatalog.hpp"
#include "NativeTypes.hpp"

namespace mplayer {
namespace native {

/// Declared, not included: this header must not pull the loader's headers into
/// every preference caller.
struct RuntimeSelection;

/// This module's group; it also holds the runtime choice, owned by
/// NativeRuntimeSelection and preserved.
QString nativeSettingsGroup();

/// Separate from the preset bindings so the two spaces never share a settings key.
QString nativeShortcutsSettingsGroup();

/// The only schema this build reads.
inline constexpr int kNativePreferencesSchema = 1;

/// Stable preference keys. No caller spells one out.
inline constexpr char kNativeSchemaKey[] = "schema";
inline constexpr char kNativeEnabledKey[] = "enabled";
inline constexpr char kNativeBackendKey[] = "backend";
inline constexpr char kNativeRecipeKey[] = "recipe";
inline constexpr char kNativeCustomModelsDirKey[] = "customModelsDir";
inline constexpr char kNativeToggleProcessingKey[] = "toggleProcessing";

/// One key that did not load as stored, and the fact that explains it.
struct NativePreferenceIssue {
    QString key;
    QString reasonKey;
    QString reason;
};

/// The requested native preference, validated per key.
struct NativePreferences {
    /// Persisted even while the choice is unavailable.
    bool enabled = false;

    /// A stored name this build does not know is refused by name; a
    /// known-but-unoffered backend is kept as the request.
    Backend backend = Backend::TensorRT;

    /// Kept verbatim: one this build cannot offer is requested-but-unavailable,
    /// never a reset to Original.
    QString recipeId;

    /// The validated canonical directory, or empty; read-only to the UI.
    QString customModelsDir;

    /// The PortableText binding, or empty when unbound; installability is the
    /// shared collision check's decision, not this module's.
    QString toggleProcessingShortcut;

    /// "Never written" and "written as nothing" are different facts: only the
    /// first may fall back to a documented default binding.
    bool toggleProcessingShortcutStored = false;

    /// False when the stored block is a schema this build does not read; the
    /// requested fields carry defaults while the stored document is left untouched.
    bool schemaRecognised = true;
    int storedSchema = 0;

    /// Per-key fallback facts; empty when every key loaded.
    QVector<NativePreferenceIssue> issues;

    /// What a missing or unknown-schema block reads as.
    bool isDefault() const;
};

/// Additive and read-only: no key is written, removed or created, so opening
/// settings cannot mutate the store.
NativePreferences loadNativePreferences();

/// `nullopt` leaves the stored key exactly as it is, which is how a read-only or
/// untouched key survives a save.
struct NativePreferencePatch {
    std::optional<bool> enabled;
    std::optional<Backend> backend;
    /// An empty string removes the key.
    std::optional<QString> recipeId;
    std::optional<QString> customModelsDir;
    /// The one empty value that does *not* remove its key: a cleared binding is
    /// stored as an explicit "unbound" so the default is not re-offered.
    std::optional<QString> toggleProcessingShortcut;

    /// True when the patch names no key at all; saving it writes nothing - not
    /// even the schema - which keeps a General save out of native storage.
    bool isEmpty() const;
};

/// Writes only the keys the patch names plus the schema; an empty patch writes nothing.
/// Returns false with `reason` when the store is not writable or the schema is protected.
bool saveNativePreferences(const NativePreferencePatch& patch, QString* reason = nullptr);

/// Removes every key this module owns, leaving the rest of the group - the runtime
/// choice included - alone.
void clearNativePreferences();

/// One recipe the catalogue producer vouches for, with its own availability.
struct NativeRecipeOption {
    QString id;
    QString label;
    AvailabilityState availability;
};

/// An empty snapshot is not an error: nothing is available yet, and the reason
/// says so.
struct NativeAvailability {
    /// True only when a real engine exists to run a prepared chain.
    bool engineAvailable = false;
    QString engineReasonKey;
    QString engineReason;

    /// As a producer vouched for them; a vocabulary is not a claim, so this stays
    /// empty until one does.
    QVector<Backend> backends;

    QVector<NativeRecipeOption> recipes;

    const NativeRecipeOption* findRecipe(const QString& id) const;
};

/// Named rather than guessed: a non-native selection names the runtime gap, a loaded
/// native runtime its unconfirmed capabilities. No capability is read off a manifest label.
NativeAvailability nativeAvailabilityFor(const RuntimeSelection& runtime);

/// Observed from the runtime selection this process actually made; nothing is faked
/// Ready.
NativeAvailability currentNativeAvailability();

/// Requested versus effective, kept apart.
struct NativeSelectionView {
    bool requestedEnabled = false;
    /// True when the whole request is *eligible*; eligibility is not activation, only
    /// the filter's own status snapshot may report the chain running.
    bool requestAvailable = false;
    Backend backend = Backend::TensorRT;
    QString recipeId;
    /// Not Ready, with the fact, for any reason the request cannot be applied.
    AvailabilityState availability;
};

/// Never substitutes another recipe or backend for what was asked for.
NativeSelectionView evaluateNativeSelection(const NativePreferences& preferences,
                                            const NativeAvailability& available);

}  // namespace native
}  // namespace mplayer

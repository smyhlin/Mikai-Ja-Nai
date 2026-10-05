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

/// Declared, not included, for the same reason: an availability caller passes a
/// pointer to the observation its own core published, and `nullptr` is a whole
/// caller (cold, offline, no status read yet). The definition is NativeStatus.hpp.
struct NativeFactoryObservation;

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

/// The stored toggle binding as the player's Qt GUI reader reported it. Reading
/// and canonicalising a PortableText key chord needs QKeySequence, which lives
/// in QtGui; this QtCore-only contract declares the shape, and the reader
/// (src/player/NativeToggleShortcut.cpp) performs the read and the parse.
struct NativeToggleShortcut {
    /// True when the key is present at all, even as an explicit empty binding.
    bool stored = false;
    /// The canonical PortableText binding; empty when unbound or invalid.
    QString value;
    /// False only when a stored, non-empty text named no key at all; the fact
    /// is then named by the caller, never silently dropped.
    bool valid = true;
};

/// Reads the toggle binding from the shortcuts group through the player's GUI
/// reader. Never writes.
NativeToggleShortcut loadStoredToggleShortcut();

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
    /// The exact temporal curated model identity derived from the recipe's
    /// temporal stage via `rifeModelName` (e.g. "rife_v4.14_lite"); empty when
    /// the recipe carries no temporal stage. Never parsed from a label, a file
    /// name or an ordinal. Populated in every real producer path for stored
    /// recipes so the RIFE panel can route without guessing.
    QString rifeModel;
};

/// An empty snapshot is not an error: nothing is available yet, and the reason
/// says so.
struct NativeAvailability {
    /// True only when a factory observation for the live source executed a query
    /// that answered success and so proved the runtime can consume at least one
    /// role. It is eligibility to run a prepared chain on this runtime - NOT an
    /// attestation that any particular engine is built, cached, Prepared or
    /// Active, or that a model is present; those separate facts belong to the
    /// preparation and status paths. A preference, a manifest label, a file name,
    /// a stored recipe or a previous source never sets it, and nothing here is
    /// latched across calls.
    bool engineAvailable = false;
    /// The named fact that blocks the engine; empty exactly when `engineAvailable`.
    QString engineReasonKey;
    QString engineReason;

    /// The backends the queried factory was actually checked against. A
    /// vocabulary is not a claim, so this stays empty until one vouches for it.
    QVector<Backend> backends;

    /// Every recipe row this build offers, each with its own availability. A row
    /// is a REQUEST: available means selectable, never that its artifacts exist
    /// or that a chain is running - only the filter's own status says that.
    QVector<NativeRecipeOption> recipes;

    const NativeRecipeOption* findRecipe(const QString& id) const;
};

/// Named rather than guessed, in this order: the module this process actually
/// pinned (absent or refused names the runtime gap), then the observation for the
/// live source (absent or unexecuted names the unconfirmed gap), then the
/// observation's own refusal (protocol, device, backend) carried verbatim, and
/// only an executed success makes the engine available. No capability is read off
/// a manifest label.
///
/// `factory` is the parsed observation from the SAME source's status snapshot, or
/// `nullptr` for a caller with none; it is required so no global factory state
/// can stand in for a caller's own.
NativeAvailability nativeAvailabilityFor(const RuntimeSelection& runtime,
                                         const NativeFactoryObservation* factory);

/// The above against the runtime selection this process actually made. The caller
/// passes the observation its own core published; nothing is cached here.
NativeAvailability currentNativeAvailability(const NativeFactoryObservation* factory);

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

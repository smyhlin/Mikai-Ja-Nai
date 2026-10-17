#include "TestNativePrefs.hpp"

#include "native/bundle/NativePaths.hpp"
#include "native/bundle/NativePrefs.hpp"
#include "native/models/NativeProfiles.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "native/status/NativeStatus.hpp"
#include "player/shaders/PresetShortcuts.hpp"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>

using namespace mplayer::native;

namespace {

/// Keys outside the native preference fields. Native saves and each isolated
/// test leave them untouched except when a test deliberately seeds one.
constexpr const char kRetiredToggleBindingKey[] =
    "player/native/shortcuts/toggleProcessing";
constexpr const char* kForeignKeys[] = {"player/presetShortcuts/hd2x", "ui/language",
                                        "quality/cache", "feeds",
                                        kRetiredToggleBindingKey};

void clearForeignKeys() {
  QSettings settings;
  for (const char* key : kForeignKeys) {
    settings.remove(QString::fromLatin1(key));
  }
  settings.sync();
}

mplayer::shell::ShortcutClaim claim(const QString& id, const QString& label,
                                    mplayer::shell::ShortcutOwnerKind kind) {
  mplayer::shell::ShortcutClaim result;
  result.ownerId = id;
  result.ownerLabel = label;
  result.kind = kind;
  return result;
}

/// A parsed observation as the status reader hands one on: an executed query that
/// answered success for this exact adopted source/device, naming the backend and
/// the roles it really checked. This is the input availability must react to; the
/// test never fabricates production state out of it.
NativeFactoryObservation observedFactory(const QString& backend, bool spatial, bool rife) {
  NativeFactoryObservation factory;
  factory.observed = true;
  factory.backend = backend;
  factory.spatial = spatial;
  factory.rife = rife;
  factory.runtimeVersion = QStringLiteral("TensorRT 11.3.0.99");
  factory.deviceIdentity = QStringLiteral("f76e436fc5198f5a95f7ce9481b54b5b");
  factory.smMajor = 8;
  factory.smMinor = 9;
  return factory;
}

/// An observation that ran and refused, carrying the named fact it got back.
NativeFactoryObservation refusedFactory(const QString& reasonKey, const QString& reason) {
  NativeFactoryObservation factory;
  factory.observed = true;
  factory.reasonKey = reasonKey;
  factory.reason = reason;
  return factory;
}

/// The runtime selection a process that really pinned the native module reports.
RuntimeSelection loadedNativeRuntime() {
  RuntimeSelection native;
  native.loaded = true;
  native.kind = BundleKind::Native;
  native.requested = RuntimeChoice::Native;
  native.version = QStringLiteral("9.9.9");
  return native;
}

}  // namespace

void TestNativePrefs::init() {
  clearNativePreferences();
  clearForeignKeys();
  QSettings settings;
  settings.remove(QStringLiteral("player/native/runtimeChoice"));
  settings.sync();
}

void TestNativePrefs::cleanup() { init(); }

void TestNativePrefs::aNativeRequestRoundTripsThroughItsGroup() {
  NativePreferencePatch patch;
  patch.enabled = true;
  patch.backend = Backend::DirectML;
  patch.recipeId = QStringLiteral("janai.spatial.hd2b.v1");
  QString reason;
  QVERIFY2(saveNativePreferences(patch, &reason), qPrintable(reason));

  const NativePreferences loaded = loadNativePreferences();
  QVERIFY(loaded.schemaRecognised);
  QCOMPARE(loaded.storedSchema, kNativePreferencesSchema);
  QVERIFY(loaded.enabled);
  QVERIFY(loaded.backend == Backend::DirectML);
  QCOMPARE(loaded.recipeId, QStringLiteral("janai.spatial.hd2b.v1"));
  QVERIFY(loaded.issues.isEmpty());
  QVERIFY(!loaded.isDefault());
}

void TestNativePrefs::theSavedBlockCarriesTheSchemaThisBuildWrites() {
  NativePreferencePatch patch;
  patch.enabled = false;
  QString reason;
  QVERIFY2(saveNativePreferences(patch, &reason), qPrintable(reason));

  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  QCOMPARE(settings.value(QLatin1String(kNativeSchemaKey)).toInt(), kNativePreferencesSchema);
  settings.endGroup();
}

void TestNativePrefs::loadingAnEmptyGroupWritesNothingAndReadsDefaults() {
  const NativePreferences loaded = loadNativePreferences();
  QVERIFY(loaded.isDefault());
  QVERIFY(loaded.schemaRecognised);
  QCOMPARE(loaded.storedSchema, 0);
  QVERIFY(loaded.issues.isEmpty());

  // A read is not a write: no key of ours exists after loading an empty group.
  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  QVERIFY(!settings.contains(QLatin1String(kNativeSchemaKey)));
  QVERIFY(!settings.contains(QLatin1String(kNativeEnabledKey)));
  QVERIFY(!settings.contains(QLatin1String(kNativeBackendKey)));
  QVERIFY(!settings.contains(QLatin1String(kNativeRecipeKey)));
  QVERIFY(!settings.contains(QLatin1String(kNativeCustomModelsDirKey)));
  settings.endGroup();
}

void TestNativePrefs::eachCorruptKeyFallsBackAloneAndReportsTheReason() {
  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  settings.setValue(QLatin1String(kNativeEnabledKey), QStringLiteral("maybe"));
  settings.setValue(QLatin1String(kNativeBackendKey), QStringLiteral("tensorrt-ultra"));
  // A recipe id is not parsed against a catalogue here, so an unknown one is
  // kept, not treated as corruption.
  settings.setValue(QLatin1String(kNativeRecipeKey), QStringLiteral("janai.unknown.v9"));
  settings.setValue(QLatin1String(kNativeCustomModelsDirKey),
                    QStringLiteral("Z:/mikai/definitely-not-here"));
  settings.endGroup();
  settings.sync();

  const NativePreferences loaded = loadNativePreferences();
  QVERIFY(!loaded.enabled);
  QVERIFY(loaded.backend == Backend::TensorRT);
  QCOMPARE(loaded.recipeId, QStringLiteral("janai.unknown.v9"));
  QVERIFY(loaded.customModelsDir.isEmpty());

  QSet<QString> reasonKeys;
  for (const NativePreferenceIssue& issue : loaded.issues) {
    reasonKeys.insert(issue.reasonKey);
    QVERIFY(!issue.reason.isEmpty());
    QVERIFY(!issue.key.isEmpty());
  }
  QVERIFY(reasonKeys.contains(QStringLiteral("native.enabledInvalid")));
  QVERIFY(reasonKeys.contains(QStringLiteral("native.backendUnknown")));
  QVERIFY(reasonKeys.contains(QStringLiteral("native.modelsDirInvalid")));
}

void TestNativePrefs::anUnknownSchemaReadsAsOffAndLeavesTheStoredBlockAlone() {
  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  settings.setValue(QLatin1String(kNativeSchemaKey), 2);
  settings.setValue(QLatin1String(kNativeEnabledKey), true);
  settings.setValue(QLatin1String(kNativeRecipeKey), QStringLiteral("future.recipe"));
  settings.endGroup();
  settings.sync();

  const NativePreferences loaded = loadNativePreferences();
  QVERIFY(!loaded.schemaRecognised);
  QCOMPARE(loaded.storedSchema, 2);
  QVERIFY(!loaded.enabled);
  QVERIFY(loaded.recipeId.isEmpty());
  QCOMPARE(static_cast<int>(loaded.issues.size()), 1);
  QCOMPARE(loaded.issues.first().reasonKey, QStringLiteral("native.schemaUnknown"));

  // The newer document survives being looked at.
  QSettings check;
  check.beginGroup(nativeSettingsGroup());
  QCOMPARE(check.value(QLatin1String(kNativeSchemaKey)).toInt(), 2);
  QCOMPARE(check.value(QLatin1String(kNativeEnabledKey)).toBool(), true);
  QCOMPARE(check.value(QLatin1String(kNativeRecipeKey)).toString(),
           QStringLiteral("future.recipe"));
  check.endGroup();
}

void TestNativePrefs::aSaveRefusesToOverwriteAnUnknownSchema() {
  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  settings.setValue(QLatin1String(kNativeSchemaKey), 2);
  settings.setValue(QLatin1String(kNativeEnabledKey), true);
  settings.endGroup();
  settings.sync();

  NativePreferencePatch patch;
  patch.enabled = false;
  QString reason;
  QVERIFY(!saveNativePreferences(patch, &reason));
  QVERIFY(!reason.isEmpty());

  QSettings check;
  check.beginGroup(nativeSettingsGroup());
  QCOMPARE(check.value(QLatin1String(kNativeSchemaKey)).toInt(), 2);
  QCOMPARE(check.value(QLatin1String(kNativeEnabledKey)).toBool(), true);
  check.endGroup();
}

void TestNativePrefs::anEmptyPatchSaveWritesNothing() {
  NativePreferencePatch patch;
  QVERIFY(patch.isEmpty());

  QString reason = QStringLiteral("stale");
  QVERIFY(saveNativePreferences(patch, &reason));
  QVERIFY(reason.isEmpty());

  // Not even the schema is written: a General save names no native field and
  // must leave native storage alone.
  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  QVERIFY(!settings.contains(QLatin1String(kNativeSchemaKey)));
  QVERIFY(!settings.contains(QLatin1String(kNativeEnabledKey)));
  QVERIFY(!settings.contains(QLatin1String(kNativeBackendKey)));
  QVERIFY(!settings.contains(QLatin1String(kNativeRecipeKey)));
  settings.endGroup();
}

void TestNativePrefs::anUntouchedUnknownSchemaSurvivesAGeneralSave() {
  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  settings.setValue(QLatin1String(kNativeSchemaKey), 2);
  settings.setValue(QLatin1String(kNativeEnabledKey), true);
  settings.setValue(QLatin1String(kNativeBackendKey), QStringLiteral("future-backend"));
  settings.setValue(QLatin1String(kNativeRecipeKey), QStringLiteral("future.recipe"));
  settings.endGroup();
  settings.sync();

  // What a General save does when only the language, the token, the feeds or the
  // presets changed: no native field is named.
  const NativePreferencePatch untouched;
  QString reason;
  QVERIFY2(saveNativePreferences(untouched, &reason), qPrintable(reason));
  QVERIFY(reason.isEmpty());

  QSettings check;
  check.beginGroup(nativeSettingsGroup());
  QCOMPARE(check.value(QLatin1String(kNativeSchemaKey)).toInt(), 2);
  QCOMPARE(check.value(QLatin1String(kNativeEnabledKey)).toBool(), true);
  QCOMPARE(check.value(QLatin1String(kNativeBackendKey)).toString(),
           QStringLiteral("future-backend"));
  QCOMPARE(check.value(QLatin1String(kNativeRecipeKey)).toString(),
           QStringLiteral("future.recipe"));
  check.endGroup();

  // And the untouched block still reads exactly as it did: off, unrecognised,
  // with the fact reported rather than applied.
  const NativePreferences loaded = loadNativePreferences();
  QVERIFY(!loaded.schemaRecognised);
  QCOMPARE(loaded.storedSchema, 2);
  QVERIFY(!loaded.enabled);
  QCOMPARE(static_cast<int>(loaded.issues.size()), 1);
  QCOMPARE(loaded.issues.first().reasonKey, QStringLiteral("native.schemaUnknown"));
}

void TestNativePrefs::anUnknownRecipeIsKeptAndReportedUnavailable() {
  NativeAvailability available;
  available.engineAvailable = true;
  available.backends = allBackends();

  NativePreferences request;
  request.enabled = true;
  request.recipeId = QStringLiteral("janai.unknown.v9");

  const NativeSelectionView view = evaluateNativeSelection(request, available);
  QVERIFY(!view.requestAvailable);
  QCOMPARE(view.recipeId, QStringLiteral("janai.unknown.v9"));
  QVERIFY(view.availability.value == Availability::Unavailable);
  QCOMPARE(view.availability.reasonKey, QStringLiteral("native.recipeUnknown"));
  QVERIFY(view.availability.reason.contains(QStringLiteral("janai.unknown.v9")));
}

void TestNativePrefs::anOfferedRecipeOnAnUnavailableBackendStaysRequested() {
  NativeAvailability available;
  available.engineAvailable = true;
  available.backends = {Backend::TensorRT};
  available.recipes.append({QStringLiteral("janai.spatial.hd2x.v1"), QStringLiteral("HD 2x"),
                            readyAvailability(), QString()});

  NativePreferences request;
  request.enabled = true;
  request.backend = Backend::DirectML;
  request.recipeId = QStringLiteral("janai.spatial.hd2x.v1");

  const NativeSelectionView view = evaluateNativeSelection(request, available);
  QVERIFY(!view.requestAvailable);
  QVERIFY(view.backend == Backend::DirectML);
  QVERIFY(view.availability.value == Availability::Unavailable);
  QCOMPARE(view.availability.reasonKey, QStringLiteral("native.backendUnsupported"));
}

void TestNativePrefs::anAvailableRecipeOnItsBackendIsRequestedButNeverActive() {
  NativeAvailability available;
  available.engineAvailable = true;
  available.backends = allBackends();
  available.recipes.append({QStringLiteral("janai.spatial.hd2x.v1"), QStringLiteral("HD 2x"),
                            readyAvailability(), QString()});

  NativePreferences request;
  request.enabled = true;
  request.backend = Backend::TensorRT;
  request.recipeId = QStringLiteral("janai.spatial.hd2x.v1");

  const NativeSelectionView view = evaluateNativeSelection(request, available);
  QVERIFY(view.requestAvailable);
  QVERIFY(view.availability.value == Availability::Ready);
  QVERIFY(view.availability.isUsable());
}

void TestNativePrefs::aRequestedTickPersistsWhileUnavailableAndKeepsTheRuntimeChoice() {
  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  settings.setValue(QStringLiteral("runtimeChoice"), QStringLiteral("native"));
  settings.endGroup();
  settings.sync();

  // A tick for a recipe this build cannot offer is the *request*: it is written,
  // never refused or reset for being unavailable.
  NativePreferencePatch patch;
  patch.enabled = true;
  patch.backend = Backend::DirectML;
  patch.recipeId = QStringLiteral("janai.spatial.hd2b.v1");
  QString reason;
  QVERIFY2(saveNativePreferences(patch, &reason), qPrintable(reason));

  const NativePreferences loaded = loadNativePreferences();
  QVERIFY(loaded.enabled);
  QVERIFY(loaded.backend == Backend::DirectML);
  QCOMPARE(loaded.recipeId, QStringLiteral("janai.spatial.hd2b.v1"));

  // A cold test process has no observation for any source, so the tick stays
  // requested: nothing is invented to make it look applied.
  const NativeAvailability available = currentNativeAvailability(nullptr);
  QVERIFY(!available.engineAvailable);
  QVERIFY(!available.engineReasonKey.isEmpty());
  QVERIFY(!available.engineReason.isEmpty());
  const NativeSelectionView view = evaluateNativeSelection(loaded, available);
  QVERIFY(view.requestedEnabled);
  QVERIFY(!view.requestAvailable);
  QVERIFY(view.availability.value == Availability::Unavailable);
  QVERIFY(!view.availability.reason.isEmpty());

  // The runtime choice shares the group and is not this module's to touch.
  QSettings check;
  check.beginGroup(nativeSettingsGroup());
  QCOMPARE(check.value(QStringLiteral("runtimeChoice")).toString(), QStringLiteral("native"));
  check.endGroup();
}

void TestNativePrefs::anOldToggleBindingDoesNotInvalidatePreferences() {
  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  settings.setValue(QLatin1String(kNativeSchemaKey), kNativePreferencesSchema);
  settings.setValue(QLatin1String(kNativeEnabledKey), true);
  settings.setValue(QLatin1String(kNativeBackendKey), QStringLiteral("directml"));
  settings.setValue(QLatin1String(kNativeRecipeKey),
                    QStringLiteral("janai.spatial.hd2x.v1"));
  settings.endGroup();
  settings.setValue(QString::fromLatin1(kRetiredToggleBindingKey),
                    QStringLiteral("Ctrl+Shift+J, Ctrl+Shift+K"));
  settings.sync();

  const NativePreferences loaded = loadNativePreferences();
  QVERIFY(loaded.enabled);
  QCOMPARE(loaded.backend, Backend::DirectML);
  QCOMPARE(loaded.recipeId, QStringLiteral("janai.spatial.hd2x.v1"));
  QVERIFY(loaded.issues.isEmpty());
}

void TestNativePrefs::aNativeSaveLeavesForeignKeysAlone() {
  QSettings settings;
  settings.setValue(QStringLiteral("player/presetShortcuts/hd2x"), QStringLiteral("Ctrl+1"));
  settings.setValue(QStringLiteral("ui/language"), QStringLiteral("uk"));
  settings.setValue(QStringLiteral("quality/cache"), QStringLiteral("cache-body"));
  settings.setValue(QStringLiteral("feeds"), QStringLiteral("home,updates"));
  settings.sync();

  NativePreferencePatch patch;
  patch.enabled = true;
  patch.backend = Backend::DirectML;
  patch.recipeId = QStringLiteral("janai.spatial.hd2x.v1");
  QString reason;
  QVERIFY2(saveNativePreferences(patch, &reason), qPrintable(reason));

  QSettings check;
  QCOMPARE(check.value(QStringLiteral("player/presetShortcuts/hd2x")).toString(),
           QStringLiteral("Ctrl+1"));
  QCOMPARE(check.value(QStringLiteral("ui/language")).toString(), QStringLiteral("uk"));
  QCOMPARE(check.value(QStringLiteral("quality/cache")).toString(), QStringLiteral("cache-body"));
  QCOMPARE(check.value(QStringLiteral("feeds")).toString(), QStringLiteral("home,updates"));
}

void TestNativePrefs::clearingNativePreferencesLeavesTheRuntimeChoiceAlone() {
  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  settings.setValue(QStringLiteral("runtimeChoice"), QStringLiteral("native"));
  settings.setValue(QLatin1String(kNativeEnabledKey), true);
  settings.endGroup();
  settings.sync();

  clearNativePreferences();

  QSettings check;
  check.beginGroup(nativeSettingsGroup());
  QCOMPARE(check.value(QStringLiteral("runtimeChoice")).toString(), QStringLiteral("native"));
  QVERIFY(!check.contains(QLatin1String(kNativeEnabledKey)));
  check.endGroup();
}

void TestNativePrefs::theModelsDirectoryIsValidatedAndCanonicalised() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = QDir::cleanPath(directory.path());

  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  settings.setValue(QLatin1String(kNativeCustomModelsDirKey), path);
  settings.endGroup();
  settings.sync();

  const NativePreferences loaded = loadNativePreferences();
  QVERIFY(loaded.issues.isEmpty());
  QVERIFY(!loaded.customModelsDir.isEmpty());
  QCOMPARE(loaded.customModelsDir, canonicalPath(path));
  QVERIFY(QFileInfo(loaded.customModelsDir).isDir());
}

void TestNativePrefs::anUnknownStoredModelsDirectoryIsIgnoredAndReported() {
  QSettings settings;
  settings.beginGroup(nativeSettingsGroup());
  settings.setValue(QLatin1String(kNativeCustomModelsDirKey),
                    QStringLiteral("Z:/mikai/definitely-not-here"));
  settings.endGroup();
  settings.sync();

  const NativePreferences loaded = loadNativePreferences();
  QVERIFY(loaded.customModelsDir.isEmpty());
  QCOMPARE(static_cast<int>(loaded.issues.size()), 1);
  QCOMPARE(loaded.issues.first().reasonKey, QStringLiteral("native.modelsDirInvalid"));
}

void TestNativePrefs::aMissingNativeRuntimeNamesTheRuntimeGap() {
  // Nothing selected at all, and a Standard module that was selected: both are
  // "no native runtime", named rather than guessed.
  const RuntimeSelection none;
  const NativeAvailability missing = nativeAvailabilityFor(none, nullptr);
  QVERIFY(!missing.engineAvailable);
  QCOMPARE(missing.engineReasonKey, QStringLiteral("native.runtimeUnavailable"));
  QVERIFY(!missing.engineReason.isEmpty());
  QVERIFY(missing.backends.isEmpty());

  RuntimeSelection standard;
  standard.loaded = true;
  standard.kind = BundleKind::Standard;
  const NativeAvailability fromStandard = nativeAvailabilityFor(standard, nullptr);
  QVERIFY(!fromStandard.engineAvailable);
  QCOMPARE(fromStandard.engineReasonKey, QStringLiteral("native.runtimeUnavailable"));
  QVERIFY(!fromStandard.engineReason.isEmpty());
}

void TestNativePrefs::aLoadedNativeRuntimeWithoutConfirmedCapabilitiesNamesThatGap() {
  RuntimeSelection native;
  native.loaded = true;
  native.kind = BundleKind::Native;
  native.requested = RuntimeChoice::Native;
  native.version = QStringLiteral("9.9.9");

  const NativeAvailability available = nativeAvailabilityFor(native, nullptr);
  // The descriptor's own label is not a capability, so the answer is unchanged
  // by it - and nothing is fabricated to fill the gap.
  QVERIFY(!available.engineAvailable);
  QCOMPARE(available.engineReasonKey, QStringLiteral("native.capabilitiesUnconfirmed"));
  QVERIFY(!available.engineReason.isEmpty());
  QVERIFY(available.backends.isEmpty());
  for (const NativeRecipeOption& option : available.recipes) {
    QVERIFY(option.availability.value == Availability::Unavailable);
    QCOMPARE(option.availability.reasonKey, available.engineReasonKey);
    QCOMPARE(option.availability.reason, available.engineReason);
  }
}

void TestNativePrefs::aRefusedNativeRequestKeepsItsNamedReason() {
  RuntimeSelection refused;
  refused.requested = RuntimeChoice::Native;
  refused.kind = BundleKind::Standard;
  refused.nativeNotApplied = true;
  refused.runtimeUnavailable = true;
  refused.reason = QStringLiteral("No native runtime bundle is installed.");

  const NativeAvailability available = nativeAvailabilityFor(refused, nullptr);
  QVERIFY(!available.engineAvailable);
  QCOMPARE(available.engineReasonKey, QStringLiteral("native.runtimeUnavailable"));
  // The selection's own fact is what is reported, not a re-worded invention.
  QCOMPARE(available.engineReason, refused.reason);
}

void TestNativePrefs::theAvailabilityProducerNeverClaimsAnEngine() {
  // A caller with no observation at all - cold, offline, no status read yet -
  // passes nullptr and gets no engine, with the gap named.
  const NativeAvailability cold = currentNativeAvailability(nullptr);
  QVERIFY(!cold.engineAvailable);
  QVERIFY(!cold.engineReasonKey.isEmpty());
  QVERIFY(!cold.engineReason.isEmpty());
  QVERIFY(cold.backends.isEmpty());

  const RuntimeSelection runtime = loadedNativeRuntime();

  // A loaded native runtime with no observation, and one whose observation never
  // executed, are both unconfirmed: the module label is not a capability.
  const NativeAvailability unobserved = nativeAvailabilityFor(runtime, nullptr);
  QVERIFY(!unobserved.engineAvailable);
  QCOMPARE(unobserved.engineReasonKey, QStringLiteral("native.capabilitiesUnconfirmed"));
  QVERIFY(unobserved.backends.isEmpty());

  const NativeFactoryObservation notRun;
  const NativeAvailability neverRun = nativeAvailabilityFor(runtime, &notRun);
  QVERIFY(!neverRun.engineAvailable);
  QCOMPARE(neverRun.engineReasonKey, QStringLiteral("native.capabilitiesUnconfirmed"));
  QVERIFY(neverRun.backends.isEmpty());
}

void TestNativePrefs::aUsableFactoryObservationPermitsOnlyItsObservedBackend() {
  const RuntimeSelection runtime = loadedNativeRuntime();

  // The executed query's backend token is the only thing that names a backend:
  // the offer is exactly the one the factory was actually checked against.
  const NativeFactoryObservation tensorrt =
      observedFactory(QStringLiteral("tensorrt"), true, false);
  const NativeAvailability fromTensorrt = nativeAvailabilityFor(runtime, &tensorrt);
  QVERIFY(fromTensorrt.engineAvailable);
  QVERIFY(fromTensorrt.engineReasonKey.isEmpty());
  QVERIFY(fromTensorrt.engineReason.isEmpty());
  QCOMPARE(static_cast<int>(fromTensorrt.backends.size()), 1);
  QVERIFY(fromTensorrt.backends.contains(Backend::TensorRT));
  QVERIFY(!fromTensorrt.backends.contains(Backend::DirectML));

  const NativeFactoryObservation directml =
      observedFactory(QStringLiteral("directml"), true, false);
  const NativeAvailability fromDirectml = nativeAvailabilityFor(runtime, &directml);
  QVERIFY(fromDirectml.engineAvailable);
  QCOMPARE(static_cast<int>(fromDirectml.backends.size()), 1);
  QVERIFY(fromDirectml.backends.contains(Backend::DirectML));
  QVERIFY(!fromDirectml.backends.contains(Backend::TensorRT));

  // An existing curated leaf is the request row: the answered spatial role is
  // what makes it selectable, and no row is invented for it. What was asked for
  // is kept, and eligibility is still not activation.
  const NativeRecipeOption* leaf =
      fromTensorrt.findRecipe(QStringLiteral("janai.profile.quality.v1"));
  QVERIFY(leaf != nullptr);
  QVERIFY(leaf->availability.isUsable());

  NativePreferences request;
  request.enabled = true;
  request.backend = Backend::TensorRT;
  request.recipeId = QStringLiteral("janai.profile.quality.v1");
  const NativeSelectionView view = evaluateNativeSelection(request, fromTensorrt);
  QVERIFY(view.requestAvailable);
  QVERIFY(view.requestedEnabled);
  QCOMPARE(view.recipeId, QStringLiteral("janai.profile.quality.v1"));
  QVERIFY(view.backend == Backend::TensorRT);
  QVERIFY(view.availability.value == Availability::Ready);
  QVERIFY(view.availability.isUsable());
}

void TestNativePrefs::anObservedFactoryRefusalKeepsItsOwnNamedReason() {
  const RuntimeSelection runtime = loadedNativeRuntime();

  // The shape the writer publishes for a named refusal: the query executed, and
  // came back unavailable with neither role. The consumer reports that exact fact
  // rather than re-wording it, and nothing is offered to run on.
  const NativeFactoryObservation refused = refusedFactory(
      QStringLiteral("native.factory.device"),
      QStringLiteral("the adopted device did not match the queried factory"));
  const NativeAvailability available = nativeAvailabilityFor(runtime, &refused);
  QVERIFY(!available.engineAvailable);
  QCOMPARE(available.engineReasonKey, refused.reasonKey);
  QCOMPARE(available.engineReason, refused.reason);
  QVERIFY(available.backends.isEmpty());
  for (const NativeRecipeOption& option : available.recipes) {
    QVERIFY(!option.availability.isUsable());
    QCOMPARE(option.availability.reasonKey, refused.reasonKey);
  }

  // A refusal that names nothing still gets one named gap rather than an
  // unavailable row with no reason.
  NativeFactoryObservation bare;
  bare.observed = true;
  const NativeAvailability fromBare = nativeAvailabilityFor(runtime, &bare);
  QVERIFY(!fromBare.engineAvailable);
  QVERIFY(!fromBare.engineReasonKey.isEmpty());
  QVERIFY(!fromBare.engineReason.isEmpty());
}

void TestNativePrefs::aRecipeNeedingAnUnsupportedRoleStaysUnavailable() {
  const RuntimeSelection runtime = loadedNativeRuntime();

  // The query answered the RIFE role but not the spatial one: the engine and its
  // backend are offered, and every spatial leaf still names the missing role.
  const NativeFactoryObservation rifeOnly =
      observedFactory(QStringLiteral("tensorrt"), false, true);
  const NativeAvailability available = nativeAvailabilityFor(runtime, &rifeOnly);
  QVERIFY(available.engineAvailable);
  QVERIFY(available.backends.contains(Backend::TensorRT));
  for (const ProfileTemplate& profile : profileTemplates()) {
    const NativeRecipeOption* option = available.findRecipe(profile.id);
    QVERIFY(option != nullptr);
    QVERIFY(!option->availability.isUsable());
    QCOMPARE(option->availability.reasonKey,
             QStringLiteral("native.recipe.spatialUnsupported"));
    QVERIFY(!option->availability.reason.isEmpty());
  }

  NativePreferences request;
  request.enabled = true;
  request.backend = Backend::TensorRT;
  request.recipeId = QStringLiteral("janai.profile.quality.v1");
  const NativeSelectionView view = evaluateNativeSelection(request, available);
  QVERIFY(!view.requestAvailable);
  QVERIFY(view.requestedEnabled);
  QCOMPARE(view.recipeId, request.recipeId);
  QVERIFY(view.availability.value == Availability::Unavailable);
  QCOMPARE(view.availability.reasonKey, QStringLiteral("native.recipe.spatialUnsupported"));
}

void TestNativePrefs::aStandardModuleIgnoresAUsableFactoryObservation() {
  // A Standard module that took over a refused native request: the module this
  // process pinned decides, and a successful observation published by some
  // source's status does not make the native engine available on it.
  RuntimeSelection standard;
  standard.loaded = true;
  standard.kind = BundleKind::Standard;
  standard.requested = RuntimeChoice::Native;
  standard.nativeNotApplied = true;

  const NativeFactoryObservation observed =
      observedFactory(QStringLiteral("tensorrt"), true, true);
  const NativeAvailability available = nativeAvailabilityFor(standard, &observed);
  QVERIFY(!available.engineAvailable);
  QCOMPARE(available.engineReasonKey, QStringLiteral("native.runtimeUnavailable"));
  QVERIFY(available.backends.isEmpty());
  for (const NativeRecipeOption& option : available.recipes) {
    QVERIFY(!option.availability.isUsable());
    QCOMPARE(option.availability.reasonKey, available.engineReasonKey);
  }
}

void TestNativePrefs::theSharedCheckRefusesReservedKeysInEveryForm() {
  using namespace mplayer::shell;
  const QMap<QString, ShortcutClaim> claims;

  // The player's handlers act on the key alone, so a modifier does not make it
  // assignable.
  for (const QKeySequence& sequence :
       {QKeySequence(Qt::Key_Space), QKeySequence(Qt::CTRL | Qt::Key_S),
        QKeySequence(Qt::Key_Escape)}) {
    const ShortcutConflict conflict = findShortcutConflict(sequence, claims);
    QVERIFY(conflict.blocked);
    QVERIFY(conflict.ownerKind == ShortcutOwnerKind::Reserved);
    QVERIFY(conflict.reasonKey == QLatin1String(kShortcutReservedReasonKey));
  }
}

void TestNativePrefs::theSharedCheckRefusesADuplicateAndNamesTheOwner() {
  using namespace mplayer::shell;
  const QKeySequence sequence(Qt::CTRL | Qt::Key_J);
  QMap<QString, ShortcutClaim> claims;
  claims.insert(shortcutClaimKey(sequence), claim(QStringLiteral("preset.a"),
                                                  QStringLiteral("Preset A"),
                                                  ShortcutOwnerKind::Preset));

  const ShortcutConflict conflict = findShortcutConflict(sequence, claims);
  QVERIFY(conflict.blocked);
  QVERIFY(conflict.ownerKind == ShortcutOwnerKind::Preset);
  QCOMPARE(conflict.ownerId, QStringLiteral("preset.a"));
  QCOMPARE(conflict.ownerLabel, QStringLiteral("Preset A"));
  QVERIFY(conflict.reasonKey == QLatin1String(kShortcutDuplicateReasonKey));
}

void TestNativePrefs::anEmptySequenceIsRefusedAsReserved() {
  using namespace mplayer::shell;
  const QMap<QString, ShortcutClaim> claims;
  const ShortcutConflict conflict = findShortcutConflict(QKeySequence(), claims);
  QVERIFY(conflict.blocked);
  QVERIFY(conflict.ownerKind == ShortcutOwnerKind::Reserved);
}

void TestNativePrefs::theComponentsPromptStartsUndismissedAndStaysDismissed() {
  // Hermetic: main.cpp points QSettings at a throwaway directory for the whole
  // run, so this never touches the viewer's registry.
  QVERIFY(!mplayer::native::nativeComponentsPromptDismissed());
  mplayer::native::dismissNativeComponentsPrompt();
  QVERIFY(mplayer::native::nativeComponentsPromptDismissed());
}

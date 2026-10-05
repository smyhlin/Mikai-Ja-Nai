#include "TestNativeMenu.hpp"

#include "Anime4K.hpp"
#include "native/NativeMenu.hpp"

#include <algorithm>

using mplayer::anime4k::MenuRow;
using mplayer::anime4k::menuRows;
using mplayer::native::Availability;
using mplayer::native::Backend;
using mplayer::native::NativeCadenceCounters;
using mplayer::native::NativeEffectivePlan;
using mplayer::native::NativeInterpolation;
using mplayer::native::NativePreferences;
using mplayer::native::NativeRecipeOption;
using mplayer::native::NativeSelectionView;
using mplayer::native::NativeSize;
using mplayer::native::NativeState;
using mplayer::native::Rational;
using mplayer::native::StatusSnapshot;
using mplayer::native::menu::Action;
using mplayer::native::menu::Inputs;
using mplayer::native::menu::Row;
using mplayer::native::menu::StatusLines;
using mplayer::native::menu::currentRowId;
using mplayer::native::menu::defaultToggleShortcut;
using mplayer::native::menu::levelRows;
using mplayer::native::menu::nativeRows;
using mplayer::native::menu::ownsLevel;
using mplayer::native::menu::rifeRows;
using mplayer::native::menu::rootRows;
using mplayer::native::menu::statusLines;
using mplayer::native::menu::toggleShortcut;
using mplayer::native::rifeModelName;
using mplayer::native::RifeVariant;
using mplayer::native::RifeVersion;

namespace {

NativeRecipeOption recipe(const QString& id, Availability availability) {
  NativeRecipeOption option;
  option.id = id;
  option.label = id;
  option.availability = availability == Availability::Ready
                            ? mplayer::native::readyAvailability()
                            : mplayer::native::unavailableAvailability(
                                  QStringLiteral("native.recipeUnavailable"),
                                  QStringLiteral("The recipe is not available."));
  return option;
}

Inputs unavailableInputs() {
  Inputs inputs;
  // The truth of this build: no engine, nothing reported, nothing requested.
  inputs.availability.engineAvailable = false;
  inputs.availability.engineReasonKey = QStringLiteral("native.engineUnavailable");
  inputs.availability.engineReason = QStringLiteral("The native engine is not available.");
  return inputs;
}

/// A fully eligible catalogue: an engine, the backend it carries, and one
/// Ready recipe. Eligibility says the request *could* run, never that it does.
Inputs eligibleInputs() {
  Inputs inputs;
  inputs.availability.engineAvailable = true;
  inputs.availability.backends = {Backend::DirectML};
  inputs.availability.recipes = {recipe(QStringLiteral("user:abc"), Availability::Ready)};
  inputs.preferences.enabled = true;
  inputs.preferences.backend = Backend::DirectML;
  inputs.preferences.recipeId = QStringLiteral("user:abc");
  return inputs;
}

const Row* findRow(const QVector<Row>& rows, const QString& id) {
  for (const Row& row : rows) {
    if (row.id == id) {
      return &row;
    }
  }
  return nullptr;
}

int indexOfRow(const QVector<Row>& rows, const QString& id) {
  for (int index = 0; index < rows.size(); ++index) {
    if (rows.at(index).id == id) {
      return index;
    }
  }
  return -1;
}

StatusSnapshot activeSnapshot() {
  StatusSnapshot snapshot;
  snapshot.version = 1;
  snapshot.revision = 7;
  snapshot.state = NativeState::Active;
  NativeEffectivePlan plan;
  plan.recipe = QStringLiteral("user:abc");
  plan.provider = QStringLiteral("tensorrt");
  plan.model = QStringLiteral("rife_v4.14");
  plan.spatialStages = 1;
  plan.order = QStringLiteral("beforeSpatial");
  plan.sourceSize = NativeSize{1920, 1080};
  plan.outputSize = NativeSize{3840, 2160};
  plan.sourceFps = Rational{24, 1};
  plan.outputFps = Rational{60, 1};
  NativeInterpolation interpolation;
  interpolation.factor = Rational{5, 2};
  interpolation.model = QStringLiteral("rife_v4.14");
  plan.interpolation = interpolation;
  snapshot.effective = plan;
  NativeCadenceCounters counters;
  counters.framesIn = 100;
  counters.framesOut = 250;
  counters.dropped = 3;
  counters.interpolated = 150;
  snapshot.counters = counters;
  return snapshot;
}

}  // namespace

void TestNativeMenu::offStaysSelectableWhenEverythingElseIsUnavailable() {
  Inputs inputs = unavailableInputs();
  QVector<Row> rows = nativeRows(inputs);
  const Row* off = findRow(rows, QStringLiteral("native/off"));
  QVERIFY(off != nullptr);
  QVERIFY(!off->disabled);
  QVERIFY(off->action == Action::TurnOff);
  QVERIFY(off->current);

  // A request that cannot be applied still leaves Off choosable: that is the
  // one action that always works.
  inputs.preferences.enabled = true;
  rows = nativeRows(inputs);
  off = findRow(rows, QStringLiteral("native/off"));
  QVERIFY(off != nullptr);
  QVERIFY(!off->disabled);
  QVERIFY(!off->current);

  for (const Row& row : rows) {
    if (row.id == QLatin1String("native/off")) {
      continue;
    }
    // Everything that would run something is blocked in this build.
    QVERIFY2(row.disabled, qPrintable(row.id));
  }
}

void TestNativeMenu::unavailableProvidersAndModelsAreDisabledWithAReason() {
  const QVector<Row> rows = nativeRows(unavailableInputs());
  for (const Row& row : rows) {
    QVERIFY2(!row.tooltip.isEmpty(), qPrintable(row.id));
    if (row.id == QLatin1String("native/off")) {
      continue;
    }
    QVERIFY2(row.disabled, qPrintable(row.id));
  }
  const Row* none = findRow(rows, QStringLiteral("native/backend/none"));
  QVERIFY(none != nullptr);
  QVERIFY(none->disabled);
  QCOMPARE(none->tooltip, QStringLiteral("The native engine is not available."));
}

void TestNativeMenu::unavailableModelsAreDisabledEvenWhenTheEngineIsReady() {
  // The engine exists, but the model is not Ready: the model row is disabled
  // with the model's own reason, and the backend the runtime carries stays
  // choosable because nothing blocks it.
  Inputs inputs = eligibleInputs();
  inputs.availability.recipes = {
      recipe(QStringLiteral("user:abc"), Availability::Unavailable)};

  const QVector<Row> rows = nativeRows(inputs);
  const Row* model = findRow(rows, QStringLiteral("native/recipe/user:abc"));
  QVERIFY(model != nullptr);
  QVERIFY(model->disabled);
  QVERIFY(!model->tooltip.isEmpty());
  QVERIFY(model->current);

  const Row* backend = findRow(rows, QStringLiteral("native/backend/directml"));
  QVERIFY(backend != nullptr);
  QVERIFY(!backend->disabled);
}

void TestNativeMenu::emptyCataloguePresentsNoInventedModels() {
  const QVector<Row> rows = nativeRows(unavailableInputs());
  int modelRows = 0;
  bool sawEmptyRow = false;
  for (const Row& row : rows) {
    if (row.id.startsWith(QLatin1String("native/recipe/"))) {
      QFAIL("an empty catalogue must not be filled with models that do not exist");
    }
    if (row.id == QLatin1String("native/no-models")) {
      sawEmptyRow = true;
      ++modelRows;
      QVERIFY(row.disabled);
      QCOMPARE(row.tooltip, QStringLiteral("The native engine is not available."));
    }
  }
  QCOMPARE(modelRows, 1);
  QVERIFY(sawEmptyRow);
}

void TestNativeMenu::currentMarkFollowsStableIdAcrossReorder() {
  Inputs inputs = unavailableInputs();
  inputs.preferences.enabled = true;
  inputs.preferences.recipeId = QStringLiteral("user:abc");
  inputs.availability.recipes = {
      recipe(QStringLiteral("user:abc"), Availability::Ready),
      recipe(QStringLiteral("user:zzz"), Availability::Ready)};

  QVector<Row> rows = nativeRows(inputs);
  const QString marked = currentRowId(inputs.preferences);
  QCOMPARE(marked, QStringLiteral("native/recipe/user:abc"));
  const int firstIndex = indexOfRow(rows, marked);
  QVERIFY(firstIndex >= 0);
  QVERIFY(rows.at(firstIndex).current);

  // The same catalogue, listed in the other order. The mark has to follow the
  // id, not the index.
  std::swap(inputs.availability.recipes[0], inputs.availability.recipes[1]);
  rows = nativeRows(inputs);
  QCOMPARE(currentRowId(inputs.preferences), marked);
  const int secondIndex = indexOfRow(rows, marked);
  QVERIFY(secondIndex >= 0);
  QVERIFY(rows.at(secondIndex).current);
  QVERIFY(firstIndex != secondIndex);
  for (int index = 0; index < rows.size(); ++index) {
    if (index != secondIndex) {
      QVERIFY(!rows.at(index).current);
    }
  }
}

void TestNativeMenu::offRowKeepsTheStoredRequestUntouched() {
  Inputs inputs = unavailableInputs();
  inputs.preferences.enabled = true;
  inputs.preferences.backend = Backend::DirectML;
  inputs.preferences.recipeId = QStringLiteral("user:abc");
  const NativePreferences before = inputs.preferences;

  // The branch is a pure consumer: rendering it twice cannot change what was
  // asked for, and Off names no value a consumer could mistake for the request
  // it is leaving alone.
  const QVector<Row> first = nativeRows(inputs);
  const QVector<Row> second = nativeRows(inputs);
  QVERIFY(before.enabled == inputs.preferences.enabled);
  QVERIFY(before.backend == inputs.preferences.backend);
  QCOMPARE(before.recipeId, inputs.preferences.recipeId);
  QCOMPARE(first.size(), second.size());
  for (int index = 0; index < first.size(); ++index) {
    QCOMPARE(first.at(index).id, second.at(index).id);
    QCOMPARE(first.at(index).current, second.at(index).current);
    QCOMPARE(first.at(index).disabled, second.at(index).disabled);
  }

  const Row* off = findRow(first, QStringLiteral("native/off"));
  QVERIFY(off != nullptr);
  QVERIFY(!off->disabled);
  QVERIFY(off->action == Action::TurnOff);
  QVERIFY(off->value.isEmpty());
  QVERIFY(!off->current);
  QCOMPARE(currentRowId(inputs.preferences), QStringLiteral("native/recipe/user:abc"));
}

void TestNativeMenu::unknownRequestStaysVisibleAsTheCurrentUnavailableChoice() {
  // A saved request the catalogue does not offer has to keep the current mark:
  // no offered row carries its id, and silently dropping the mark would tell
  // the user their stored choice is gone. It stays a request, never an offered
  // recipe and never an activation.
  Inputs inputs = unavailableInputs();
  inputs.preferences.enabled = true;
  inputs.preferences.recipeId = QStringLiteral("user:missing");

  const QVector<Row> rows = nativeRows(inputs);
  const QString marked = currentRowId(inputs.preferences);
  QCOMPARE(marked, QStringLiteral("native/recipe/user:missing"));

  const Row* unknown = findRow(rows, marked);
  QVERIFY(unknown != nullptr);
  QVERIFY(unknown->current);
  QVERIFY(unknown->disabled);
  QVERIFY(unknown->action == Action::None);
  QVERIFY(!unknown->tooltip.isEmpty());
  QVERIFY(unknown->tooltip.contains(inputs.preferences.recipeId));

  // The request is the one marked row, and no other row claims the mark.
  const Row* on = findRow(rows, QStringLiteral("native/on"));
  QVERIFY(on != nullptr);
  QVERIFY(!on->current);
  QVERIFY(on->disabled);
  for (const Row& row : rows) {
    if (row.id != marked) {
      QVERIFY2(!row.current, qPrintable(row.id));
    }
  }
  QVERIFY(!statusLines(inputs).active);
}

void TestNativeMenu::offeredRecipeIsNotEchoedAsAnUnknownRow() {
  // A request the catalogue offers is exactly the offered row, not that row
  // plus a shadow copy under the same id.
  Inputs inputs = eligibleInputs();
  const QVector<Row> rows = nativeRows(inputs);
  QCOMPARE(currentRowId(inputs.preferences), QStringLiteral("native/recipe/user:abc"));

  int matches = 0;
  int currents = 0;
  for (const Row& row : rows) {
    if (row.id == QLatin1String("native/recipe/user:abc")) {
      ++matches;
      QVERIFY(row.current);
      QVERIFY(!row.disabled);
      QVERIFY(row.action == Action::SetRecipe);
    }
    if (row.current) {
      ++currents;
    }
  }
  QCOMPARE(matches, 1);
  QCOMPARE(currents, 1);
}

void TestNativeMenu::requestOffInventsNoUnknownRowOrCurrentOn() {
  // Off is off: a recipe id left in the store is not a request, so no unknown
  // row echoes it and no On row wears the mark.
  Inputs inputs = unavailableInputs();
  inputs.preferences.enabled = false;
  inputs.preferences.recipeId = QStringLiteral("user:missing");

  const QVector<Row> rows = nativeRows(inputs);
  QCOMPARE(currentRowId(inputs.preferences), QStringLiteral("native/off"));
  QVERIFY(findRow(rows, QStringLiteral("native/recipe/user:missing")) == nullptr);

  const Row* on = findRow(rows, QStringLiteral("native/on"));
  QVERIFY(on != nullptr);
  QVERIFY(!on->current);

  int currents = 0;
  for (const Row& row : rows) {
    if (row.current) {
      ++currents;
      QCOMPARE(row.id, QStringLiteral("native/off"));
    }
  }
  QCOMPARE(currents, 1);
}

void TestNativeMenu::effectiveOnlyWhenTheSnapshotSaysActive() {
  // A request that is fully eligible is still not effective until the filter
  // itself reports Active: eligibility is not activation.
  Inputs inputs = eligibleInputs();
  const NativeSelectionView view =
      mplayer::native::evaluateNativeSelection(inputs.preferences, inputs.availability);
  QVERIFY(view.requestAvailable);

  QVector<Row> rows = nativeRows(inputs);
  const Row* effective = findRow(rows, QStringLiteral("native/effective"));
  QVERIFY(effective != nullptr);
  QVERIFY(effective->disabled);
  QVERIFY(!effective->tooltip.isEmpty());
  const QString pendingLabel = effective->label;
  QVERIFY(!statusLines(inputs).active);

  // The eligible rows are choosable now - only the claim of activation waits.
  const Row* on = findRow(rows, QStringLiteral("native/on"));
  QVERIFY(on != nullptr);
  QVERIFY(!on->disabled);
  const Row* model = findRow(rows, QStringLiteral("native/recipe/user:abc"));
  QVERIFY(model != nullptr);
  QVERIFY(!model->disabled);

  // Only the filter's own snapshot turns the request effective.
  inputs.status.snapshot = activeSnapshot();
  QVERIFY(statusLines(inputs).active);
  rows = nativeRows(inputs);
  effective = findRow(rows, QStringLiteral("native/effective"));
  QVERIFY(effective != nullptr);
  QVERIFY(effective->disabled);
  QVERIFY(effective->label != pendingLabel);
}

void TestNativeMenu::rootRowsOwnsNativeAndRifeLevels() {
  const QVector<Row> roots = rootRows(unavailableInputs());
  QCOMPARE(roots.size(), 2);
  QVERIFY(ownsLevel(QStringLiteral("native")));
  QVERIFY(ownsLevel(QStringLiteral("rife")));
  QVERIFY(!ownsLevel(QStringLiteral("realesrgan")));

  const Row* native = findRow(roots, QStringLiteral("native"));
  const Row* rife = findRow(roots, QStringLiteral("rife"));
  QVERIFY(native != nullptr);
  QVERIFY(rife != nullptr);
  // The shell drills with `level|<value>`, so the value is the exact level id.
  QCOMPARE(native->value, QStringLiteral("native"));
  QCOMPARE(rife->value, QStringLiteral("rife"));
  QVERIFY(native->submenu);
  QVERIFY(rife->submenu);
  QVERIFY(!native->disabled);
  QVERIFY(!rife->disabled);
}

namespace {
NativeRecipeOption temporalRecipe(const QString& id, const QString& model,
                                  Availability availability) {
  NativeRecipeOption option;
  option.id = id;
  option.label = id;
  option.availability = availability == Availability::Ready
                            ? mplayer::native::readyAvailability()
                            : mplayer::native::unavailableAvailability(
                                  QStringLiteral("native.recipeUnavailable"),
                                  QStringLiteral("The recipe is not available."));
  option.rifeModel = model;
  return option;
}

QString fullModel() { return rifeModelName(RifeVersion::V4_14, RifeVariant::Full, false); }
QString liteModel() { return rifeModelName(RifeVersion::V4_14, RifeVariant::Lite, false); }
QString fullEnsembleModel() {
  return rifeModelName(RifeVersion::V4_14, RifeVariant::Full, true);
}
QString liteEnsembleModel() {
  return rifeModelName(RifeVersion::V4_14, RifeVariant::Lite, true);
}

Inputs usableRifeInputs(const QString& recipeId, const QString& model) {
  Inputs inputs;
  inputs.availability.engineAvailable = true;
  inputs.availability.backends = {Backend::TensorRT};
  inputs.availability.recipes = {temporalRecipe(recipeId, model, Availability::Ready)};
  inputs.preferences.enabled = true;
  inputs.preferences.backend = Backend::TensorRT;
  inputs.preferences.recipeId = recipeId;
  return inputs;
}
}  // namespace

void TestNativeMenu::rifeRootStaysAvailableWithNoConfiguredRecipe() {
  // No stored temporal recipe: the RIFE root and page stay available so the
  // page can explain the gap, with one honest disabled row per variant.
  const QVector<Row> roots = rootRows(unavailableInputs());
  QVERIFY(findRow(roots, QStringLiteral("rife")) != nullptr);

  const QVector<Row> rows = rifeRows(unavailableInputs());
  QCOMPARE(rows.size(), 4);
  for (const Row& row : rows) {
    QVERIFY(row.disabled);
    QVERIFY(row.action == Action::None);
    QVERIFY(!row.submenu);
    QVERIFY(row.value.isEmpty());
  }
  QVERIFY(findRow(rows, QStringLiteral("rife/") + fullModel()) != nullptr);
  QVERIFY(findRow(rows, QStringLiteral("rife/") + liteModel()) != nullptr);
  QVERIFY(findRow(rows, QStringLiteral("rife/") + fullEnsembleModel()) != nullptr);
  QVERIFY(findRow(rows, QStringLiteral("rife/") + liteEnsembleModel()) != nullptr);
  QVERIFY(ownsLevel(QStringLiteral("rife")));
}

void TestNativeMenu::usableRifeRecipeIsChoosableWithItsExactId() {
  const QString id = QStringLiteral("recipe:sha256:abc:tensorrt");
  const Inputs inputs = usableRifeInputs(id, liteModel());

  const QVector<Row> rows = rifeRows(inputs);
  const Row* row = findRow(rows, QStringLiteral("rife/recipe/") + id);
  QVERIFY(row != nullptr);
  QVERIFY(!row->disabled);
  QVERIFY(row->action == Action::SetRecipe);
  // The shell spells the value into `native|recipe|<value>`, so the exact
  // saved id is what the existing SetRecipe action receives.
  QCOMPARE(row->value, id);
  QVERIFY(row->current);

  // The configured variant leaves no missing-variant shadow behind it.
  QVERIFY(findRow(rows, QStringLiteral("rife/") + liteModel()) == nullptr);
  // The other variants still ask for configuration with the runtime gap.
  const Row* missing = findRow(rows, QStringLiteral("rife/") + fullModel());
  QVERIFY(missing != nullptr);
  QVERIFY(missing->disabled);
  QVERIFY(missing->action == Action::None);
}

void TestNativeMenu::unavailableRifeRecipeStaysDisabledWithItsOwnReason() {
  Inputs inputs;
  inputs.availability.engineAvailable = true;
  inputs.availability.backends = {Backend::TensorRT};
  inputs.availability.recipes = {
      temporalRecipe(QStringLiteral("recipe:sha256:abc:tensorrt"), liteModel(),
                     Availability::Unavailable)};
  inputs.preferences.enabled = true;
  inputs.preferences.backend = Backend::TensorRT;
  inputs.preferences.recipeId = QStringLiteral("recipe:sha256:abc:tensorrt");

  const QVector<Row> rows = rifeRows(inputs);
  const Row* row =
      findRow(rows, QStringLiteral("rife/recipe/recipe:sha256:abc:tensorrt"));
  QVERIFY(row != nullptr);
  QVERIFY(row->disabled);
  QVERIFY(row->action == Action::SetRecipe);
  QCOMPARE(row->value, QStringLiteral("recipe:sha256:abc:tensorrt"));
  QVERIFY(row->current);
  QVERIFY(row->tooltip.contains(QStringLiteral("The recipe is not available.")));
}

void TestNativeMenu::rifeVariantsNeverCrossRouteAndDuplicatesStaySelectable() {
  // Two stored recipes for the same variant stay separately selectable; a full
  // recipe never answers for a lite row.
  Inputs inputs;
  inputs.availability.engineAvailable = true;
  inputs.availability.backends = {Backend::TensorRT};
  inputs.availability.recipes = {
      temporalRecipe(QStringLiteral("recipe:sha256:aaa:tensorrt"), liteModel(),
                     Availability::Ready),
      temporalRecipe(QStringLiteral("recipe:sha256:bbb:tensorrt"), liteModel(),
                     Availability::Ready)};
  inputs.preferences.enabled = true;
  inputs.preferences.backend = Backend::TensorRT;
  inputs.preferences.recipeId = QStringLiteral("recipe:sha256:aaa:tensorrt");

  const QVector<Row> rows = rifeRows(inputs);
  const Row* first = findRow(rows, QStringLiteral("rife/recipe/recipe:sha256:aaa:tensorrt"));
  const Row* second = findRow(rows, QStringLiteral("rife/recipe/recipe:sha256:bbb:tensorrt"));
  QVERIFY(first != nullptr);
  QVERIFY(second != nullptr);
  QVERIFY(!first->disabled);
  QVERIFY(!second->disabled);
  QCOMPARE(first->value, QStringLiteral("recipe:sha256:aaa:tensorrt"));
  QCOMPARE(second->value, QStringLiteral("recipe:sha256:bbb:tensorrt"));
  QVERIFY(first->current);
  QVERIFY(!second->current);
  // The configured variant has no missing shadow; the untouched variants do.
  QVERIFY(findRow(rows, QStringLiteral("rife/") + liteModel()) == nullptr);
  QVERIFY(findRow(rows, QStringLiteral("rife/") + fullModel()) != nullptr);
  QVERIFY(findRow(rows, QStringLiteral("rife/") + fullEnsembleModel()) != nullptr);
  QVERIFY(findRow(rows, QStringLiteral("rife/") + liteEnsembleModel()) != nullptr);
}

void TestNativeMenu::rifeCurrentMarkSurvivesReordering() {
  Inputs inputs;
  inputs.availability.engineAvailable = true;
  inputs.availability.backends = {Backend::TensorRT};
  inputs.availability.recipes = {
      temporalRecipe(QStringLiteral("recipe:sha256:aaa:tensorrt"), liteModel(),
                     Availability::Ready),
      temporalRecipe(QStringLiteral("recipe:sha256:bbb:tensorrt"), fullModel(),
                     Availability::Ready)};
  inputs.preferences.enabled = true;
  inputs.preferences.backend = Backend::TensorRT;
  inputs.preferences.recipeId = QStringLiteral("recipe:sha256:aaa:tensorrt");

  QVector<Row> rows = rifeRows(inputs);
  const int firstIndex =
      indexOfRow(rows, QStringLiteral("rife/recipe/recipe:sha256:aaa:tensorrt"));
  QVERIFY(firstIndex >= 0);
  QVERIFY(rows.at(firstIndex).current);

  std::swap(inputs.availability.recipes[0], inputs.availability.recipes[1]);
  rows = rifeRows(inputs);
  const int secondIndex =
      indexOfRow(rows, QStringLiteral("rife/recipe/recipe:sha256:aaa:tensorrt"));
  QVERIFY(secondIndex >= 0);
  QVERIFY(rows.at(secondIndex).current);
  QVERIFY(firstIndex != secondIndex);
  int currents = 0;
  for (const Row& row : rows) {
    if (row.current) {
      ++currents;
      QCOMPARE(row.id, QStringLiteral("rife/recipe/recipe:sha256:aaa:tensorrt"));
    }
  }
  QCOMPARE(currents, 1);
}

void TestNativeMenu::realesrganAndOtherModelsStayPlaceholders() {
  QVERIFY(!ownsLevel(QStringLiteral("realesrgan")));
  QVERIFY(!ownsLevel(QStringLiteral("other-models")));
  for (const QString& level :
       {QStringLiteral("realesrgan"), QStringLiteral("other-models")}) {
    const QVector<MenuRow> rows = menuRows(level);
    QVERIFY(!rows.isEmpty());
    for (const MenuRow& row : rows) {
      // No selectable row and no drill-down: these pages can change nothing.
      QVERIFY(row.role == MenuRow::Role::Placeholder);
    }
  }
}

void TestNativeMenu::statusStaysHiddenForDefaultOff() {
  const StatusLines lines = statusLines(unavailableInputs());
  QVERIFY(!lines.visible);
  QVERIFY(lines.headline.isEmpty());
  QVERIFY(!lines.active);
}

void TestNativeMenu::statusRendersOnlyWhatTheSnapshotStates() {
  Inputs inputs = unavailableInputs();
  inputs.preferences.enabled = true;
  inputs.status.availability = mplayer::native::readyAvailability();
  inputs.status.snapshot = activeSnapshot();

  const StatusLines lines = statusLines(inputs);
  QVERIFY(lines.visible);
  QVERIFY(lines.active);
  QCOMPARE(lines.provider, QStringLiteral("tensorrt"));
  QCOMPARE(lines.model, QStringLiteral("rife_v4.14"));
  QCOMPARE(lines.geometry, QStringLiteral("1920x1080 → 3840x2160"));
  QCOMPARE(lines.fps, QStringLiteral("24/1 → 60/1"));
  QCOMPARE(lines.interpolation, QStringLiteral("5/2"));
  QCOMPARE(lines.dropped, QStringLiteral("3"));
  QVERIFY(lines.reason.isEmpty());

  // A preparing filter is not Active, and carries its own reason.
  StatusSnapshot preparing;
  preparing.version = 1;
  preparing.revision = 8;
  preparing.state = NativeState::Preparing;
  preparing.reason = QStringLiteral("warming up");
  inputs.status.snapshot = preparing;
  const StatusLines pending = statusLines(inputs);
  QVERIFY(pending.visible);
  QVERIFY(!pending.active);
  QCOMPARE(pending.reason, QStringLiteral("warming up"));
}

void TestNativeMenu::snapshotWithoutGeometryNeverRendersZero() {
  Inputs inputs = unavailableInputs();
  inputs.preferences.enabled = true;
  StatusSnapshot snapshot;
  snapshot.version = 1;
  snapshot.revision = 2;
  snapshot.state = NativeState::Active;
  NativeEffectivePlan plan;
  // The filter published no geometry and no counters: those stay absent.
  plan.provider = QStringLiteral("directml");
  plan.sourceSize = NativeSize{0, 0};
  plan.outputSize = NativeSize{0, 0};
  snapshot.effective = plan;
  inputs.status.snapshot = snapshot;

  const StatusLines lines = statusLines(inputs);
  QVERIFY(lines.visible);
  QVERIFY(lines.geometry.isEmpty());
  QVERIFY(lines.fps.isEmpty());
  QVERIFY(lines.dropped.isEmpty());

  // And with no snapshot at all, the reason is the evaluated refusal - never a
  // zeroed device.
  const StatusLines absent = statusLines(unavailableInputs());
  QVERIFY(!absent.visible);
  QVERIFY(absent.geometry.isEmpty());
}

void TestNativeMenu::toggleShortcutPrefersTheStoredBinding() {
  NativePreferences preferences;
  QVERIFY(!defaultToggleShortcut().isEmpty());
  // Nothing stored yet: the default is what the toggle would install.
  QCOMPARE(toggleShortcut(preferences), defaultToggleShortcut());

  // A stored binding wins, whatever it is.
  preferences.toggleProcessingShortcutStored = true;
  preferences.toggleProcessingShortcut = QStringLiteral("Ctrl+Alt+J");
  QCOMPARE(toggleShortcut(preferences), QStringLiteral("Ctrl+Alt+J"));

  // An explicitly cleared binding stays cleared: the default is not re-offered.
  preferences.toggleProcessingShortcut.clear();
  QVERIFY(toggleShortcut(preferences).isEmpty());
}

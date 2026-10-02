#include "TestNativeProfiles.hpp"

#include "native/NativePrefs.hpp"
#include "native/NativeProfiles.hpp"
#include "native/NativeRecipe.hpp"

#include <QtCore/QSize>

using namespace mplayer::native;

namespace {

// The byte-verified acquisitions, exactly as `native/spatial-acquisition.md`
// records them. A window is matched to a catalogue row by this digest.
constexpr const char* kHdBalancedSha =
    "b308e93c3dda3c4f9f968295b2b7b8d1c0583a711bca53a08d51134d9d1cbd5c";
constexpr const char* kHdPerformanceSha =
    "7b804d87320f37b7269d6e27796046fbd975e95e2aa1f65d5557f52a36282716";
constexpr const char* kSdCompactSha =
    "ded46591ddfaeb22b3daaf84ad7b36838335aa3efca66acfa36177ce4ab21eb5";

const QString kQualityId = QStringLiteral("janai.profile.quality.v1");
const QString kBalancedId = QStringLiteral("janai.profile.balanced.v1");
const QString kPerformanceId = QStringLiteral("janai.profile.performance.v1");

TensorProfile spatialTensor(qint64 h, qint64 w) {
  TensorProfile profile;
  profile.dtype = QStringLiteral("fp16");
  profile.rank = 4;
  profile.batch = 1;
  profile.channels = 3;
  profile.height = h;
  profile.width = w;
  return profile;
}

ModelProfile upscale2x(qint64 h, qint64 w) {
  ModelProfile profile;
  profile.input = spatialTensor(h, w);
  profile.output = spatialTensor(h * 2, w * 2);
  return profile;
}

/// A catalogue row built the way the inventory builds one: its id is the
/// content identity of the digest and declared profile, so no alias is invented
/// for a curated model.
ModelRecord spatialRow(const char* sha, qint64 h, qint64 w) {
  ModelRecord record;
  record.role = ModelRole::Spatial;
  record.origin = ModelOrigin::UserImported;
  record.onnxSha256 = QString::fromLatin1(sha);
  record.actualProfile = upscale2x(h, w);
  record.displayName = record.onnxSha256.left(12);
  record.onnxPath = QStringLiteral("C:/models/%1.onnx").arg(record.onnxSha256.left(12));
  record.availableBackends = {Backend::TensorRT, Backend::DirectML};
  record.availability = readyAvailability();
  record.id = userModelIdFor(
      computeContentId(ModelRole::Spatial, record.onnxSha256, record.actualProfile));
  return record;
}

CatalogView curatedCatalog() {
  CatalogView catalog;
  catalog.models = {
      spatialRow(kHdBalancedSha, 1080, 1920),
      spatialRow(kHdPerformanceSha, 1080, 1920),
      spatialRow(kSdCompactSha, 720, 1280),
  };
  return catalog;
}

QString idOf(const CatalogView& catalog, const char* sha) {
  const QString digest = QString::fromLatin1(sha);
  for (const ModelRecord& record : catalog.models) {
    if (record.onnxSha256 == digest) return record.id;
  }
  return QString();
}

/// Resolves and keeps the refusal key/reason beside the result, so a test can
/// assert both the chain and why nothing came back.
struct Resolved {
  std::optional<Recipe> recipe;
  QString key;
  QString reason;
};

Resolved resolve(const QString& id, const QSize& size, const Rational& rate,
                 const CatalogView& catalog, Backend backend = Backend::TensorRT) {
  Resolved out;
  out.recipe = resolveProfile(id, size, rate, backend, catalog, &out.key, &out.reason);
  return out;
}

}  // namespace

void TestNativeProfiles::theLowWindowOverlapResolvesToHdFirst() {
  const CatalogView catalog = curatedCatalog();
  const QString hdBalanced = idOf(catalog, kHdBalancedSha);
  const QString hdPerformance = idOf(catalog, kHdPerformanceSha);

  // 1280x720 sits on the boundary of the HD and SD windows; the first window in
  // order wins, so it is the HD chain that is chosen.
  const Resolved quality = resolve(kQualityId, QSize(1280, 720), Rational{24, 1}, catalog);
  QVERIFY(quality.recipe.has_value());
  QCOMPARE(quality.recipe->spatial.at(0).modelId, hdBalanced);
  QVERIFY(quality.key.isEmpty());

  const Resolved balanced = resolve(kBalancedId, QSize(1280, 720), Rational{24, 1}, catalog);
  QVERIFY(balanced.recipe.has_value());
  QCOMPARE(balanced.recipe->spatial.at(0).modelId, hdBalanced);

  const Resolved performance =
      resolve(kPerformanceId, QSize(1280, 720), Rational{24, 1}, catalog);
  QVERIFY(performance.recipe.has_value());
  QCOMPARE(performance.recipe->spatial.at(0).modelId, hdPerformance);

  // 1920x1080 is inside the HD window (the upper bound is inclusive).
  const Resolved fullHd = resolve(kQualityId, QSize(1920, 1080), Rational{24, 1}, catalog);
  QVERIFY(fullHd.recipe.has_value());
  QCOMPARE(fullHd.recipe->spatial.at(0).modelId, hdBalanced);
}

void TestNativeProfiles::thePixelAndFrameRateBoundsAreInclusive() {
  const CatalogView catalog = curatedCatalog();
  const QString hdBalanced = idOf(catalog, kHdBalancedSha);
  const QString hdPerformance = idOf(catalog, kHdPerformanceSha);

  // 31 fps is the upper frame-rate bound of the HD window, and it is included.
  for (const QString& id : {kQualityId, kBalancedId}) {
    const Resolved at31 = resolve(id, QSize(1920, 1080), Rational{31, 1}, catalog);
    QVERIFY(at31.recipe.has_value());
    QCOMPARE(at31.recipe->spatial.at(0).modelId, hdBalanced);
  }
  const Resolved performanceAt31 =
      resolve(kPerformanceId, QSize(1920, 1080), Rational{31, 1}, catalog);
  QVERIFY(performanceAt31.recipe.has_value());
  QCOMPARE(performanceAt31.recipe->spatial.at(0).modelId, hdPerformance);

  // A 23.976 rate is compared exactly, and is inside the window.
  const Resolved near24 = resolve(kQualityId, QSize(1920, 1080), Rational{24000, 1001}, catalog);
  QVERIFY(near24.recipe.has_value());
  QCOMPARE(near24.recipe->spatial.at(0).modelId, hdBalanced);
}

void TestNativeProfiles::aboveThirtyOneBalancedAndPerformanceRejectWhileQualityUsesItsThirdWindow() {
  const CatalogView catalog = curatedCatalog();
  const QString hdBalanced = idOf(catalog, kHdBalancedSha);

  // At 32 fps only Quality's third window (0..1920x1080, 0..61 fps) still covers
  // an HD source; the other two profiles have nothing.
  const Resolved quality = resolve(kQualityId, QSize(1920, 1080), Rational{32, 1}, catalog);
  QVERIFY(quality.recipe.has_value());
  QCOMPARE(quality.recipe->spatial.at(0).modelId, hdBalanced);

  for (const QString& id : {kBalancedId, kPerformanceId}) {
    const Resolved refused = resolve(id, QSize(1920, 1080), Rational{32, 1}, catalog);
    QVERIFY(!refused.recipe.has_value());
    QCOMPARE(refused.key, QStringLiteral("native.profile.sourceUnsupported"));
    QVERIFY(!refused.reason.isEmpty());
  }

  // The third window covers a low source at 32 fps as well; the other two still
  // do not.
  const Resolved lowQuality = resolve(kQualityId, QSize(640, 360), Rational{32, 1}, catalog);
  QVERIFY(lowQuality.recipe.has_value());
  QCOMPARE(lowQuality.recipe->spatial.at(0).modelId, hdBalanced);
  QVERIFY(!resolve(kBalancedId, QSize(640, 360), Rational{32, 1}, catalog).recipe.has_value());
}

void TestNativeProfiles::qualityReachesSixtyOneFpsAndNoFurther() {
  const CatalogView catalog = curatedCatalog();
  const QString hdBalanced = idOf(catalog, kHdBalancedSha);

  for (const int fps : {60, 61}) {
    const Resolved quality = resolve(kQualityId, QSize(1920, 1080), Rational{fps, 1}, catalog);
    QVERIFY(quality.recipe.has_value());
    QCOMPARE(quality.recipe->spatial.at(0).modelId, hdBalanced);
    QVERIFY(!resolve(kBalancedId, QSize(1920, 1080), Rational{fps, 1}, catalog)
                 .recipe.has_value());
  }

  const Resolved beyond = resolve(kQualityId, QSize(1920, 1080), Rational{62, 1}, catalog);
  QVERIFY(!beyond.recipe.has_value());
  QCOMPARE(beyond.key, QStringLiteral("native.profile.sourceUnsupported"));

  // A low source at 60 fps is covered by the same third window.
  const Resolved lowSixty = resolve(kQualityId, QSize(640, 360), Rational{60, 1}, catalog);
  QVERIFY(lowSixty.recipe.has_value());
  QCOMPARE(lowSixty.recipe->spatial.at(0).modelId, hdBalanced);
}

void TestNativeProfiles::aLowSourceResolvesSdAtANormalRateAndHdAtSixty() {
  const CatalogView catalog = curatedCatalog();
  const QString sd = idOf(catalog, kSdCompactSha);
  const QString hdBalanced = idOf(catalog, kHdBalancedSha);

  // Below 1280x720, a normal frame rate falls into the SD window of every
  // profile.
  for (const QString& id : {kQualityId, kBalancedId, kPerformanceId}) {
    const Resolved sdResolved = resolve(id, QSize(640, 360), Rational{24, 1}, catalog);
    QVERIFY(sdResolved.recipe.has_value());
    QCOMPARE(sdResolved.recipe->spatial.at(0).modelId, sd);
  }

  // At 60 fps only Quality's third window still matches, on the HD chain.
  const Resolved lowSixty = resolve(kQualityId, QSize(640, 360), Rational{60, 1}, catalog);
  QVERIFY(lowSixty.recipe.has_value());
  QCOMPARE(lowSixty.recipe->spatial.at(0).modelId, hdBalanced);
  QVERIFY(!resolve(kBalancedId, QSize(640, 360), Rational{60, 1}, catalog).recipe.has_value());
  QVERIFY(!resolve(kPerformanceId, QSize(640, 360), Rational{60, 1}, catalog)
               .recipe.has_value());
}

void TestNativeProfiles::anExactFractionalRateIsComparedWithoutRounding() {
  const CatalogView catalog = curatedCatalog();
  const QString hdBalanced = idOf(catalog, kHdBalancedSha);

  // 29.97 and 59.94 are decided by integer cross-multiplication, not by a
  // rounded double that could tip either side of 31 or 61.
  const Resolved near30 = resolve(kQualityId, QSize(1920, 1080), Rational{30000, 1001}, catalog);
  QVERIFY(near30.recipe.has_value());
  QCOMPARE(near30.recipe->spatial.at(0).modelId, hdBalanced);
  QVERIFY(resolve(kBalancedId, QSize(1920, 1080), Rational{30000, 1001}, catalog)
              .recipe.has_value());

  const Resolved near60 = resolve(kQualityId, QSize(1920, 1080), Rational{60000, 1001}, catalog);
  QVERIFY(near60.recipe.has_value());
  QCOMPARE(near60.recipe->spatial.at(0).modelId, hdBalanced);
  QVERIFY(!resolve(kBalancedId, QSize(1920, 1080), Rational{60000, 1001}, catalog)
               .recipe.has_value());

  // 61.93 fps is above Quality's 61 fps bound, exactly.
  const Resolved beyond = resolve(kQualityId, QSize(1920, 1080), Rational{62000, 1001}, catalog);
  QVERIFY(!beyond.recipe.has_value());
  QCOMPARE(beyond.key, QStringLiteral("native.profile.sourceUnsupported"));
}

void TestNativeProfiles::aMissingChosenModelIsRefusedWithoutSubstitution() {
  CatalogView catalog = curatedCatalog();
  // Remove the SD row. A low source at a normal rate chooses the SD window;
  // Quality's third window would still cover it, but that model is a different
  // window's and is never substituted.
  catalog.models.removeAt(2);

  const Resolved quality = resolve(kQualityId, QSize(640, 360), Rational{24, 1}, catalog);
  QVERIFY(!quality.recipe.has_value());
  QCOMPARE(quality.key, QStringLiteral("native.profile.modelMissing"));
  QVERIFY(quality.reason.contains(QStringLiteral("SD_V1beta34")));

  const Resolved balanced = resolve(kBalancedId, QSize(640, 360), Rational{24, 1}, catalog);
  QVERIFY(!balanced.recipe.has_value());
  QCOMPARE(balanced.key, QStringLiteral("native.profile.modelMissing"));

  // An HD source still resolves: its own window's model is present.
  const Resolved hdSource = resolve(kQualityId, QSize(1920, 1080), Rational{24, 1}, catalog);
  QVERIFY(hdSource.recipe.has_value());
}

void TestNativeProfiles::aRowWhoseIdDoesNotDeriveFromItsContentIsRefused() {
  CatalogView catalog = curatedCatalog();
  // The row carries the right digest, but its id is a user id that does not
  // derive from that digest and its declared profile.
  catalog.models[0].id = userModelIdFor(QStringLiteral("sha256:") +
                                       QString(64, QLatin1Char('0')));

  const Resolved quality = resolve(kQualityId, QSize(1920, 1080), Rational{24, 1}, catalog);
  QVERIFY(!quality.recipe.has_value());
  QCOMPARE(quality.key, QStringLiteral("native.profile.identityMismatch"));
}

void TestNativeProfiles::anUnusableChosenModelIsRefused() {
  CatalogView catalog = curatedCatalog();
  catalog.models[2].availability =
      unavailableAvailability(QStringLiteral("model.io.unsupported"),
                              QStringLiteral("the graph declares an unsupported io type"));

  const Resolved quality = resolve(kQualityId, QSize(640, 360), Rational{24, 1}, catalog);
  QVERIFY(!quality.recipe.has_value());
  QCOMPARE(quality.key, QStringLiteral("native.profile.modelUnavailable"));
  QVERIFY(quality.reason.contains(QStringLiteral("unsupported io type")));
}

void TestNativeProfiles::aBackendTheChosenModelDoesNotOfferIsRefused() {
  CatalogView catalog = curatedCatalog();
  catalog.models[0].availableBackends = {Backend::TensorRT};

  const Resolved directMl = resolve(kQualityId, QSize(1920, 1080), Rational{24, 1}, catalog,
                                    Backend::DirectML);
  QVERIFY(!directMl.recipe.has_value());
  QCOMPARE(directMl.key, QStringLiteral("native.profile.backendUnsupported"));
  QVERIFY(directMl.reason.contains(stableName(Backend::DirectML)));

  // The same source resolves on the backend the model does offer.
  const Resolved tensorRt = resolve(kQualityId, QSize(1920, 1080), Rational{24, 1}, catalog,
                                    Backend::TensorRT);
  QVERIFY(tensorRt.recipe.has_value());
  QVERIFY(tensorRt.recipe->backend == Backend::TensorRT);
}

void TestNativeProfiles::anUnknownTemplateAndAnUnknownSourceAreRefusedByName() {
  const CatalogView catalog = curatedCatalog();

  const Resolved unknown = resolve(QStringLiteral("janai.profile.unknown.v1"), QSize(1920, 1080),
                                   Rational{24, 1}, catalog);
  QVERIFY(!unknown.recipe.has_value());
  QCOMPARE(unknown.key, QStringLiteral("native.profile.unknown"));
  QVERIFY(unknown.reason.contains(QStringLiteral("janai.profile.unknown.v1")));

  // Zero, negative or absent geometry, and a rate that is not positive, are all
  // "no source yet" - never read as 0 or 1 fps.
  for (const QSize& size : {QSize(0, 0), QSize(-1, 720), QSize(1920, 0)}) {
    const Resolved undecided = resolve(kQualityId, size, Rational{24, 1}, catalog);
    QVERIFY(!undecided.recipe.has_value());
    QCOMPARE(undecided.key, QStringLiteral("native.profile.needsSource"));
    QVERIFY(!undecided.reason.isEmpty());
  }
  for (const Rational& rate : {Rational{0, 1}, Rational{1, 0}, Rational{-1, 1}}) {
    const Resolved undecided = resolve(kQualityId, QSize(1920, 1080), rate, catalog);
    QVERIFY(!undecided.recipe.has_value());
    QCOMPARE(undecided.key, QStringLiteral("native.profile.needsSource"));
  }
}

void TestNativeProfiles::theResolvedRecipeIsOneStageWithNoResizeAndTheRequestedBackend() {
  const CatalogView catalog = curatedCatalog();
  const Resolved resolved =
      resolve(kQualityId, QSize(1920, 1080), Rational{24, 1}, catalog, Backend::DirectML);
  QVERIFY(resolved.recipe.has_value());

  const Recipe& recipe = *resolved.recipe;
  // The recipe keeps the requested template id, never the resolved model or a
  // prepared build.
  QCOMPARE(recipe.id, kQualityId);
  QCOMPARE(recipe.shape(), RecipeShape::SpatialOnly);
  QCOMPARE(recipe.spatial.size(), 1);
  QVERIFY(!recipe.temporal.has_value());
  QVERIFY(recipe.backend == Backend::DirectML);

  const SpatialStage& stage = recipe.spatial.at(0);
  QCOMPARE(stage.modelId, idOf(catalog, kHdBalancedSha));
  QVERIFY(!stage.preResizeEnabled);
  QVERIFY(!stage.preResize.isValid());

  const RecipeVerdict verdict = validateRecipe(recipe);
  QVERIFY2(verdict.ok, qPrintable(verdict.reason));
}

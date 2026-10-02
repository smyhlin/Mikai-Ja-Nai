#include "TestNativeTypes.hpp"

#include "native/NativeCatalog.hpp"
#include "native/NativeTypes.hpp"

#include <optional>

#include <QtCore/QSet>

using namespace mplayer::native;

namespace {

constexpr const char* kSha =
    "b308e93c3dda3c4f9f968295b2b7b8d1c0583a711bca53a08d51134d9d1cbd5c";
constexpr const char* kOtherSha =
    "7b804d87320f37b7269d6e27796046fbd975e95e2aa1f65d5557f52a36282716";

TensorProfile tensor(const QString& dtype, qint64 n, qint64 c, std::optional<qint64> h = std::nullopt,
                     std::optional<qint64> w = std::nullopt) {
  TensorProfile profile;
  profile.dtype = dtype;
  profile.rank = 4;
  profile.batch = n;
  profile.channels = c;
  profile.height = h;
  profile.width = w;
  return profile;
}

ModelProfile fixed2xProfile() {
  ModelProfile profile;
  profile.input = tensor(QStringLiteral("fp16"), 1, 3, 1080, 1920);
  profile.output = tensor(QStringLiteral("fp16"), 1, 3, 2160, 3840);
  return profile;
}

ModelProfile dynamicProfile() {
  ModelProfile profile;
  profile.input = tensor(QStringLiteral("fp16"), 1, 3);
  profile.output = tensor(QStringLiteral("fp16"), 1, 3);
  return profile;
}

ModelRecord offeredRow(const QString& id, ModelOrigin origin, const ModelProfile& profile,
                       qint64 bytes = 1971970) {
  ModelRecord record;
  record.id = id;
  record.displayName = id;
  record.origin = origin;
  record.onnxPath = QStringLiteral("C:/models/%1.onnx").arg(id);
  record.onnxSha256 = QString::fromLatin1(kSha);
  record.onnxBytes = bytes;
  record.role = ModelRole::Spatial;
  record.actualProfile = profile;
  record.availableBackends = {Backend::TensorRT};
  record.availability = readyAvailability();
  return record;
}

}  // namespace

void TestNativeTypes::enumNamesRoundTripAndRefuseUnknownValues() {
  QCOMPARE(stableName(ModelRole::Spatial), QStringLiteral("spatial"));
  QCOMPARE(stableName(ModelRole::Temporal), QStringLiteral("temporal"));
  QCOMPARE(stableName(Backend::TensorRT), QStringLiteral("tensorrt"));
  QCOMPARE(stableName(Backend::DirectML), QStringLiteral("directml"));
  QCOMPARE(stableName(Availability::Preparing), QStringLiteral("preparing"));
  QCOMPARE(stableName(SpatialRelation::Identity), QStringLiteral("identity"));
  QCOMPARE(stableName(TemporalOrder::BeforeSpatial), QStringLiteral("beforeSpatial"));
  QCOMPARE(stableName(RifeVersion::V4_14), QStringLiteral("4.14"));
  QCOMPARE(stableName(RifeVariant::Lite), QStringLiteral("lite"));

  for (const ModelRole role : {ModelRole::Spatial, ModelRole::Temporal}) {
    QCOMPARE(modelRoleFromName(stableName(role)), std::optional<ModelRole>(role));
  }
  for (const Backend backend : allBackends()) {
    QCOMPARE(backendFromName(stableName(backend)), std::optional<Backend>(backend));
    QVERIFY(backendFromName(QStringLiteral("nonsense")).has_value() == false);
  }

  // An ordinal written where a name belongs, a name from a newer schema, and a
  // differently cased spelling are three ways to be unparseable, and none of them
  // may resolve to the first enumerator.
  QVERIFY(!modelRoleFromName(QStringLiteral("Spatial")).has_value());
  QVERIFY(!modelRoleFromName(QStringLiteral("0")).has_value());
  QVERIFY(!modelRoleFromName(QString()).has_value());
  QVERIFY(!backendFromName(QStringLiteral("cuda")).has_value());
  QVERIFY(!availabilityFromName(QStringLiteral("Ready")).has_value());
  QVERIFY(!spatialRelationFromName(QStringLiteral("Identity")).has_value());
  QVERIFY(!temporalOrderFromName(QStringLiteral("before")).has_value());
  QVERIFY(!rifeVersionFromName(QStringLiteral("4.6")).has_value());
  QVERIFY(!rifeVariantFromName(QStringLiteral("ensemble")).has_value());
}

void TestNativeTypes::aRifeModelNameComesFromRevisionVariantAndEnsemble() {
  QCOMPARE(rifeModelName(RifeVersion::V4_14, RifeVariant::Full, false),
           QStringLiteral("rife_v4.14"));
  QCOMPARE(rifeModelName(RifeVersion::V4_14, RifeVariant::Lite, false),
           QStringLiteral("rife_v4.14_lite"));
  QCOMPARE(rifeModelName(RifeVersion::V4_14, RifeVariant::Full, true),
           QStringLiteral("rife_v4.14_ensemble"));
  QCOMPARE(rifeModelName(RifeVersion::V4_14, RifeVariant::Lite, true),
           QStringLiteral("rife_v4.14_lite_ensemble"));

  // Four distinct names, and the conf code is 414/4141 regardless of ensembling.
  QSet<QString> names;
  for (const RifeVariant variant : {RifeVariant::Full, RifeVariant::Lite}) {
    for (const bool ensemble : {false, true}) {
      names.insert(rifeModelName(RifeVersion::V4_14, variant, ensemble));
    }
  }
  QCOMPARE(names.size(), 4);
  QCOMPARE(rifeModelCode(RifeVariant::Full), 414);
  QCOMPARE(rifeModelCode(RifeVariant::Lite), 4141);
}

void TestNativeTypes::anUnreducedRationalStaysUnreducedAndStillReadsAsItsOwnValue() {
  const Rational requested{4, 2};
  QVERIFY(requested.isValid());
  QVERIFY(!requested.isCanonical());
  QCOMPARE(requested.toString(), QStringLiteral("4/2"));

  // Reduced on request, without rewriting what was asked for.
  QCOMPARE(requested.reduced(), Rational(2, 1));
  QCOMPARE(requested, Rational(4, 2));
  QVERIFY(Rational(2, 1).isCanonical());
  QVERIFY(Rational(1, 2).isCanonical());
  QCOMPARE(Rational(60, 24).reduced(), Rational(5, 2));

  // 4/2 and 2/1 mean the same frame rate, so a caller comparing by meaning must
  // reduce first - but they are different requests and must not compare equal.
  QVERIFY(!(requested == Rational(2, 1)));
}

void TestNativeTypes::aRationalRefusesNonPositiveNumeratorOrDenominator() {
  QVERIFY(!Rational(0, 1).isValid());
  QVERIFY(!Rational(2, 0).isValid());
  QVERIFY(!Rational(-2, 1).isValid());
  QVERIFY(!Rational(2, -1).isValid());
  QVERIFY(Rational(1, 1).isValid());

  // Reducing an invalid pair must not turn it into a valid one.
  QCOMPARE(Rational(0, 5).reduced(), Rational(0, 5));
  QVERIFY(!Rational(0, 5).isCanonical());
}

void TestNativeTypes::availabilityCarriesAMandatoryReasonWheneverItIsNotReady() {
  const AvailabilityState ready = readyAvailability();
  QCOMPARE(ready.value, Availability::Ready);
  QVERIFY(ready.reason.isEmpty());
  QVERIFY(ready.hasMandatoryReason());
  QVERIFY(ready.isUsable());

  const AvailabilityState preparing = preparingAvailability(
      QStringLiteral("model.building"), QStringLiteral("engine is being built"));
  QCOMPARE(preparing.value, Availability::Preparing);
  QVERIFY(!preparing.isUsable());
  QVERIFY(preparing.hasMandatoryReason());

  const AvailabilityState unavailable = unavailableAvailability(
      QStringLiteral("model.missing"), QStringLiteral("the model file is not on disk"));
  QCOMPARE(unavailable.value, Availability::Unavailable);
  QVERIFY(!unavailable.isUsable());
  QCOMPARE(unavailable.reasonKey, QStringLiteral("model.missing"));

  // The invariant is why a factory exists: a state built by hand can violate it,
  // and hasMandatoryReason() is what catches that.
  AvailabilityState lying;
  lying.value = Availability::Unavailable;
  QVERIFY(!lying.hasMandatoryReason());
  lying.reason = QStringLiteral("something");
  QVERIFY(!lying.hasMandatoryReason());
  lying.reasonKey = QStringLiteral("model.missing");
  QVERIFY(lying.hasMandatoryReason());

  // Ready must carry no reason: "available, but here is why it sometimes is not"
  // is a different state and has no enum value.
  AvailabilityState chatty = readyAvailability();
  chatty.reason = QStringLiteral("was preparing earlier");
  QVERIFY(!chatty.hasMandatoryReason());
  QVERIFY(!chatty.isUsable());
}

void TestNativeTypes::modelIdentitySeparatesBuiltInNamesFromContentAddressedImports() {
  QVERIFY(isValidModelId(QStringLiteral("2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16")));
  QVERIFY(!isValidModelId(QString()));
  QVERIFY(!isValidModelId(QStringLiteral("has space")));
  QVERIFY(!isValidModelId(QStringLiteral(" leading")));
  QVERIFY(!isValidModelId(QStringLiteral("trailing ")));
  QVERIFY(!isValidModelId(QString(300, QLatin1Char('x'))));

  const QString userId = userModelIdFor(computeContentId(
      ModelRole::Spatial, QString::fromLatin1(kSha), fixed2xProfile()));
  QVERIFY(isUserModelId(userId));
  QVERIFY(!isBuiltinModelId(userId));
  QVERIFY(isBuiltinModelId(QStringLiteral("rife_v4.14")));
  QVERIFY(!isBuiltinModelId(QString()));

  QVERIFY(isSha256Hex(QString::fromLatin1(kSha)));
  QVERIFY(!isSha256Hex(QString()));
  QVERIFY(!isSha256Hex(QString::fromLatin1(kSha).left(63)));
  QVERIFY(!isSha256Hex(QString::fromLatin1(kSha) + QStringLiteral("g")));
  QVERIFY(!isSha256Hex(QString::fromLatin1(kSha).toUpper()));
}

void TestNativeTypes::contentIdentityMovesWithTheGraphAndNotWithTheMachine() {
  const ModelProfile profile = fixed2xProfile();
  const QString a = computeContentId(ModelRole::Spatial, QString::fromLatin1(kSha), profile);

  QCOMPARE(computeContentId(ModelRole::Spatial, QString::fromLatin1(kSha), profile), a);
  QVERIFY(a.startsWith(QStringLiteral("sha256:")));
  QCOMPARE(a.size(), QStringLiteral("sha256:").size() + 64);

  // Same basename, different bytes: a basename-based identity would collide here.
  const QString other =
      computeContentId(ModelRole::Spatial, QString::fromLatin1(kOtherSha), profile);
  QVERIFY(a != other);
  QVERIFY(userModelIdFor(a) != userModelIdFor(other));

  // The role is part of identity: the same bytes inspected as a temporal model
  // are a different claim about the graph.
  QVERIFY(a != computeContentId(ModelRole::Temporal, QString::fromLatin1(kSha), profile));

  // So is the declared I/O.
  ModelProfile otherIo = profile;
  otherIo.output.dtype = QStringLiteral("fp32");
  QVERIFY(a != computeContentId(ModelRole::Spatial, QString::fromLatin1(kSha), otherIo));

  // A dynamic graph and a fixed one are not the same declaration.
  QVERIFY(a != computeContentId(ModelRole::Spatial, QString::fromLatin1(kSha), dynamicProfile()));

  // The record's own id is derived from its fields, so it cannot go stale beside
  // the row it describes.
  ModelRecord record = offeredRow(QStringLiteral("builtin.hd.balanced"),
                                  ModelOrigin::Builtin, profile);
  QCOMPARE(record.contentId(), a);
  record.actualProfile = otherIo;
  QVERIFY(record.contentId() != a);
}

void TestNativeTypes::anUnsupportedDtypeIsRefusedByNameRatherThanConverted() {
  QVERIFY(isEligibleIoType(QStringLiteral("fp16")));
  QVERIFY(!isEligibleIoType(QStringLiteral("fp32")));
  QVERIFY(!isEligibleIoType(QStringLiteral("bf16")));
  QVERIFY(!isEligibleIoType(QString()));

  const ModelProfile profile = fixed2xProfile();
  QVERIFY(spatialEligibilityRefusal(profile).isEmpty());
  QVERIFY(offeredRow(QStringLiteral("builtin.hd.balanced"), ModelOrigin::Builtin, profile)
              .isOffered());

  // Both tensors are checked: an fp16 input with an fp32 output is not an fp16
  // graph, and the refusal names what was actually declared.
  ModelProfile mixed = profile;
  mixed.output.dtype = QStringLiteral("fp32");
  const QString mixedRefusal = spatialEligibilityRefusal(mixed);
  QVERIFY(mixedRefusal.contains(QStringLiteral("fp16")));
  QVERIFY(mixedRefusal.contains(QStringLiteral("fp32")));

  QString key;
  QString reason;
  ModelRecord fp32 = offeredRow(QStringLiteral("builtin.hd.balanced"), ModelOrigin::Builtin, mixed);
  fp32.refusal(&key, &reason);
  QCOMPARE(key, QStringLiteral("model.profile.ineligible"));
  QVERIFY(reason.contains(QStringLiteral("fp32")));
  QVERIFY(reason.contains(QStringLiteral("fp16")));

  // A record with no digest is not identifiable: a basename is not identity.
  ModelRecord anonymous =
      offeredRow(QStringLiteral("builtin.hd.balanced"), ModelOrigin::Builtin, profile);
  anonymous.onnxSha256.clear();
  anonymous.refusal(&key, &reason);
  QCOMPARE(key, QStringLiteral("model.digest.missing"));

  // A built-in name on a user row is a contradiction, and so is a content
  // address claimed as a built-in name.
  ModelRecord mislabelled =
      offeredRow(QStringLiteral("builtin.hd.balanced"), ModelOrigin::UserImported, profile);
  mislabelled.refusal(&key, &reason);
  QCOMPARE(key, QStringLiteral("model.id.builtin.not.user"));

  ModelRecord inverted =
      offeredRow(QStringLiteral("builtin.hd.balanced"), ModelOrigin::Builtin, profile);
  inverted.id = userModelIdFor(computeContentId(ModelRole::Spatial, QString::fromLatin1(kSha),
                                                profile));
  inverted.refusal(&key, &reason);
  QCOMPARE(key, QStringLiteral("model.id.user.not.builtin"));
}

void TestNativeTypes::aSymbolicOutputBatchIsHonouredOnlyWithAProvingRun() {
  ModelProfile declared = dynamicProfile();
  declared.output.batch = -1;
  ModelRecord record =
      offeredRow(QStringLiteral("builtin.hd.balanced"), ModelOrigin::Builtin, declared);
  QVERIFY(!record.isOffered());
  QCOMPARE(record.actualProfile.output.batch, -1);

  const ModelProfile proof = fixed2xProfile();
  record.verifiedReferenceProfile = proof;
  QVERIFY(record.isOffered());
  QCOMPARE(record.actualProfile.output.batch, -1);
  const QString identity =
      computeContentId(record.role, record.onnxSha256, record.actualProfile);

  for (int boundary = 0; boundary < 5; ++boundary) {
    ModelProfile invalid = proof;
    switch (boundary) {
      case 0: invalid.output.batch = 2; break;
      case 1: invalid.output.channels = 4; break;
      case 2: invalid.output.rank = 3; break;
      case 3: invalid.output.dtype = QStringLiteral("fp32"); break;
      case 4: invalid.output.width = 3841; break;
    }
    record.verifiedReferenceProfile = invalid;
    QVERIFY(!record.isOffered());
    QCOMPARE(computeContentId(record.role, record.onnxSha256, record.actualProfile), identity);
  }

  declared.input.batch = -1;
  record.actualProfile = declared;
  record.verifiedReferenceProfile = proof;
  QVERIFY(!record.isOffered());
}

void TestNativeTypes::aDynamicProfileAdmitsNoKnownGeometry() {
  const ModelProfile dynamic = dynamicProfile();
  QVERIFY(dynamic.isDynamicGeometry());
  QVERIFY(!dynamic.sourceGeometry().has_value());
  QVERIFY(!dynamic.outputGeometry().has_value());

  // A dynamic graph's scale is not learned until its geometry resolves, so no
  // factor may be reported - including a default of 1 or 2.
  QVERIFY(!dynamic.derivedUniformScale().has_value());

  // The declared shape still reports what the graph declared, with H and W named
  // as dynamic rather than substituted.
  QCOMPARE(dynamic.input.describe(), QStringLiteral("1x3xHxW"));
  QCOMPARE(fixed2xProfile().input.describe(), QStringLiteral("1x3x1080x1920"));

  // A dynamic profile is not refused for being dynamic; the FP16 NCHW boundary
  // rules are what an eligibility check applies, and it passes them.
  QVERIFY(spatialEligibilityRefusal(dynamic).isEmpty());
  QVERIFY(offeredRow(QStringLiteral("builtin.sd.compact"), ModelOrigin::Builtin, dynamic)
              .isOffered());

  // A fixed 2x graph learns its factor from the declared shapes, identity is 1,
  // and a non-uniform or non-integer multiple yields nothing rather than a guess.
  QCOMPARE(fixed2xProfile().derivedUniformScale(), std::optional<int>(2));
  QCOMPARE(fixed2xProfile().sourceGeometry().value(), QSize(1920, 1080));
  QCOMPARE(fixed2xProfile().outputGeometry().value(), QSize(3840, 2160));

  ModelProfile identity;
  identity.input = tensor(QStringLiteral("fp16"), 1, 3, 1080, 1920);
  identity.output = tensor(QStringLiteral("fp16"), 1, 3, 1080, 1920);
  QCOMPARE(identity.derivedUniformScale(), std::optional<int>(1));

  ModelProfile nonUniform = fixed2xProfile();
  nonUniform.output.height = 3000;
  QVERIFY(!nonUniform.derivedUniformScale().has_value());
  QVERIFY(spatialEligibilityRefusal(nonUniform).contains(QStringLiteral("not a uniform")));

  ModelProfile fractional = fixed2xProfile();
  fractional.output.height = 1620;
  QVERIFY(!fractional.derivedUniformScale().has_value());

  // Wrong rank, batch, channel count or a non-positive dimension is refused,
  // naming what was declared.
  ModelProfile rank5 = fixed2xProfile();
  rank5.input.rank = 5;
  QVERIFY(spatialEligibilityRefusal(rank5).contains(QStringLiteral("rank 5")));

  ModelProfile batch2 = fixed2xProfile();
  batch2.output.batch = 2;
  QVERIFY(spatialEligibilityRefusal(batch2).contains(QStringLiteral("N=1")));

  ModelProfile channels4 = fixed2xProfile();
  channels4.input.channels = 4;
  QVERIFY(spatialEligibilityRefusal(channels4).contains(QStringLiteral("C=3")));

  ModelProfile zeroHeight = fixed2xProfile();
  zeroHeight.input.height = 0;
  QVERIFY(spatialEligibilityRefusal(zeroHeight).contains(QStringLiteral("positive")));

  // A temporal row declares a different boundary; the spatial NCHW rules must not
  // be applied to it, or a genuine RIFE model would be refused for not being
  // 1x3xHxW. No temporal shape is invented here either.
  ModelRecord temporal;
  temporal.id = QStringLiteral("rife_v4.14");
  temporal.origin = ModelOrigin::Builtin;
  temporal.onnxSha256 = QString::fromLatin1(kSha);
  temporal.role = ModelRole::Temporal;
  temporal.availability = readyAvailability();
  temporal.availableBackends = {Backend::TensorRT};
  QString temporalKey;
  QString temporalReason;
  temporal.refusal(&temporalKey, &temporalReason);
  QVERIFY2(temporalKey.isEmpty(), qPrintable(temporalReason));
  QVERIFY(temporal.isOffered());
}
#include "NativeStatusInternal.hpp"
#include "NativeStatus.hpp"

#include <limits>
#include <numeric>
#include <utility>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QRect>
#include <QtCore/QSize>
#include <QtCore/QStringList>

#include "native/bundle/NativeDiagnostics.hpp"

namespace mplayer {
namespace native {


using namespace status_detail;

namespace {

/// A whole number that a JSON number denotes. Qt 6 keeps a parsed integer literal
/// in 64-bit storage, so it is read with `toInteger` and never `toDouble`, which
/// would round past 2^53 and silently turn one revision into another. A non-number,
/// a fractional value or one beyond the bound is refused by returning false.
bool jsonIsInteger(const QJsonValue& value, qint64* out) {
  if (!value.isDouble()) return false;
  // Any value outside the bound is refused below, so a sentinel outside it can
  // only ever mean "no whole number here".
  const qint64 sentinel = std::numeric_limits<qint64>::min();
  const qint64 wide = value.toInteger(sentinel);
  if (wide == sentinel) return false;
  if (wide < -kNativeStatusMaxExactInteger || wide > kNativeStatusMaxExactInteger) return false;
  *out = wide;
  return true;
}

bool jsonInt64(const QJsonObject& object, const QString& key, qint64* out) {
  return jsonIsInteger(object.value(key), out);
}

bool jsonInt(const QJsonObject& object, const QString& key, int* out) {
  qint64 wide = 0;
  if (!jsonInt64(object, key, &wide)) return false;
  // The exact bound is wider than int, so a whole number too large for the member
  // it names is still refused here.
  if (wide < std::numeric_limits<int>::min() || wide > std::numeric_limits<int>::max()) return false;
  *out = static_cast<int>(wide);
  return true;
}

QString jsonString(const QJsonObject& object, const QString& key) {
  return object.value(key).toString();
}

/// Rejects a member this schema does not define: a silently ignored typo would
/// read as something the document never said.
bool noUnknownKeys(const QJsonObject& object, const QStringList& known) {
  for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
    if (!known.contains(it.key())) return false;
  }
  return true;
}

/// True when any string in the document carries a URL or credential indicator;
/// untrusted text is refused whole rather than echoed back later.
bool jsonCarriesSensitiveText(const QJsonValue& value) {
  if (value.isString()) return containsSensitiveNativeText(value.toString());
  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
      // The member name is untrusted too: a refusal must not repeat it.
      if (containsSensitiveNativeText(it.key()) || jsonCarriesSensitiveText(it.value())) {
        return true;
      }
    }
    return false;
  }
  if (value.isArray()) {
    for (const QJsonValue& entry : value.toArray()) {
      if (jsonCarriesSensitiveText(entry)) return true;
    }
  }
  return false;
}

/// The free-text fields checked as one set: a credential in any is the same named
/// refusal.
StatusSnapshot redactedStatusSnapshot(const StatusSnapshot& snapshot) {
  StatusSnapshot copy = snapshot;
  copy.reason = redactNativeDiagnosticText(copy.reason);
  if (copy.requested.has_value()) {
    copy.requested->recipeId = redactNativeDiagnosticText(copy.requested->recipeId);
  }
  if (copy.effective.has_value()) {
    NativeEffectivePlan& plan = *copy.effective;
    plan.recipe = redactNativeDiagnosticText(plan.recipe);
    plan.provider = redactNativeDiagnosticText(plan.provider);
    plan.model = redactNativeDiagnosticText(plan.model);
    plan.order = redactNativeDiagnosticText(plan.order);
    plan.decoder = redactNativeDiagnosticText(plan.decoder);
    plan.csp = redactNativeDiagnosticText(plan.csp);
    if (plan.interpolation.has_value()) {
      plan.interpolation->model = redactNativeDiagnosticText(plan.interpolation->model);
    }
  }
  if (copy.probe.has_value()) {
    NativeProbePayload& probe = *copy.probe;
    if (probe.source.has_value()) {
      SourceGeometry& source = *probe.source;
      source.csp = redactNativeDiagnosticText(source.csp);
      source.matrix = redactNativeDiagnosticText(source.matrix);
      source.range = redactNativeDiagnosticText(source.range);
      source.siting = redactNativeDiagnosticText(source.siting);
    }
    if (probe.device.has_value()) {
      DeviceIdentity& device = *probe.device;
      device.decoderDeviceUuid = redactNativeDiagnosticText(device.decoderDeviceUuid);
      device.computeDeviceUuid = redactNativeDiagnosticText(device.computeDeviceUuid);
      device.adapterLuid = redactNativeDiagnosticText(device.adapterLuid);
    }
  }
  if (copy.factory.has_value()) {
    NativeFactoryObservation& factory = *copy.factory;
    factory.backend = redactNativeDiagnosticText(factory.backend);
    factory.runtimeVersion = redactNativeDiagnosticText(factory.runtimeVersion);
    factory.deviceIdentity = redactNativeDiagnosticText(factory.deviceIdentity);
    factory.reasonKey = redactNativeDiagnosticText(factory.reasonKey);
    factory.reason = redactNativeDiagnosticText(factory.reason);
  }
  return copy;
}

/// A tag is a strict decimal scalar: the outer layer is untyped, so it is read
/// rather than left to `QString::toInt`'s tolerance.
bool isDecimalText(const QString& text) {
  if (text.isEmpty()) return false;
  for (const QChar c : text) {
    if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
  }
  return true;
}

QStringList documentKeys() {
  return {QStringLiteral("schema"),   QStringLiteral("protocol"), QStringLiteral("revision"),
          QStringLiteral("state"),    QStringLiteral("epoch"),    QStringLiteral("requested"),
          QStringLiteral("effective"), QStringLiteral("counters"), QStringLiteral("probe"),
          QStringLiteral("factory"),  QStringLiteral("reason")};
}

QStringList requestedKeys() {
  return {QStringLiteral("recipe"), QStringLiteral("backend")};
}

QStringList effectiveKeys() {
  return {QStringLiteral("recipe"),       QStringLiteral("provider"),   QStringLiteral("model"),
          QStringLiteral("spatialStages"), QStringLiteral("interpolation"),
          QStringLiteral("order"),        QStringLiteral("sourceSize"),
          QStringLiteral("outputSize"),   QStringLiteral("sourceFps"),
          QStringLiteral("outputFps"),    QStringLiteral("decoder"),    QStringLiteral("csp")};
}

QStringList interpolationKeys() {
  return {QStringLiteral("num"), QStringLiteral("den"), QStringLiteral("model")};
}

QStringList sizeKeys() {
  return {QStringLiteral("w"), QStringLiteral("h")};
}

QStringList rateKeys() {
  return {QStringLiteral("num"), QStringLiteral("den")};
}

QStringList counterKeys() {
  return {QStringLiteral("framesIn"),  QStringLiteral("framesOut"), QStringLiteral("dropped"),
          QStringLiteral("interpolated"), QStringLiteral("sceneCuts")};
}

QStringList probeKeys() {
  return {QStringLiteral("source"), QStringLiteral("device")};
}

QStringList probeSourceKeys() {
  return {QStringLiteral("codedW"),  QStringLiteral("codedH"), QStringLiteral("crop"),
          QStringLiteral("rotation"), QStringLiteral("fps"),  QStringLiteral("csp"),
          QStringLiteral("matrix"),   QStringLiteral("range"), QStringLiteral("siting"),
          QStringLiteral("bitDepth")};
}

QStringList probeRectKeys() {
  return {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("w"), QStringLiteral("h")};
}

QStringList probeDeviceKeys() {
  return {QStringLiteral("decoderDeviceUuid"), QStringLiteral("computeDeviceUuid"),
          QStringLiteral("adapterLuid"), QStringLiteral("smMajor"), QStringLiteral("smMinor")};
}

bool readRate(const QJsonObject& object, const QString& member, Rational* out, QString* errorKey,
              QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  const QJsonValue value = object.value(member);
  if (!value.isObject()) {
    return reject(QStringLiteral("native.status.effective.rate.malformed"),
                  QStringLiteral("the effective %1 is not an object").arg(member));
  }
  const QJsonObject rate = value.toObject();
  if (!noUnknownKeys(rate, rateKeys())) {
    return reject(QStringLiteral("native.status.effective.rate.unknown.member"),
                  QStringLiteral("the effective %1 carries a key this schema does not define")
                      .arg(member));
  }
  Rational parsed;
  if (!jsonInt(rate, QStringLiteral("num"), &parsed.num) ||
      !jsonInt(rate, QStringLiteral("den"), &parsed.den)) {
    return reject(QStringLiteral("native.status.effective.rate.malformed"),
                  QStringLiteral("the effective %1 needs integer num and den").arg(member));
  }
  if (!rateOk(parsed)) {
    return reject(QStringLiteral("native.status.effective.cadence"),
                  QStringLiteral("the effective %1 is %2; both sides must be positive and at most "
                                 "%3")
                      .arg(member, showRate(parsed))
                      .arg(kNativeStatusMaxRateComponent));
  }
  *out = parsed;
  return true;
}

bool readSize(const QJsonObject& object, const QString& member, NativeSize* out, QString* errorKey,
              QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  const QJsonValue value = object.value(member);
  if (!value.isObject()) {
    return reject(QStringLiteral("native.status.effective.geometry.malformed"),
                  QStringLiteral("the effective %1 is not an object").arg(member));
  }
  const QJsonObject size = value.toObject();
  if (!noUnknownKeys(size, sizeKeys())) {
    return reject(QStringLiteral("native.status.effective.geometry.unknown.member"),
                  QStringLiteral("the effective %1 carries a key this schema does not define")
                      .arg(member));
  }
  int w = 0;
  int h = 0;
  if (!jsonInt(size, QStringLiteral("w"), &w) || !jsonInt(size, QStringLiteral("h"), &h)) {
    return reject(QStringLiteral("native.status.effective.geometry.malformed"),
                  QStringLiteral("the effective %1 needs integer w and h").arg(member));
  }
  if (w <= 0 || h <= 0) {
    return reject(QStringLiteral("native.status.effective.geometry"),
                  QStringLiteral("the effective %1 is %2x%3; both sides must be positive")
                      .arg(member)
                      .arg(w)
                      .arg(h));
  }
  out->w = w;
  out->h = h;
  return true;
}

bool readEffective(const QJsonValue& value, NativeEffectivePlan* plan, QString* errorKey,
                   QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  if (!value.isObject()) {
    return reject(QStringLiteral("native.status.effective.not.an.object"),
                  QStringLiteral("the effective member is not an object"));
  }
  const QJsonObject object = value.toObject();
  if (!noUnknownKeys(object, effectiveKeys())) {
    return reject(QStringLiteral("native.status.effective.unknown.member"),
                  QStringLiteral("the effective plan carries a key this schema does not define"));
  }

  NativeEffectivePlan parsed;
  parsed.recipe = jsonString(object, QStringLiteral("recipe"));
  parsed.provider = jsonString(object, QStringLiteral("provider"));
  parsed.model = jsonString(object, QStringLiteral("model"));
  parsed.order = jsonString(object, QStringLiteral("order"));
  parsed.decoder = jsonString(object, QStringLiteral("decoder"));
  parsed.csp = jsonString(object, QStringLiteral("csp"));
  const struct {
    const char* member;
    const QString* value;
  } required[] = {{"recipe", &parsed.recipe},   {"provider", &parsed.provider},
                  {"model", &parsed.model},     {"order", &parsed.order},
                  {"decoder", &parsed.decoder}, {"csp", &parsed.csp}};
  for (const auto& field : required) {
    if (field.value->isEmpty()) {
      const QString member = QString::fromLatin1(field.member);
      return reject(QStringLiteral("native.status.effective.%1.missing").arg(member),
                    QStringLiteral("the effective plan states no %1").arg(member));
    }
  }

  if (!jsonInt(object, QStringLiteral("spatialStages"), &parsed.spatialStages) ||
      parsed.spatialStages < 0) {
    return reject(QStringLiteral("native.status.effective.stages.malformed"),
                  QStringLiteral("the effective plan has no non-negative integer spatialStages"));
  }

  const QJsonValue interpolation = object.value(QStringLiteral("interpolation"));
  if (!interpolation.isUndefined() && !interpolation.isNull()) {
    if (!interpolation.isObject()) {
      return reject(QStringLiteral("native.status.effective.interpolation.not.an.object"),
                    QStringLiteral("the effective interpolation is neither null nor an object"));
    }
    const QJsonObject stage = interpolation.toObject();
    if (!noUnknownKeys(stage, interpolationKeys())) {
      return reject(
          QStringLiteral("native.status.effective.interpolation.unknown.member"),
          QStringLiteral("the effective interpolation carries a key this schema does not define"));
    }
    NativeInterpolation stageParsed;
    if (!jsonInt(stage, QStringLiteral("num"), &stageParsed.factor.num) ||
        !jsonInt(stage, QStringLiteral("den"), &stageParsed.factor.den)) {
      return reject(QStringLiteral("native.status.effective.interpolation.malformed"),
                    QStringLiteral("the effective interpolation needs integer num and den"));
    }
    if (!rateOk(stageParsed.factor)) {
      return reject(QStringLiteral("native.status.effective.cadence"),
                    QStringLiteral("the effective interpolation factor is %1; both sides must be "
                                   "positive and at most %2")
                        .arg(showRate(stageParsed.factor))
                        .arg(kNativeStatusMaxRateComponent));
    }
    stageParsed.model = jsonString(stage, QStringLiteral("model"));
    if (stageParsed.model.isEmpty()) {
      return reject(QStringLiteral("native.status.effective.interpolation.model.missing"),
                    QStringLiteral("the effective interpolation names no model"));
    }
    parsed.interpolation = stageParsed;
  }

  if (!readSize(object, QStringLiteral("sourceSize"), &parsed.sourceSize, errorKey, error) ||
      !readSize(object, QStringLiteral("outputSize"), &parsed.outputSize, errorKey, error)) {
    return false;
  }

  // Each present rate is moved in only once accepted, so a malformed one is
  // refused outright and an absent one is never defaulted.
  const struct {
    const char* member;
    std::optional<Rational>* out;
  } rates[] = {{"sourceFps", &parsed.sourceFps}, {"outputFps", &parsed.outputFps}};
  for (const auto& rate : rates) {
    const QString member = QString::fromLatin1(rate.member);
    const QJsonValue value = object.value(member);
    if (value.isUndefined() || value.isNull()) continue;
    Rational parsedRate;
    if (!readRate(object, member, &parsedRate, errorKey, error)) return false;
    *rate.out = parsedRate;
  }

  *plan = parsed;
  return true;
}

bool readCounters(const QJsonValue& value, NativeCadenceCounters* counters, QString* errorKey,
                  QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  if (!value.isObject()) {
    return reject(QStringLiteral("native.status.counters.not.an.object"),
                  QStringLiteral("the counters member is not an object"));
  }
  const QJsonObject object = value.toObject();
  if (!noUnknownKeys(object, counterKeys())) {
    return reject(QStringLiteral("native.status.counters.unknown.member"),
                  QStringLiteral("the counters carry a key this schema does not define"));
  }
  NativeCadenceCounters parsed;
  const struct {
    const char* member;
    qint64* target;
  } fields[] = {{"framesIn", &parsed.framesIn},   {"framesOut", &parsed.framesOut},
                {"dropped", &parsed.dropped},     {"interpolated", &parsed.interpolated},
                {"sceneCuts", &parsed.sceneCuts}};
  for (const auto& field : fields) {
    const QString member = QString::fromLatin1(field.member);
    if (!jsonInt64(object, member, field.target)) {
      return reject(QStringLiteral("native.status.counters.malformed"),
                    QStringLiteral("the counters have no integer %1").arg(member));
    }
    if (*field.target < 0) {
      return reject(QStringLiteral("native.status.counters.malformed"),
                    QStringLiteral("the counter %1 is negative").arg(member));
    }
  }
  *counters = parsed;
  return true;
}

bool readProbeRect(const QJsonObject& object, int codedW, int codedH, QRect* out,
                   QString* errorKey, QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  if (!noUnknownKeys(object, probeRectKeys())) {
    return reject(QStringLiteral("native.status.probe.unknown.member"),
                  QStringLiteral("the probe crop carries a key this schema does not define"));
  }
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  if (!jsonInt(object, QStringLiteral("x"), &x) || !jsonInt(object, QStringLiteral("y"), &y) ||
      !jsonInt(object, QStringLiteral("w"), &w) || !jsonInt(object, QStringLiteral("h"), &h)) {
    return reject(QStringLiteral("native.status.probe.malformed"),
                  QStringLiteral("the probe crop needs integer x, y, w and h"));
  }
  if (x < 0 || y < 0 || w <= 0 || h <= 0 || w > codedW || h > codedH || x > codedW - w ||
      y > codedH - h) {
    return reject(QStringLiteral("native.status.probe.geometry"),
                  QStringLiteral("the probe crop is empty or reaches outside the coded frame"));
  }
  *out = QRect(x, y, w, h);
  return true;
}

bool readProbeRate(const QJsonObject& object, Rational* out, QString* errorKey, QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  if (!noUnknownKeys(object, rateKeys())) {
    return reject(QStringLiteral("native.status.probe.unknown.member"),
                  QStringLiteral("the probe fps carries a key this schema does not define"));
  }
  Rational parsed;
  if (!jsonInt(object, QStringLiteral("num"), &parsed.num) ||
      !jsonInt(object, QStringLiteral("den"), &parsed.den)) {
    return reject(QStringLiteral("native.status.probe.malformed"),
                  QStringLiteral("the probe fps needs integer num and den"));
  }
  if (!rateOk(parsed)) {
    return reject(QStringLiteral("native.status.probe.cadence"),
                  QStringLiteral("the probe fps is not a usable rate"));
  }
  *out = parsed;
  return true;
}

bool readProbeFpsValue(const QJsonValue& value, Rational* out, QString* errorKey,
                      QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  if (!value.isObject()) {
    return reject(QStringLiteral("native.status.probe.malformed"),
                  QStringLiteral("the probe fps is neither null nor an object"));
  }
  Rational rate;
  if (!readProbeRate(value.toObject(), &rate, errorKey, error)) return false;
  *out = rate;
  return true;
}

bool readProbeSource(const QJsonValue& value, SourceGeometry* out, QString* errorKey,
                     QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  if (!value.isObject()) {
    return reject(QStringLiteral("native.status.probe.malformed"),
                  QStringLiteral("the probe source is not an object"));
  }
  const QJsonObject object = value.toObject();
  if (!noUnknownKeys(object, probeSourceKeys())) {
    return reject(QStringLiteral("native.status.probe.unknown.member"),
                  QStringLiteral("the probe source carries a key this schema does not define"));
  }
  SourceGeometry parsed;
  int codedW = 0;
  int codedH = 0;
  if (!jsonInt(object, QStringLiteral("codedW"), &codedW) ||
      !jsonInt(object, QStringLiteral("codedH"), &codedH)) {
    return reject(QStringLiteral("native.status.probe.malformed"),
                  QStringLiteral("the probe source needs integer codedW and codedH"));
  }
  if (codedW <= 0 || codedH <= 0 || codedW > kMaxDimension || codedH > kMaxDimension) {
    return reject(QStringLiteral("native.status.probe.geometry"),
                  QStringLiteral("the probe coded size is not a usable frame size"));
  }
  parsed.codedSize = QSize(codedW, codedH);
  const QJsonValue crop = object.value(QStringLiteral("crop"));
  if (!crop.isUndefined() && !crop.isNull()) {
    if (!crop.isObject()) {
      return reject(QStringLiteral("native.status.probe.malformed"),
                    QStringLiteral("the probe crop is neither null nor an object"));
    }
    QRect rect;
    if (!readProbeRect(crop.toObject(), codedW, codedH, &rect, errorKey, error)) return false;
    parsed.crop = rect;
  }
  const QJsonValue rotation = object.value(QStringLiteral("rotation"));
  if (!rotation.isUndefined() && !rotation.isNull()) {
    int degrees = 0;
    if (!jsonInt(object, QStringLiteral("rotation"), &degrees)) {
      return reject(QStringLiteral("native.status.probe.malformed"),
                    QStringLiteral("the probe rotation is not an integer"));
    }
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
      return reject(QStringLiteral("native.status.probe.geometry"),
                    QStringLiteral("the probe rotation is not one this build models"));
    }
    parsed.rotationDegrees = degrees;
  }
  const QJsonValue fps = object.value(QStringLiteral("fps"));
  if (!fps.isUndefined() && !fps.isNull()) {
    Rational rate;
    if (!readProbeFpsValue(fps, &rate, errorKey, error)) return false;
    parsed.fps = rate;
  }
  const QJsonValue csp = object.value(QStringLiteral("csp"));
  if (!csp.isUndefined() && !csp.isNull()) {
    if (!csp.isString() || csp.toString().isEmpty() || !probeIdOk(csp.toString())) {
      return reject(QStringLiteral("native.status.probe.malformed"),
                    QStringLiteral("the probe csp is not an observed tag"));
    }
    parsed.csp = csp.toString();
  }
  const QJsonValue matrix = object.value(QStringLiteral("matrix"));
  if (!matrix.isUndefined() && !matrix.isNull()) {
    if (!matrix.isString() || matrix.toString().isEmpty() || !probeIdOk(matrix.toString())) {
      return reject(QStringLiteral("native.status.probe.malformed"),
                    QStringLiteral("the probe matrix is not an observed tag"));
    }
    parsed.matrix = matrix.toString();
  }
  const QJsonValue range = object.value(QStringLiteral("range"));
  if (!range.isUndefined() && !range.isNull()) {
    if (!range.isString() || range.toString().isEmpty() || !probeIdOk(range.toString())) {
      return reject(QStringLiteral("native.status.probe.malformed"),
                    QStringLiteral("the probe range is not an observed tag"));
    }
    parsed.range = range.toString();
  }
  const QJsonValue siting = object.value(QStringLiteral("siting"));
  if (!siting.isUndefined() && !siting.isNull()) {
    if (!siting.isString() || siting.toString().isEmpty() || !probeIdOk(siting.toString())) {
      return reject(QStringLiteral("native.status.probe.malformed"),
                    QStringLiteral("the probe siting is not an observed tag"));
    }
    parsed.siting = siting.toString();
  }
  const QJsonValue bits = object.value(QStringLiteral("bitDepth"));
  if (!bits.isUndefined() && !bits.isNull()) {
    int depth = 0;
    if (!jsonInt(object, QStringLiteral("bitDepth"), &depth)) {
      return reject(QStringLiteral("native.status.probe.malformed"),
                    QStringLiteral("the probe bitDepth is not an integer"));
    }
    if (depth != 8 && depth != 10 && depth != 12 && depth != 16) {
      return reject(QStringLiteral("native.status.probe.geometry"),
                    QStringLiteral("the probe bitDepth is not a depth this build binds"));
    }
    parsed.bitDepth = depth;
  }
  *out = parsed;
  return true;
}

bool readProbeDevice(const QJsonValue& value, DeviceIdentity* out, QString* errorKey,
                     QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  if (!value.isObject()) {
    return reject(QStringLiteral("native.status.probe.malformed"),
                  QStringLiteral("the probe device is not an object"));
  }
  const QJsonObject object = value.toObject();
  if (!noUnknownKeys(object, probeDeviceKeys())) {
    return reject(QStringLiteral("native.status.probe.unknown.member"),
                  QStringLiteral("the probe device carries a key this schema does not define"));
  }
  DeviceIdentity parsed;
  bool haveFact = false;
  const QJsonValue decoder = object.value(QStringLiteral("decoderDeviceUuid"));
  if (!decoder.isUndefined() && !decoder.isNull()) {
    if (!decoder.isString() || !probeIdOk(decoder.toString())) {
      return reject(QStringLiteral("native.status.probe.identity"),
                    QStringLiteral("the probe decoderDeviceUuid is not an observed device"));
    }
    parsed.decoderDeviceUuid = decoder.toString();
    haveFact = true;
  }
  const QJsonValue compute = object.value(QStringLiteral("computeDeviceUuid"));
  if (!compute.isUndefined() && !compute.isNull()) {
    if (!compute.isString() || !probeIdOk(compute.toString())) {
      return reject(QStringLiteral("native.status.probe.identity"),
                    QStringLiteral("the probe computeDeviceUuid is not an observed device"));
    }
    parsed.computeDeviceUuid = compute.toString();
    haveFact = true;
  }
  const QJsonValue luid = object.value(QStringLiteral("adapterLuid"));
  if (!luid.isUndefined() && !luid.isNull()) {
    if (!luid.isString() || !probeIdOk(luid.toString())) {
      return reject(QStringLiteral("native.status.probe.identity"),
                    QStringLiteral("the probe adapterLuid is not an observed device"));
    }
    parsed.adapterLuid = luid.toString();
    haveFact = true;
  }
  const QJsonValue major = object.value(QStringLiteral("smMajor"));
  const QJsonValue minor = object.value(QStringLiteral("smMinor"));
  if (!major.isUndefined() && !major.isNull()) {
    int sm = 0;
    if (!jsonInt(object, QStringLiteral("smMajor"), &sm)) {
      return reject(QStringLiteral("native.status.probe.malformed"),
                    QStringLiteral("the probe smMajor is not an integer"));
    }
    if (sm < 1 || sm > 32) {
      return reject(QStringLiteral("native.status.probe.identity"),
                    QStringLiteral("the probe smMajor is not a measured capability"));
    }
    parsed.smMajor = sm;
    haveFact = true;
  }
  if (!minor.isUndefined() && !minor.isNull()) {
    int sm = 0;
    if (!jsonInt(object, QStringLiteral("smMinor"), &sm)) {
      return reject(QStringLiteral("native.status.probe.malformed"),
                    QStringLiteral("the probe smMinor is not an integer"));
    }
    if (!parsed.smMajor) {
      return reject(QStringLiteral("native.status.probe.identity"),
                    QStringLiteral("the probe smMinor names no measured smMajor"));
    }
    if (sm < 0 || sm > 99) {
      return reject(QStringLiteral("native.status.probe.identity"),
                    QStringLiteral("the probe smMinor is not a measured capability"));
    }
    parsed.smMinor = sm;
    haveFact = true;
  }
  if (!haveFact) {
    return reject(QStringLiteral("native.status.probe.malformed"),
                  QStringLiteral("the probe device names no observed fact"));
  }
  *out = parsed;
  return true;
}

bool readProbe(const QJsonValue& value, NativeProbePayload* out, QString* errorKey,
               QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  if (!value.isObject()) {
    return reject(QStringLiteral("native.status.probe.malformed"),
                  QStringLiteral("the probe member is not an object"));
  }
  const QJsonObject object = value.toObject();
  if (!noUnknownKeys(object, probeKeys())) {
    return reject(QStringLiteral("native.status.probe.unknown.member"),
                  QStringLiteral("the probe carries a key this schema does not define"));
  }
  NativeProbePayload parsed;
  const QJsonValue source = object.value(QStringLiteral("source"));
  if (!source.isUndefined() && !source.isNull()) {
    SourceGeometry geometry;
    if (!readProbeSource(source, &geometry, errorKey, error)) return false;
    parsed.source = geometry;
  }
  const QJsonValue device = object.value(QStringLiteral("device"));
  if (!device.isUndefined() && !device.isNull()) {
    DeviceIdentity identity;
    if (!readProbeDevice(device, &identity, errorKey, error)) return false;
    parsed.device = identity;
  }
  if (!parsed.source.has_value() && !parsed.device.has_value()) {
    return reject(QStringLiteral("native.status.probe.malformed"),
                  QStringLiteral("the probe names no observed fact"));
  }
  *out = parsed;
  return true;
}

QStringList factoryKeys() {
  return {QStringLiteral("queryProtocol"),  QStringLiteral("nativeProtocol"),
          QStringLiteral("nativeSchema"),   QStringLiteral("sourceAbi"),
          QStringLiteral("observed"),       QStringLiteral("backend"),
          QStringLiteral("spatial"),        QStringLiteral("rife"),
          QStringLiteral("runtimeVersion"), QStringLiteral("deviceIdentity"),
          QStringLiteral("smMajor"),        QStringLiteral("smMinor"),
          QStringLiteral("reasonKey"),      QStringLiteral("reason")};
}

/// A JSON boolean, never a 0/1 number read as one: whether the query ran and
/// whether a role is supported must be an explicit JSON type, not an integer
/// that a hostile document could reuse for a different member.
bool jsonBool(const QJsonObject& object, const QString& key, bool* out) {
  const QJsonValue value = object.value(key);
  if (!value.isBool()) return false;
  *out = value.toBool();
  return true;
}

/// The canonical 32 lower-case hex form of a device UUID, or an empty string
/// when `value` is not one. Accepts the producer ABI's 32-hex form and the
/// equivalent dashed 8-4-4-4-12 form, so two spellings of one device compare
/// equal; it is the only conversion that relates a factory to a probe.
bool readFactory(const QJsonValue& value, NativeFactoryObservation* out, QString* errorKey,
                 QString* error) {
  const auto reject = [errorKey, error](const QString& key, const QString& text) {
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  if (!value.isObject()) {
    return reject(QStringLiteral("native.status.factory.malformed"),
                  QStringLiteral("the factory member is not an object"));
  }
  const QJsonObject object = value.toObject();
  if (!noUnknownKeys(object, factoryKeys())) {
    return reject(QStringLiteral("native.status.factory.unknown.member"),
                  QStringLiteral("the factory carries a key this schema does not define"));
  }

  // Each identity is stated separately and must name this build's own value; the
  // status wire, the producer's snapshot protocol/schema and the AJI ABI are
  // never conflated into one version check.
  int queryProtocol = 0;
  int nativeProtocol = 0;
  int nativeSchema = 0;
  int sourceAbi = 0;
  if (!jsonInt(object, QStringLiteral("queryProtocol"), &queryProtocol) ||
      queryProtocol != kNativeFactoryQueryProtocol) {
    return reject(QStringLiteral("native.status.factory.protocol"),
                  QStringLiteral("the factory query protocol is not %1")
                      .arg(kNativeFactoryQueryProtocol));
  }
  if (!jsonInt(object, QStringLiteral("nativeProtocol"), &nativeProtocol) ||
      nativeProtocol != kNativeFactorySnapshotProtocol) {
    return reject(QStringLiteral("native.status.factory.protocol"),
                  QStringLiteral("the factory native protocol is not %1")
                      .arg(kNativeFactorySnapshotProtocol));
  }
  if (!jsonInt(object, QStringLiteral("nativeSchema"), &nativeSchema) ||
      nativeSchema != kNativeFactorySnapshotSchema) {
    return reject(QStringLiteral("native.status.factory.protocol"),
                  QStringLiteral("the factory native schema is not %1")
                      .arg(kNativeFactorySnapshotSchema));
  }
  if (!jsonInt(object, QStringLiteral("sourceAbi"), &sourceAbi) ||
      sourceAbi != kNativeFactorySourceAbi) {
    return reject(QStringLiteral("native.status.factory.protocol"),
                  QStringLiteral("the factory source ABI is not %1").arg(kNativeFactorySourceAbi));
  }

  NativeFactoryObservation parsed;
  if (!jsonBool(object, QStringLiteral("observed"), &parsed.observed) ||
      !jsonBool(object, QStringLiteral("spatial"), &parsed.spatial) ||
      !jsonBool(object, QStringLiteral("rife"), &parsed.rife)) {
    return reject(QStringLiteral("native.status.factory.malformed"),
                  QStringLiteral("the factory observed/spatial/rife members are not booleans"));
  }

  const QJsonValue backend = object.value(QStringLiteral("backend"));
  if (!backend.isString()) {
    return reject(QStringLiteral("native.status.factory.malformed"),
                  QStringLiteral("the factory backend is not a string"));
  }
  parsed.backend = backend.toString();
  if (!parsed.backend.isEmpty() && !factoryBackendNameOk(parsed.backend)) {
    return reject(QStringLiteral("native.status.factory.backend"),
                  QStringLiteral("the factory backend is not one this build checks"));
  }

  const QJsonValue reasonKey = object.value(QStringLiteral("reasonKey"));
  if (!reasonKey.isString()) {
    return reject(QStringLiteral("native.status.factory.malformed"),
                  QStringLiteral("the factory reasonKey is not a string"));
  }
  parsed.reasonKey = reasonKey.toString();
  if (!parsed.reasonKey.isEmpty() && !factoryReasonKeyOk(parsed.reasonKey)) {
    return reject(QStringLiteral("native.status.factory.reason"),
                  QStringLiteral("the factory reasonKey is not a stable key"));
  }
  const QJsonValue reason = object.value(QStringLiteral("reason"));
  if (!reason.isString()) {
    return reject(QStringLiteral("native.status.factory.malformed"),
                  QStringLiteral("the factory reason is not a string"));
  }
  parsed.reason = reason.toString();

  // An unobserved or unavailable factory must never claim a supported role, and
  // an unavailable one must name why. A usable one must name its backend and
  // carry no refusal. This is the one place a capability flag can be turned on,
  // and it can only be turned on by an observed, successful answer.
  if (!parsed.observed && (parsed.spatial || parsed.rife)) {
    return reject(QStringLiteral("native.status.factory.inconsistent"),
                  QStringLiteral("an unobserved factory claims a supported role"));
  }
  if (parsed.isUsable()) {
    if (!factoryBackendOk(parsed.backend)) {
      return reject(QStringLiteral("native.status.factory.backend"),
                    QStringLiteral("a usable factory names no checked backend"));
    }
    if (!parsed.reasonKey.isEmpty()) {
      return reject(QStringLiteral("native.status.factory.inconsistent"),
                    QStringLiteral("a usable factory carries a refusal"));
    }
  } else {
    if (parsed.spatial || parsed.rife) {
      return reject(QStringLiteral("native.status.factory.inconsistent"),
                    QStringLiteral("an unavailable factory claims a supported role"));
    }
    if (parsed.reasonKey.isEmpty()) {
      return reject(QStringLiteral("native.status.factory.reason"),
                    QStringLiteral("an unavailable factory names no reason"));
    }
  }

  const QJsonValue runtime = object.value(QStringLiteral("runtimeVersion"));
  if (!runtime.isUndefined() && !runtime.isNull()) {
    if (!runtime.isString() || !factoryRuntimeOk(runtime.toString())) {
      return reject(QStringLiteral("native.status.factory.malformed"),
                    QStringLiteral("the factory runtimeVersion is not an observed token"));
    }
    parsed.runtimeVersion = runtime.toString();
  }
  const QJsonValue device = object.value(QStringLiteral("deviceIdentity"));
  if (!device.isUndefined() && !device.isNull()) {
    if (!device.isString() || !isActualDeviceUuid(device.toString())) {
      return reject(QStringLiteral("native.status.factory.identity"),
                    QStringLiteral("the factory deviceIdentity is not an observed device"));
    }
    parsed.deviceIdentity = device.toString();
  }
  const QJsonValue major = object.value(QStringLiteral("smMajor"));
  const QJsonValue minor = object.value(QStringLiteral("smMinor"));
  if (!major.isUndefined() && !major.isNull()) {
    int sm = 0;
    if (!jsonInt(object, QStringLiteral("smMajor"), &sm)) {
      return reject(QStringLiteral("native.status.factory.malformed"),
                    QStringLiteral("the factory smMajor is not an integer"));
    }
    if (sm < 1 || sm > 32) {
      return reject(QStringLiteral("native.status.factory.identity"),
                    QStringLiteral("the factory smMajor is not a measured capability"));
    }
    parsed.smMajor = sm;
  }
  if (!minor.isUndefined() && !minor.isNull()) {
    int sm = 0;
    if (!jsonInt(object, QStringLiteral("smMinor"), &sm)) {
      return reject(QStringLiteral("native.status.factory.malformed"),
                    QStringLiteral("the factory smMinor is not an integer"));
    }
    if (!parsed.smMajor) {
      return reject(QStringLiteral("native.status.factory.identity"),
                    QStringLiteral("the factory smMinor names no measured smMajor"));
    }
    if (sm < 0 || sm > 99) {
      return reject(QStringLiteral("native.status.factory.identity"),
                    QStringLiteral("the factory smMinor is not a measured capability"));
    }
    parsed.smMinor = sm;
  }

  // A usable factory proved itself against a real device and a real runtime, so
  // it must name both; a claim of support with neither is refused.
  if (parsed.isUsable()) {
    if (parsed.runtimeVersion.isEmpty()) {
      return reject(QStringLiteral("native.status.factory.identity"),
                    QStringLiteral("a usable factory names no runtime it checked"));
    }
    if (parsed.deviceIdentity.isEmpty() || parsed.smMajor == 0) {
      return reject(QStringLiteral("native.status.factory.identity"),
                    QStringLiteral("a usable factory names no measured device"));
    }
  }
  *out = parsed;
  return true;
}

}  // namespace


bool parseStatusSnapshot(const QJsonObject& object, StatusSnapshot* snapshot, QString* errorKey,
                         QString* error) {
  if (snapshot == nullptr) return false;
  const auto reject = [snapshot, errorKey, error](const QString& key, const QString& text) {
    *snapshot = StatusSnapshot{};
    if (errorKey) *errorKey = key;
    // Redacted before leaving the parser: a refusal must not repeat what it refused.
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };

  // Refused first, by name and without an echo, before any later check repeats it.
  if (jsonCarriesSensitiveText(object)) {
    return reject(QStringLiteral("native.status.sensitiveData"), sensitiveStatusReason());
  }

  if (!noUnknownKeys(object, documentKeys())) {
    return reject(QStringLiteral("native.status.unknown.member"),
                  QStringLiteral("the snapshot carries a key this schema does not define"));
  }

  const QString schema = jsonString(object, QStringLiteral("schema"));
  if (schema != nativeStatusSchema()) {
    return reject(QStringLiteral("native.status.schema.unknown"),
                  QStringLiteral("snapshot schema %1 is not %2").arg(schema, nativeStatusSchema()));
  }

  int protocol = 0;
  if (!jsonInt(object, QStringLiteral("protocol"), &protocol) ||
      protocol != kNativeStatusProtocol) {
    return reject(QStringLiteral("native.status.protocol.unknown"),
                  QStringLiteral("snapshot protocol %1 is not %2")
                      .arg(jsonString(object, QStringLiteral("protocol")))
                      .arg(kNativeStatusProtocol));
  }

  qint64 revision = 0;
  if (!jsonInt64(object, QStringLiteral("revision"), &revision) || revision < 0) {
    return reject(QStringLiteral("native.status.revision.invalid"),
                  QStringLiteral("the snapshot has no non-negative integer revision"));
  }

  const std::optional<NativeState> state =
      nativeStateFromName(jsonString(object, QStringLiteral("state")));
  if (!state.has_value()) {
    return reject(QStringLiteral("native.status.state.unknown"),
                  QStringLiteral("state %1 is not a state this build speaks")
                      .arg(jsonString(object, QStringLiteral("state"))));
  }

  StatusSnapshot parsed;
  parsed.version = protocol;
  parsed.revision = revision;
  parsed.state = *state;

  const QJsonValue epoch = object.value(QStringLiteral("epoch"));
  if (!epoch.isUndefined() && !epoch.isNull()) {
    qint64 value = 0;
    if (!jsonInt64(object, QStringLiteral("epoch"), &value) || value < 0) {
      return reject(QStringLiteral("native.status.epoch.invalid"),
                    QStringLiteral("the snapshot epoch is not a non-negative integer"));
    }
    parsed.epoch = value;
  }

  const QJsonValue requested = object.value(QStringLiteral("requested"));
  if (!requested.isUndefined() && !requested.isNull()) {
    if (!requested.isObject()) {
      return reject(QStringLiteral("native.status.requested.not.an.object"),
                    QStringLiteral("the requested member is not an object"));
    }
    const QJsonObject plan = requested.toObject();
    if (!noUnknownKeys(plan, requestedKeys())) {
      return reject(QStringLiteral("native.status.requested.unknown.member"),
                    QStringLiteral("the requested plan carries a key this schema does not define"));
    }
    SelectionState selection;
    selection.recipeId = jsonString(plan, QStringLiteral("recipe"));
    if (!selection.recipeId.isEmpty() && !isValidModelId(selection.recipeId)) {
      return reject(QStringLiteral("native.status.requested.recipe.invalid"),
                    QStringLiteral("requested recipe id %1 is not a usable identifier")
                        .arg(selection.recipeId));
    }
    const std::optional<Backend> backend =
        backendFromName(jsonString(plan, QStringLiteral("backend")));
    if (!backend.has_value()) {
      return reject(QStringLiteral("native.status.requested.backend.unknown"),
                    QStringLiteral("requested backend %1 is neither tensorrt nor directml")
                        .arg(jsonString(plan, QStringLiteral("backend"))));
    }
    selection.backend = *backend;
    parsed.requested = selection;
  }

  const QJsonValue effective = object.value(QStringLiteral("effective"));
  if (!effective.isUndefined() && !effective.isNull()) {
    NativeEffectivePlan plan;
    if (!readEffective(effective, &plan, errorKey, error)) {
      *snapshot = StatusSnapshot{};
      return false;
    }
    parsed.effective = plan;
  }

  const QJsonValue counters = object.value(QStringLiteral("counters"));
  if (!counters.isUndefined() && !counters.isNull()) {
    NativeCadenceCounters counted;
    if (!readCounters(counters, &counted, errorKey, error)) {
      *snapshot = StatusSnapshot{};
      return false;
    }
    parsed.counters = counted;
  }

  const QJsonValue probe = object.value(QStringLiteral("probe"));
  if (!probe.isUndefined() && !probe.isNull()) {
    NativeProbePayload payload;
    if (!readProbe(probe, &payload, errorKey, error)) {
      *snapshot = StatusSnapshot{};
      return false;
    }
    parsed.probe = payload;
  }

  const QJsonValue factory = object.value(QStringLiteral("factory"));
  if (!factory.isUndefined() && !factory.isNull()) {
    NativeFactoryObservation observation;
    if (!readFactory(factory, &observation, errorKey, error)) {
      *snapshot = StatusSnapshot{};
      return false;
    }
    parsed.factory = observation;
  }

  const QJsonValue reason = object.value(QStringLiteral("reason"));
  if (!reason.isString()) {
    return reject(QStringLiteral("native.status.reason.malformed"),
                  QStringLiteral("the snapshot reason is not a string"));
  }
  parsed.reason = reason.toString();

  QString key;
  QString text;
  if (!statusSnapshotIsConsistent(parsed, &key, &text)) {
    return reject(key, text);
  }

  *snapshot = parsed;
  return true;
}

bool parseStatusSnapshot(const QByteArray& payload, StatusSnapshot* snapshot, QString* errorKey,
                         QString* error) {
  if (snapshot == nullptr) return false;
  const auto reject = [snapshot, errorKey, error](const QString& key, const QString& text) {
    *snapshot = StatusSnapshot{};
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };
  QJsonParseError parse{};
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject()) {
    return reject(QStringLiteral("native.status.snapshot.malformed"),
                  QStringLiteral("the snapshot tag is not one JSON object: %1")
                      .arg(parse.error == QJsonParseError::NoError
                               ? QStringLiteral("not an object")
                               : parse.errorString()));
  }
  return parseStatusSnapshot(document.object(), snapshot, errorKey, error);
}

bool parseStatusTags(const NodeValue& tags, StatusSnapshot* snapshot, QString* errorKey,
                     QString* error) {
  if (snapshot == nullptr) return false;
  const auto reject = [snapshot, errorKey, error](const QString& key, const QString& text) {
    *snapshot = StatusSnapshot{};
    if (errorKey) *errorKey = key;
    if (error) *error = redactNativeDiagnosticText(text);
    return false;
  };

  // An absent property is a named state, not an empty success.
  if (tags.format == MPV_FORMAT_NONE) {
    return reject(QStringLiteral("native.status.missing"),
                  QStringLiteral("%1 answered no value").arg(nativeStatusProperty()));
  }
  if (tags.format != MPV_FORMAT_NODE_MAP) {
    return reject(QStringLiteral("native.status.tags.shape"),
                  QStringLiteral("%1 did not answer a flat string map (format %2)")
                      .arg(nativeStatusProperty())
                      .arg(static_cast<int>(tags.format)));
  }

  QString schema;
  QString protocol;
  QString revision;
  QString document;
  bool haveSchema = false;
  bool haveProtocol = false;
  bool haveRevision = false;
  bool haveSnapshot = false;
  for (const std::pair<std::string, NodeValue>& tag : tags.map) {
    if (tag.second.format != MPV_FORMAT_STRING) {
      return reject(QStringLiteral("native.status.tags.type"),
                    QStringLiteral("tag %1 is not a string")
                        .arg(QString::fromStdString(tag.first)));
    }
    const QString key = QString::fromStdString(tag.first);
    const QString text = QString::fromStdString(tag.second.text);
    if (key == QStringLiteral("schema")) {
      schema = text;
      haveSchema = true;
    } else if (key == QStringLiteral("protocol")) {
      protocol = text;
      haveProtocol = true;
    } else if (key == QStringLiteral("revision")) {
      revision = text;
      haveRevision = true;
    } else if (key == QStringLiteral("snapshot")) {
      document = text;
      haveSnapshot = true;
    } else {
      return reject(QStringLiteral("native.status.tags.unknown"),
                    QStringLiteral("tag %1 is not one this schema defines").arg(key));
    }
  }
  if (!haveSchema || !haveProtocol || !haveRevision || !haveSnapshot) {
    return reject(QStringLiteral("native.status.tags.incomplete"),
                  QStringLiteral("%1 is missing schema, protocol, revision or snapshot")
                      .arg(nativeStatusProperty()));
  }

  if (schema != nativeStatusSchema()) {
    return reject(QStringLiteral("native.status.schema.unknown"),
                  QStringLiteral("status schema %1 is not %2").arg(schema, nativeStatusSchema()));
  }
  if (!isDecimalText(protocol) || protocol.toInt() != kNativeStatusProtocol) {
    return reject(QStringLiteral("native.status.protocol.unknown"),
                  QStringLiteral("status protocol %1 is not %2")
                      .arg(protocol)
                      .arg(kNativeStatusProtocol));
  }
  bool revisionOk = false;
  const qint64 declaredRevision = revision.toLongLong(&revisionOk);
  if (!isDecimalText(revision) || !revisionOk) {
    return reject(QStringLiteral("native.status.revision.invalid"),
                  QStringLiteral("status revision %1 is not a non-negative integer").arg(revision));
  }
  if (document.isEmpty()) {
    return reject(QStringLiteral("native.status.snapshot.missing"),
                  QStringLiteral("the snapshot tag is empty"));
  }

  StatusSnapshot parsed;
  QString innerKey;
  QString innerText;
  if (!parseStatusSnapshot(document.toUtf8(), &parsed, &innerKey, &innerText)) {
    return reject(innerKey, innerText);
  }
  // Both layers must state the same identity, not just the same schema.
  if (parsed.version != kNativeStatusProtocol) {
    return reject(QStringLiteral("native.status.snapshot.mismatch"),
                  QStringLiteral("the snapshot declares protocol %1, the tags declare %2")
                      .arg(parsed.version)
                      .arg(kNativeStatusProtocol));
  }
  if (parsed.revision != declaredRevision) {
    return reject(QStringLiteral("native.status.snapshot.mismatch"),
                  QStringLiteral("the snapshot declares revision %1, the tags declare %2")
                      .arg(parsed.revision)
                      .arg(declaredRevision));
  }

  *snapshot = parsed;
  return true;
}

QJsonObject statusSnapshotToJson(const StatusSnapshot& snapshot) {
  QJsonObject document;
  document.insert(QStringLiteral("schema"), nativeStatusSchema());
  document.insert(QStringLiteral("protocol"), snapshot.version);
  // The inner document carries real JSON numbers; only the outer tag map is
  // string-typed. The number is written as a 64-bit integer, never through a
  // double, so a revision past 2^53 is not rounded into another on the way out.
  document.insert(QStringLiteral("revision"), QJsonValue(snapshot.revision));
  document.insert(QStringLiteral("state"), stableName(snapshot.state));
  if (snapshot.epoch.has_value()) {
    document.insert(QStringLiteral("epoch"), QJsonValue(*snapshot.epoch));
  }
  if (snapshot.requested.has_value()) {
    QJsonObject requested;
    requested.insert(QStringLiteral("recipe"), snapshot.requested->recipeId);
    requested.insert(QStringLiteral("backend"), stableName(snapshot.requested->backend));
    document.insert(QStringLiteral("requested"), requested);
  }
  if (snapshot.effective.has_value()) {
    const NativeEffectivePlan& plan = *snapshot.effective;
    QJsonObject effective;
    effective.insert(QStringLiteral("recipe"), plan.recipe);
    effective.insert(QStringLiteral("provider"), plan.provider);
    effective.insert(QStringLiteral("model"), plan.model);
    effective.insert(QStringLiteral("spatialStages"), plan.spatialStages);
    if (plan.interpolation.has_value()) {
      QJsonObject interpolation;
      interpolation.insert(QStringLiteral("num"), plan.interpolation->factor.num);
      interpolation.insert(QStringLiteral("den"), plan.interpolation->factor.den);
      interpolation.insert(QStringLiteral("model"), plan.interpolation->model);
      effective.insert(QStringLiteral("interpolation"), interpolation);
    } else {
      // A spatial-only chain states the member as null rather than omitting it.
      effective.insert(QStringLiteral("interpolation"), QJsonValue(QJsonValue::Null));
    }
    effective.insert(QStringLiteral("order"), plan.order);
    QJsonObject sourceSize;
    sourceSize.insert(QStringLiteral("w"), plan.sourceSize.w);
    sourceSize.insert(QStringLiteral("h"), plan.sourceSize.h);
    effective.insert(QStringLiteral("sourceSize"), sourceSize);
    QJsonObject outputSize;
    outputSize.insert(QStringLiteral("w"), plan.outputSize.w);
    outputSize.insert(QStringLiteral("h"), plan.outputSize.h);
    effective.insert(QStringLiteral("outputSize"), outputSize);
    // An absent rate is omitted, never written as a zero or 1/1 the filter never published.
    if (plan.sourceFps.has_value()) {
      QJsonObject sourceFps;
      sourceFps.insert(QStringLiteral("num"), plan.sourceFps->num);
      sourceFps.insert(QStringLiteral("den"), plan.sourceFps->den);
      effective.insert(QStringLiteral("sourceFps"), sourceFps);
    }
    if (plan.outputFps.has_value()) {
      QJsonObject outputFps;
      outputFps.insert(QStringLiteral("num"), plan.outputFps->num);
      outputFps.insert(QStringLiteral("den"), plan.outputFps->den);
      effective.insert(QStringLiteral("outputFps"), outputFps);
    }
    effective.insert(QStringLiteral("decoder"), plan.decoder);
    effective.insert(QStringLiteral("csp"), plan.csp);
    document.insert(QStringLiteral("effective"), effective);
  }
  if (snapshot.counters.has_value()) {
    const NativeCadenceCounters& counters = *snapshot.counters;
    QJsonObject counted;
    counted.insert(QStringLiteral("framesIn"), QJsonValue(counters.framesIn));
    counted.insert(QStringLiteral("framesOut"), QJsonValue(counters.framesOut));
    counted.insert(QStringLiteral("dropped"), QJsonValue(counters.dropped));
    counted.insert(QStringLiteral("interpolated"), QJsonValue(counters.interpolated));
    counted.insert(QStringLiteral("sceneCuts"), QJsonValue(counters.sceneCuts));
    document.insert(QStringLiteral("counters"), counted);
  }
  if (snapshot.probe.has_value()) {
    const NativeProbePayload& probe = *snapshot.probe;
    QJsonObject payload;
    if (probe.source.has_value()) {
      const SourceGeometry& source = *probe.source;
      QJsonObject geometry;
      if (source.codedSize.has_value()) {
        geometry.insert(QStringLiteral("codedW"), source.codedSize->width());
        geometry.insert(QStringLiteral("codedH"), source.codedSize->height());
      }
      if (source.crop.has_value()) {
        QJsonObject crop;
        crop.insert(QStringLiteral("x"), source.crop->x());
        crop.insert(QStringLiteral("y"), source.crop->y());
        crop.insert(QStringLiteral("w"), source.crop->width());
        crop.insert(QStringLiteral("h"), source.crop->height());
        geometry.insert(QStringLiteral("crop"), crop);
      }
      if (source.rotationDegrees.has_value()) {
        geometry.insert(QStringLiteral("rotation"), *source.rotationDegrees);
      }
      if (source.fps.has_value()) {
        QJsonObject fps;
        fps.insert(QStringLiteral("num"), source.fps->num);
        fps.insert(QStringLiteral("den"), source.fps->den);
        geometry.insert(QStringLiteral("fps"), fps);
      }
      if (!source.csp.isEmpty()) {
        geometry.insert(QStringLiteral("csp"), source.csp);
      }
      if (!source.matrix.isEmpty()) {
        geometry.insert(QStringLiteral("matrix"), source.matrix);
      }
      if (!source.range.isEmpty()) {
        geometry.insert(QStringLiteral("range"), source.range);
      }
      if (!source.siting.isEmpty()) {
        geometry.insert(QStringLiteral("siting"), source.siting);
      }
      if (source.bitDepth != 0) {
        geometry.insert(QStringLiteral("bitDepth"), source.bitDepth);
      }
      payload.insert(QStringLiteral("source"), geometry);
    }
    if (probe.device.has_value()) {
      const DeviceIdentity& device = *probe.device;
      QJsonObject identity;
      if (!device.decoderDeviceUuid.isEmpty()) {
        identity.insert(QStringLiteral("decoderDeviceUuid"), device.decoderDeviceUuid);
      }
      if (!device.computeDeviceUuid.isEmpty()) {
        identity.insert(QStringLiteral("computeDeviceUuid"), device.computeDeviceUuid);
      }
      if (!device.adapterLuid.isEmpty()) {
        identity.insert(QStringLiteral("adapterLuid"), device.adapterLuid);
      }
      if (device.smMajor != 0) {
        identity.insert(QStringLiteral("smMajor"), device.smMajor);
        identity.insert(QStringLiteral("smMinor"), device.smMinor);
      } else if (device.smMinor != 0) {
        // A measured smMinor with no measured smMajor is written rather than
        // dropped: the reader refuses it by name instead of accepting a fact the
        // filter never proved.
        identity.insert(QStringLiteral("smMinor"), device.smMinor);
      }
      payload.insert(QStringLiteral("device"), identity);
    }
    document.insert(QStringLiteral("probe"), payload);
  }
  if (snapshot.factory.has_value()) {
    const NativeFactoryObservation& factory = *snapshot.factory;
    QJsonObject object;
    // Each identity is written separately, never folded into the status protocol.
    object.insert(QStringLiteral("queryProtocol"), kNativeFactoryQueryProtocol);
    object.insert(QStringLiteral("nativeProtocol"), kNativeFactorySnapshotProtocol);
    object.insert(QStringLiteral("nativeSchema"), kNativeFactorySnapshotSchema);
    object.insert(QStringLiteral("sourceAbi"), kNativeFactorySourceAbi);
    object.insert(QStringLiteral("observed"), factory.observed);
    object.insert(QStringLiteral("backend"), factory.backend);
    object.insert(QStringLiteral("spatial"), factory.spatial);
    object.insert(QStringLiteral("rife"), factory.rife);
    if (!factory.runtimeVersion.isEmpty()) {
      object.insert(QStringLiteral("runtimeVersion"), factory.runtimeVersion);
    }
    if (!factory.deviceIdentity.isEmpty()) {
      object.insert(QStringLiteral("deviceIdentity"), factory.deviceIdentity);
    }
    if (factory.smMajor != 0) {
      object.insert(QStringLiteral("smMajor"), factory.smMajor);
      object.insert(QStringLiteral("smMinor"), factory.smMinor);
    } else if (factory.smMinor != 0) {
      // A measured smMinor with no measured smMajor is written rather than
      // dropped, so the reader refuses it by name instead of accepting a fact the
      // filter never proved.
      object.insert(QStringLiteral("smMinor"), factory.smMinor);
    }
    object.insert(QStringLiteral("reasonKey"), factory.reasonKey);
    object.insert(QStringLiteral("reason"), factory.reason);
    document.insert(QStringLiteral("factory"), object);
  }
  // Written verbatim: this is the raw document a filter publishes, and a hostile
  // URL must reach the reader unchanged so it is refused by name rather than
  // quietly redacted into an accepted document. `serializeStatusSnapshot` is the
  // boundary that redacts before anything is exported or logged.
  document.insert(QStringLiteral("reason"), snapshot.reason);
  return document;
}

QByteArray serializeStatusSnapshot(const StatusSnapshot& snapshot) {
  return QJsonDocument(statusSnapshotToJson(redactedStatusSnapshot(snapshot)))
      .toJson(QJsonDocument::Compact);
}

NodeValue statusTags(const StatusSnapshot& snapshot) {
  NodeValue tags;
  tags.format = MPV_FORMAT_NODE_MAP;
  const auto put = [&tags](const char* key, const QString& value) {
    NodeValue entry;
    entry.format = MPV_FORMAT_STRING;
    entry.text = value.toStdString();
    tags.map.emplace_back(std::string(key), std::move(entry));
  };
  put("schema", nativeStatusSchema());
  put("protocol", QString::number(kNativeStatusProtocol));
  put("revision", QString::number(snapshot.revision));
  put("snapshot", QString::fromUtf8(serializeStatusSnapshot(snapshot)));
  return tags;
}


}  // namespace native
}  // namespace mplayer

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
bool statusCarriesSensitiveText(const StatusSnapshot& snapshot) {
  if (containsSensitiveNativeText(snapshot.reason)) return true;
  if (snapshot.requested.has_value() &&
      containsSensitiveNativeText(snapshot.requested->recipeId)) {
    return true;
  }
  if (snapshot.effective.has_value()) {
    const NativeEffectivePlan& plan = *snapshot.effective;
    const QString* const fields[] = {&plan.recipe, &plan.provider, &plan.model,
                                     &plan.order,  &plan.decoder, &plan.csp};
    for (const QString* field : fields) {
      if (containsSensitiveNativeText(*field)) return true;
    }
    if (plan.interpolation.has_value() &&
        containsSensitiveNativeText(plan.interpolation->model)) {
      return true;
    }
  }
  if (snapshot.probe.has_value()) {
    const NativeProbePayload& probe = *snapshot.probe;
    if (probe.source.has_value()) {
      const SourceGeometry& source = *probe.source;
      const QString* const fields[] = {&source.csp, &source.matrix, &source.range,
                                       &source.siting};
      for (const QString* field : fields) {
        if (containsSensitiveNativeText(*field)) return true;
      }
    }
    if (probe.device.has_value()) {
      const DeviceIdentity& device = *probe.device;
      const QString* const fields[] = {&device.decoderDeviceUuid, &device.computeDeviceUuid,
                                       &device.adapterLuid};
      for (const QString* field : fields) {
        if (containsSensitiveNativeText(*field)) return true;
      }
    }
  }
  if (snapshot.factory.has_value()) {
    const NativeFactoryObservation& factory = *snapshot.factory;
    const QString* const fields[] = {&factory.backend, &factory.runtimeVersion,
                                     &factory.deviceIdentity, &factory.reasonKey, &factory.reason};
    for (const QString* field : fields) {
      if (containsSensitiveNativeText(*field)) return true;
    }
  }
  return false;
}

/// The fixed refusal text; never names or repeats the offending value.
QString sensitiveStatusReason() {
  return QStringLiteral("the snapshot carries a URL or a credential-looking value");
}

/// A copy of `snapshot` with every free-text member passed through
/// `redactNativeDiagnosticText`. The raw document writer keeps the bytes a filter
/// published, so the reader can refuse a hostile one by name; this copy is what an
/// export or a log carries, so no credential leaves in the clear.
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

QStringList innerTagKeys() {
  return {QStringLiteral("schema"), QStringLiteral("protocol"), QStringLiteral("revision"),
          QStringLiteral("snapshot")};
}

bool rateComponentOk(int value) {
  return value > 0 && value <= kNativeStatusMaxRateComponent;
}

bool rateOk(const Rational& rate) {
  return rateComponentOk(rate.num) && rateComponentOk(rate.den);
}

/// Exact rational equality without division or reduction: 4/2 equals 2/1.
bool sameRate(const Rational& a, const Rational& b) {
  return static_cast<qint64>(a.num) * b.den == static_cast<qint64>(b.num) * a.den;
}

/// `out == source * factor` exactly; every component passed `rateComponentOk`, so
/// each product stays inside 64-bit arithmetic.
bool scaledRate(const Rational& out, const Rational& source, const Rational& factor) {
  return static_cast<qint64>(out.num) * source.den * factor.den ==
         static_cast<qint64>(source.num) * factor.num * out.den;
}

/// True when `count` does not exceed `floor(frames * numerator / den)` for
/// non-negative operands. Exact and overflow-safe: `frames` is split first
/// (`frames = quotient * den + remainder`) so the trailing term is tiny, and a
/// quotient that cannot be multiplied means the cap exceeds every counter this
/// schema carries, which accepts rather than wraps into a wrong refusal.
bool countWithinRatio(qint64 frames, qint64 count, qint64 numerator, qint64 den) {
  if (den <= 0 || frames < 0 || count < 0 || numerator < 0) return false;
  if (numerator == 0) return count == 0;
  if (count == 0) return true;
  const qint64 quotient = frames / den;
  const qint64 remainder = frames % den;
  const qint64 tail = (remainder * numerator) / den;
  if (quotient > (std::numeric_limits<qint64>::max() - tail) / numerator) {
    return true;
  }
  return count <= quotient * numerator + tail;
}

/// The same comparison against `ceil(frames * numerator / den)`.
bool countWithinCeilRatio(qint64 frames, qint64 count, qint64 numerator, qint64 den) {
  if (den <= 0 || frames < 0 || count < 0 || numerator < 0) return false;
  if (numerator == 0) return count == 0;
  if (count == 0) return true;
  const qint64 quotient = frames / den;
  const qint64 remainder = frames % den;
  const qint64 tail = (remainder * numerator + den - 1) / den;
  if (quotient > (std::numeric_limits<qint64>::max() - tail) / numerator) {
    return true;
  }
  return count <= quotient * numerator + tail;
}

QString showRate(const Rational& rate) {
  return QStringLiteral("%1/%2").arg(rate.num).arg(rate.den);
}

QString showRate(const std::optional<Rational>& rate) {
  return rate.has_value() ? showRate(*rate) : QStringLiteral("absent");
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

bool isPlaceholderProbeToken(const QString& value) {
  static const QStringList placeholders = {
      QStringLiteral("unknown"), QStringLiteral("unspecified"), QStringLiteral("none"),
      QStringLiteral("null"),    QStringLiteral("n/a"),        QStringLiteral("-"),
      QStringLiteral("?"),       QStringLiteral("0"),          QStringLiteral("undefined")};
  const QString trimmed = value.trimmed();
  if (trimmed.isEmpty()) return true;
  return placeholders.contains(trimmed, Qt::CaseInsensitive);
}

bool probeIdOk(const QString& value) {
  if (value.isEmpty() || value != value.trimmed()) return false;
  if (value.size() > 128) return false;
  for (const QChar c : value) {
    const char16_t u = c.unicode();
    if (u < 0x21 || u > 0x7e) return false;
  }
  if (isPlaceholderProbeToken(value)) return false;
  return !containsSensitiveNativeText(value);
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
QString deviceUuidHex(const QString& value) {
  QString hex = value;
  if (hex.contains(QLatin1Char('-'))) {
    static const int kDashedSizes[] = {8, 4, 4, 4, 12};
    const QStringList parts = hex.split(QLatin1Char('-'));
    if (parts.size() != 5) return QString();
    hex.clear();
    for (int i = 0; i < parts.size(); ++i) {
      if (parts.at(i).size() != kDashedSizes[i]) return QString();
      hex += parts.at(i);
    }
  }
  if (hex.size() != 32) return QString();
  for (const QChar c : hex) {
    const char16_t u = c.unicode();
    if (!((u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f'))) return QString();
  }
  return hex;
}

/// The actual device UUID, in either the 32 lower-case hex form the producer ABI
/// names or the equivalent dashed 8-4-4-4-12 form. Anything else is not an
/// observed identity and is refused rather than stored as a device.
bool isActualDeviceUuid(const QString& value) {
  return !deviceUuidHex(value).isEmpty();
}

/// The only backend tokens a factory may name as actually checked.
bool factoryBackendOk(const QString& backend) {
  return backend == QStringLiteral("tensorrt") || backend == QStringLiteral("directml");
}

/// Any backend token the wire may carry. `none` is the honest "nothing was
/// checked" answer (e.g. the DirectML path is absent); it can never back a claim
/// of support, which requires a real backend below.
bool factoryBackendNameOk(const QString& backend) {
  return backend == QStringLiteral("none") || factoryBackendOk(backend);
}

/// A runtime version token: printable, non-empty and bounded, passed through
/// verbatim otherwise. It is a version string, not an identifier, so spaces are
/// allowed; a credential-looking value is refused by the caller's scan.
bool factoryRuntimeOk(const QString& value) {
  if (value.isEmpty() || value.size() > 127) return false;
  for (const QChar c : value) {
    const char16_t u = c.unicode();
    if (u < 0x20 || u > 0x7e) return false;
  }
  return true;
}

/// A stable machine key: dot-separated segments of ASCII letters, digits,
/// underscore or hyphen. A free-text sentence (spaces, punctuation, prose) is
/// refused so a consumer can branch on the key.
bool factoryReasonKeyOk(const QString& key) {
  if (key.isEmpty() || key.size() > 96) return false;
  const QStringList parts = key.split(QLatin1Char('.'));
  for (const QString& part : parts) {
    if (part.isEmpty()) return false;
    for (const QChar c : part) {
      const char16_t u = c.unicode();
      const bool ok = (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') ||
                      (u >= u'0' && u <= u'9') || u == u'_' || u == u'-';
      if (!ok) return false;
    }
  }
  return true;
}

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

bool sourceGeometryEqual(const SourceGeometry& a, const SourceGeometry& b) {
  return a.codedSize == b.codedSize && a.crop == b.crop &&
         a.rotationDegrees == b.rotationDegrees && a.fps == b.fps && a.csp == b.csp &&
         a.matrix == b.matrix && a.range == b.range && a.siting == b.siting &&
         a.bitDepth == b.bitDepth;
}

bool deviceIdentityEqual(const DeviceIdentity& a, const DeviceIdentity& b) {
  return a.decoderDeviceUuid == b.decoderDeviceUuid &&
         a.computeDeviceUuid == b.computeDeviceUuid && a.adapterLuid == b.adapterLuid &&
         a.smMajor == b.smMajor && a.smMinor == b.smMinor;
}

bool NativeProbePayload::operator==(const NativeProbePayload& other) const {
  const bool sameSource = source.has_value() == other.source.has_value() &&
                          (!source.has_value() || sourceGeometryEqual(*source, *other.source));
  const bool sameDevice = device.has_value() == other.device.has_value() &&
                          (!device.has_value() || deviceIdentityEqual(*device, *other.device));
  return sameSource && sameDevice;
}

QString nativeStatusProperty() {
  return QString::fromLatin1(kNativeStatusProperty);
}

QString nativeStatusSchema() {
  return QString::fromLatin1(kNativeStatusSchema);
}

QString stableName(NativeState state) {
  switch (state) {
    case NativeState::Off:
      return QStringLiteral("Off");
    case NativeState::Probe:
      return QStringLiteral("Probe");
    case NativeState::Preparing:
      return QStringLiteral("Preparing");
    case NativeState::NeedsPreparation:
      return QStringLiteral("NeedsPreparation");
    case NativeState::Active:
      return QStringLiteral("Active");
    case NativeState::Suspended:
      return QStringLiteral("Suspended");
    case NativeState::Failed:
      return QStringLiteral("Failed");
    case NativeState::Unsupported:
      return QStringLiteral("Unsupported");
    case NativeState::Deactivated:
      return QStringLiteral("Deactivated");
  }
  return QString();
}

std::optional<NativeState> nativeStateFromName(const QString& name) {
  for (const NativeState state : allNativeStates()) {
    if (stableName(state) == name) return state;
  }
  return std::nullopt;
}

QVector<NativeState> allNativeStates() {
  return {NativeState::Off,       NativeState::Probe,        NativeState::Preparing,
          NativeState::NeedsPreparation, NativeState::Active, NativeState::Suspended,
          NativeState::Failed,    NativeState::Unsupported,  NativeState::Deactivated};
}

bool nativeStateNeedsPolling(NativeState state) {
  return state == NativeState::Active || state == NativeState::Preparing;
}

bool statusSnapshotIsConsistent(const StatusSnapshot& snapshot, QString* reasonKey,
                                QString* reason) {
  const auto disagree = [reasonKey, reason](const QString& key, const QString& text) {
    if (reasonKey) *reasonKey = key;
    if (reason) *reason = redactNativeDiagnosticText(text);
    return false;
  };

  // Refused first, by a fixed name, before any later rule can quote the value back.
  if (statusCarriesSensitiveText(snapshot)) {
    return disagree(QStringLiteral("native.status.sensitiveData"), sensitiveStatusReason());
  }

  if (snapshot.version != kNativeStatusProtocol) {
    return disagree(QStringLiteral("native.status.protocol.unknown"),
                    QStringLiteral("snapshot protocol %1 is not %2")
                        .arg(snapshot.version)
                        .arg(kNativeStatusProtocol));
  }
  if (snapshot.revision < 0) {
    return disagree(QStringLiteral("native.status.revision.invalid"),
                    QStringLiteral("snapshot revision %1 is negative").arg(snapshot.revision));
  }
  if (snapshot.epoch.has_value() && *snapshot.epoch < 0) {
    return disagree(QStringLiteral("native.status.epoch.invalid"),
                    QStringLiteral("snapshot epoch %1 is negative").arg(*snapshot.epoch));
  }

  // Non-empty exactly when not Active, or the document cannot be shown truthfully.
  if (snapshot.state == NativeState::Active) {
    if (!snapshot.reason.isEmpty()) {
      return disagree(QStringLiteral("native.status.reason.unexpected"),
                      QStringLiteral("an Active snapshot carries a reason: %1")
                          .arg(snapshot.reason));
    }
  } else if (snapshot.reason.isEmpty()) {
    return disagree(QStringLiteral("native.status.reason.missing"),
                    QStringLiteral("state %1 carries no reason")
                        .arg(stableName(snapshot.state)));
  }

  if (snapshot.requested.has_value() && !snapshot.requested->recipeId.isEmpty() &&
      !isValidModelId(snapshot.requested->recipeId)) {
    return disagree(QStringLiteral("native.status.requested.recipe.invalid"),
                    QStringLiteral("requested recipe id %1 is not a usable identifier")
                        .arg(snapshot.requested->recipeId));
  }

  if (!snapshot.effective.has_value()) {
    if (snapshot.counters.has_value()) {
      return disagree(QStringLiteral("native.status.counters.without.effective"),
                      QStringLiteral("the snapshot counts frames with no effective plan"));
    }
  } else {
    // A chain that is not running publishes no plan. Off has no vf in the graph
    // and Probe is the passthrough that does no work at all, so an effective plan
    // under either would be the one thing this reader must never let a consumer
    // read as applied. A Failed or Suspended snapshot still carries the plan it
    // was running, which is a different fact and stays allowed.
    if (snapshot.state == NativeState::Off || snapshot.state == NativeState::Probe) {
      return disagree(QStringLiteral("native.status.effective.unexpected"),
                      QStringLiteral("state %1 publishes no effective plan")
                          .arg(stableName(snapshot.state)));
    }
    const NativeEffectivePlan& plan = *snapshot.effective;
    if (!plan.sourceSize.isValid() || !plan.outputSize.isValid()) {
      return disagree(QStringLiteral("native.status.effective.geometry"),
                      QStringLiteral("the effective geometry %1x%2 -> %3x%4 is not positive")
                          .arg(plan.sourceSize.w)
                          .arg(plan.sourceSize.h)
                          .arg(plan.outputSize.w)
                          .arg(plan.outputSize.h));
    }
    // Both rates are required; an absent one is refused like an unusable one,
    // never defaulted.
    if (!plan.sourceFps.has_value() || !plan.outputFps.has_value() ||
        !rateOk(*plan.sourceFps) || !rateOk(*plan.outputFps)) {
      return disagree(QStringLiteral("native.status.effective.cadence"),
                      QStringLiteral("the effective cadence %1 -> %2 is not two usable rates")
                          .arg(showRate(plan.sourceFps), showRate(plan.outputFps)));
    }
    if (plan.interpolation.has_value()) {
      if (plan.interpolation->model.isEmpty() || !rateOk(plan.interpolation->factor)) {
        return disagree(QStringLiteral("native.status.effective.interpolation.malformed"),
                        QStringLiteral("the effective interpolation has no model or no usable "
                                       "factor"));
      }
      // Interpolation adds frames, never pixels: no spatial stage cannot move geometry.
      if (plan.spatialStages == 0 && plan.outputSize != plan.sourceSize) {
        return disagree(QStringLiteral("native.status.effective.geometry"),
                        QStringLiteral("a chain with no spatial stage reports %1x%2 as output of "
                                       "%3x%4")
                            .arg(plan.outputSize.w)
                            .arg(plan.outputSize.h)
                            .arg(plan.sourceSize.w)
                            .arg(plan.sourceSize.h));
      }
      if (!scaledRate(*plan.outputFps, *plan.sourceFps, plan.interpolation->factor)) {
        return disagree(QStringLiteral("native.status.effective.cadence"),
                        QStringLiteral("output cadence %1 is not source %2 scaled by %3")
                            .arg(showRate(*plan.outputFps), showRate(*plan.sourceFps),
                                 showRate(plan.interpolation->factor)));
      }
    } else {
      if (plan.spatialStages == 0) {
        return disagree(QStringLiteral("native.status.effective.stages"),
                        QStringLiteral("the effective plan has no spatial stage and no "
                                       "interpolation, so there is nothing to run"));
      }
      if (!sameRate(*plan.outputFps, *plan.sourceFps)) {
        return disagree(QStringLiteral("native.status.effective.cadence"),
                        QStringLiteral("a spatial-only chain reports the cadence change %1 -> %2")
                            .arg(showRate(*plan.sourceFps), showRate(*plan.outputFps)));
      }
    }
  }

  if (snapshot.counters.has_value()) {
    const NativeCadenceCounters& counters = *snapshot.counters;
    if (counters.interpolated < 0 || counters.framesOut < 0) {
      return disagree(QStringLiteral("native.status.counters.malformed"),
                      QStringLiteral("the counters are negative"));
    }
    const bool spatialOnly = snapshot.effective.has_value() &&
                             !snapshot.effective->interpolation.has_value();
    if (spatialOnly) {
      if (counters.framesOut != counters.framesIn) {
        return disagree(QStringLiteral("native.status.counters.inconsistent"),
                        QStringLiteral("a spatial-only chain reports %1 frames out of %2 in")
                            .arg(counters.framesOut)
                            .arg(counters.framesIn));
      }
      if (counters.interpolated != 0) {
        return disagree(QStringLiteral("native.status.counters.inconsistent"),
                        QStringLiteral("a spatial-only chain reports %1 interpolated frames")
                            .arg(counters.interpolated));
      }
    } else if (snapshot.effective.has_value() &&
               snapshot.effective->interpolation.has_value()) {
      // `interpolated` counts the frames actually synthesized at the output pin,
      // not the net gain over the source. The filter consumes source frames ahead
      // of the output pin, so a snapshot is a transient, not a closed segment: a
      // source original can still be pending when a synthesized frame is already
      // out. The per-source-frame synthesis rate is therefore the safe upper
      // bound (num-1)/den over the consumed frames, and the emitted total limits
      // it too. A factor at or below one synthesizes nothing.
      const NativeInterpolation& interpolation = *snapshot.effective->interpolation;
      const qint64 divisor = std::gcd(interpolation.factor.num, interpolation.factor.den);
      const qint64 num = interpolation.factor.num / divisor;
      const qint64 den = interpolation.factor.den / divisor;
      const bool tooMany = counters.interpolated > counters.framesOut ||
                           (num > den &&
                            !countWithinRatio(counters.framesIn, counters.interpolated,
                                              num - 1, den));
      if (num <= den ? counters.interpolated != 0 : tooMany) {
        return disagree(QStringLiteral("native.status.counters.inconsistent"),
                        QStringLiteral("the chain reports %1 interpolated frames from %2 source "
                                       "frames out of %3 at a factor of %4")
                            .arg(counters.interpolated)
                            .arg(counters.framesIn)
                            .arg(counters.framesOut)
                            .arg(showRate(interpolation.factor)));
      }
      // The grid cannot emit more frames than the factor lays down for the source
      // frames it consumed, whatever the scene/drop split.
      if (!countWithinCeilRatio(counters.framesIn, counters.framesOut, num, den)) {
        return disagree(QStringLiteral("native.status.counters.inconsistent"),
                        QStringLiteral("the chain reports %1 frames out of %2 source frames at a "
                                       "factor of %3")
                            .arg(counters.framesOut)
                            .arg(counters.framesIn)
                            .arg(showRate(interpolation.factor)));
      }
    }
  }

  if (snapshot.probe.has_value()) {
    const NativeProbePayload& probe = *snapshot.probe;
    if (probe.source.has_value()) {
      const SourceGeometry& source = *probe.source;
      if (source.codedSize.has_value() && isRefusableGeometry(*source.codedSize)) {
        return disagree(QStringLiteral("native.status.probe.geometry"),
                        QStringLiteral("the probe coded size is not a usable frame size"));
      }
      if (source.rotationDegrees.has_value() && !source.hasModelledRotation()) {
        return disagree(QStringLiteral("native.status.probe.geometry"),
                        QStringLiteral("the probe rotation is not one this build models"));
      }
      if (source.crop.has_value() && source.codedSize.has_value()) {
        const QRect crop = *source.crop;
        const QSize coded = *source.codedSize;
        if (crop.x() < 0 || crop.y() < 0 || crop.width() <= 0 || crop.height() <= 0 ||
            crop.x() + crop.width() > coded.width() ||
            crop.y() + crop.height() > coded.height()) {
          return disagree(QStringLiteral("native.status.probe.geometry"),
                          QStringLiteral("the probe crop is empty or reaches outside the frame"));
        }
      }
      if (source.fps.has_value() && !rateOk(*source.fps)) {
        return disagree(QStringLiteral("native.status.probe.cadence"),
                        QStringLiteral("the probe fps is not a usable rate"));
      }
      if (source.bitDepth != 0 && source.bitDepth != 8 && source.bitDepth != 10 &&
          source.bitDepth != 12 && source.bitDepth != 16) {
        return disagree(QStringLiteral("native.status.probe.geometry"),
                        QStringLiteral("the probe bitDepth is not a depth this build binds"));
      }
    }
    if (probe.device.has_value()) {
      const DeviceIdentity& device = *probe.device;
      if (!device.decoderDeviceUuid.isEmpty() && !probeIdOk(device.decoderDeviceUuid)) {
        return disagree(QStringLiteral("native.status.probe.identity"),
                        QStringLiteral("the probe decoder device is not an observed identity"));
      }
      if (!device.computeDeviceUuid.isEmpty() && !probeIdOk(device.computeDeviceUuid)) {
        return disagree(QStringLiteral("native.status.probe.identity"),
                        QStringLiteral("the probe compute device is not an observed identity"));
      }
      if (!device.adapterLuid.isEmpty() && !probeIdOk(device.adapterLuid)) {
        return disagree(QStringLiteral("native.status.probe.identity"),
                        QStringLiteral("the probe adapter is not an observed identity"));
      }
      if (device.smMinor != 0 && device.smMajor <= 0) {
        return disagree(QStringLiteral("native.status.probe.identity"),
                        QStringLiteral("the probe smMinor names no measured smMajor"));
      }
    }
  }

  if (snapshot.factory.has_value()) {
    const NativeFactoryObservation& factory = *snapshot.factory;
    if (!factory.observed && (factory.spatial || factory.rife)) {
      return disagree(QStringLiteral("native.status.factory.inconsistent"),
                      QStringLiteral("an unobserved factory claims a supported role"));
    }
    if (!factory.backend.isEmpty() && !factoryBackendNameOk(factory.backend)) {
      return disagree(QStringLiteral("native.status.factory.backend"),
                      QStringLiteral("the factory backend is not one this build checks"));
    }
    if (!factory.reasonKey.isEmpty() && !factoryReasonKeyOk(factory.reasonKey)) {
      return disagree(QStringLiteral("native.status.factory.reason"),
                      QStringLiteral("the factory reasonKey is not a stable key"));
    }
    if (!factory.deviceIdentity.isEmpty() && !isActualDeviceUuid(factory.deviceIdentity)) {
      return disagree(QStringLiteral("native.status.factory.identity"),
                      QStringLiteral("the factory deviceIdentity is not an observed device"));
    }
    // A factory observation belongs to THIS source: when both the probe and the
    // factory name a device, the factory's must be one the probe actually saw. A
    // usable factory from another device would otherwise read as availability for
    // the wrong core. An unavailable factory carries no deviceIdentity, so this
    // never turns a named unavailability into a refusal.
    if (!factory.deviceIdentity.isEmpty() && snapshot.probe.has_value() &&
        snapshot.probe->device.has_value()) {
      const DeviceIdentity& device = *snapshot.probe->device;
      const QString factoryHex = deviceUuidHex(factory.deviceIdentity);
      bool anyProbeDevice = false;
      bool matched = false;
      const QString probeIds[] = {deviceUuidHex(device.decoderDeviceUuid),
                                  deviceUuidHex(device.computeDeviceUuid)};
      for (const QString& id : probeIds) {
        if (id.isEmpty()) continue;
        anyProbeDevice = true;
        if (id == factoryHex) matched = true;
      }
      if (anyProbeDevice && !matched) {
        return disagree(QStringLiteral("native.status.factory.identity"),
                        QStringLiteral("the factory device is not one the probe observed"));
      }
    }
    if (factory.smMinor != 0 && factory.smMajor <= 0) {
      return disagree(QStringLiteral("native.status.factory.identity"),
                      QStringLiteral("the factory smMinor names no measured smMajor"));
    }
    if (factory.smMajor > 32 || factory.smMinor > 99) {
      return disagree(QStringLiteral("native.status.factory.identity"),
                      QStringLiteral("the factory capability is not a measured one"));
    }
    if (factory.isUsable()) {
      // Support can only be claimed by an observed success that names the real
      // backend, runtime and measured device it proved itself against.
      if (factory.reasonKey.isEmpty() && factory.runtimeVersion.isEmpty()) {
        return disagree(QStringLiteral("native.status.factory.identity"),
                        QStringLiteral("a usable factory names no runtime it checked"));
      }
      if (factory.deviceIdentity.isEmpty() || factory.smMajor == 0) {
        return disagree(QStringLiteral("native.status.factory.identity"),
                        QStringLiteral("a usable factory names no measured device"));
      }
    } else {
      if (factory.spatial || factory.rife) {
        return disagree(QStringLiteral("native.status.factory.inconsistent"),
                        QStringLiteral("an unavailable factory claims a supported role"));
      }
      if (factory.reasonKey.isEmpty()) {
        return disagree(QStringLiteral("native.status.factory.reason"),
                        QStringLiteral("an unavailable factory names no reason"));
      }
    }
  }

  return true;
}

AvailabilityState statusSnapshotAvailability(const StatusSnapshot& snapshot) {
  switch (snapshot.state) {
    case NativeState::Active:
      return readyAvailability();
    case NativeState::Preparing:
      return preparingAvailability(QStringLiteral("native.status.preparing"), snapshot.reason);
    case NativeState::NeedsPreparation:
      return preparingAvailability(QStringLiteral("native.status.needsPreparation"), snapshot.reason);
    default:
      // The filter names the state it is in, so the key carries that name: a
      // reader can tell "suspended" from "failed" without parsing the reason.
      return unavailableAvailability(
          QStringLiteral("native.status.state.%1").arg(stableName(snapshot.state)), snapshot.reason);
  }
}

bool statusRefusalHaltsPolling(const QString& reasonKey) {
  static const QStringList refused{
      QStringLiteral("native.status.missing"),         // no mikai_native filter attached
      QStringLiteral("native.status.request"),         // the core would not take the read
      QStringLiteral("native.status.schema.unknown"),  // another build's tag schema
      QStringLiteral("native.status.protocol.unknown"),
      QStringLiteral("native.status.tags.unknown"),    // a tag this build does not define
      QStringLiteral("native.status.snapshot.mismatch"),
      QStringLiteral("native.status.snapshot.malformed"),
      QStringLiteral("native.status.sensitiveData"),
  };
  return refused.contains(reasonKey);
}

StatusRevision classifyStatusRevision(const StatusSnapshot& previous,
                                      const StatusSnapshot& next) {
  if (next.revision > previous.revision) return StatusRevision::Advanced;
  if (next.revision == previous.revision) return StatusRevision::Unchanged;
  // A backward revision is stale unless a new epoch restarted the filter's counter;
  // refusing it would freeze the HUD on the previous session.
  const bool newSession = previous.epoch.has_value() && next.epoch.has_value() &&
                          *previous.epoch != *next.epoch;
  return newSession ? StatusRevision::Advanced : StatusRevision::Stale;
}

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

NativeStatusConsumer::NativeStatusConsumer() = default;

NativeStatusConsumer::~NativeStatusConsumer() {
  // The callback reaches a listener whose owner may already be tearing down, and
  // detach() clears the published view - which is a change worth reporting. The
  // binding is still released; only the notification is dropped.
  callback_ = nullptr;
  detach();
}

void NativeStatusConsumer::publish(NativeStatusView next) {
  if (next == published_) return;
  published_ = std::move(next);
  if (callback_) callback_(published_);
}

void NativeStatusConsumer::publishUnavailable(const QString& reasonKey, const QString& reason) {
  NativeStatusView next;
  // Cleared with the availability: a plan that may no longer apply must not read as current.
  next.availability = unavailableAvailability(reasonKey, reason);
  publish(std::move(next));
}

void NativeStatusConsumer::attach(MpvHandle* handle) {
  if (handle == nullptr) {
    detach();
    return;
  }
  if (handle == handle_) return;
  detach();
  handle_ = handle;
  ++token_;
  requested_ = false;
  lastRequestMs_ = 0;
  publishUnavailable(QStringLiteral("native.status.unread"),
                     QStringLiteral("The native status has not been read yet."));
}

void NativeStatusConsumer::detach() {
  if (handle_ != nullptr && outstanding_) handle_->cancelAsync(pending_);
  pending_ = AsyncRequestId{};
  outstanding_ = false;
  requested_ = false;
  handle_ = nullptr;
  currentEpoch_ = 0;
  ++token_;
  // The view is cleared with the binding: a snapshot read through a core this
  // reader no longer borrows says nothing about the next one, and a plan that
  // may no longer apply must not read as current. The published state returns to
  // the same "not read yet" a fresh reader starts from.
  publishUnavailable(QStringLiteral("native.status.unread"),
                     QStringLiteral("The native status has not been read yet."));
}

void NativeStatusConsumer::setChangeCallback(
    std::function<void(const NativeStatusView&)> callback) {
  callback_ = std::move(callback);
}

void NativeStatusConsumer::tick(qint64 nowMs, quint64 currentEpoch,
                                bool pollActivePreparingSwitching) {
  if (handle_ == nullptr) return;
  // The newest pump step's epoch is what a late reply is compared against.
  currentEpoch_ = currentEpoch;
  if (outstanding_ && nowMs >= lastRequestMs_ &&
      nowMs - lastRequestMs_ >= kNativeStatusRequestTimeoutMs) {
    // One lost reply must not stop the poll forever.
    handle_->cancelAsync(pending_);
    pending_ = AsyncRequestId{};
    outstanding_ = false;
    ++dropped_;
  }
  if (!pollActivePreparingSwitching || outstanding_) return;
  if (requested_ && nowMs >= lastRequestMs_ &&
      nowMs - lastRequestMs_ < kNativeStatusPollIntervalMs) {
    return;
  }

  const quint64 epoch = currentEpoch;
  const quint64 token = token_;
  requested_ = true;
  lastRequestMs_ = nowMs;
  ++requests_;
  outstanding_ = true;
  pending_ = handle_->getPropertyAsync(
      kNativeStatusProperty, MPV_FORMAT_NODE, [this, epoch, token](const AsyncReply& reply) {
        // Runs on the draining thread; the reply node is owned by AsyncReply, so
        // everything kept is copied here while mpv's storage is still alive.
        if (token != token_) return;
        outstanding_ = false;
        pending_ = AsyncRequestId{};
        if (epoch != currentEpoch_) {
          // A reply for a superseded epoch says nothing about the current one.
          ++dropped_;
          return;
        }
        accept(reply);
      });
  if (!pending_.valid()) {
    outstanding_ = false;
    publishUnavailable(QStringLiteral("native.status.request"),
                       QStringLiteral("%1 could not be requested on this core")
                           .arg(nativeStatusProperty()));
  }
}

void NativeStatusConsumer::accept(const AsyncReply& reply) {
  if (reply.error != 0) {
    // No filter attached or the core refused the read: named rather than an
    // empty success.
    publishUnavailable(
        QStringLiteral("native.status.missing"),
        QStringLiteral("%1 answered mpv error %2, so no native status is attached")
            .arg(nativeStatusProperty())
            .arg(reply.error));
    return;
  }

  StatusSnapshot parsed;
  QString errorKey;
  QString error;
  if (!parseStatusTags(reply.node, &parsed, &errorKey, &error)) {
    ++dropped_;
    publishUnavailable(errorKey, error);
    return;
  }

  if (published_.snapshot.has_value()) {
    switch (classifyStatusRevision(*published_.snapshot, parsed)) {
      case StatusRevision::Unchanged:
        // Unchanged: a signal here would be a change notification that is not one.
        return;
      case StatusRevision::Stale:
        ++dropped_;
        return;
      case StatusRevision::Advanced:
        break;
    }
  }

  ++accepted_;
  NativeStatusView next;
  // The availability comes from the snapshot's own state, never a bare Ready: a
  // snapshot that names a reason is by definition not running.
  next.availability = statusSnapshotAvailability(parsed);
  next.snapshot = parsed;
  publish(std::move(next));
}

}  // namespace native
}  // namespace mplayer

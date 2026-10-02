#include "NativeStatus.hpp"

#include <limits>
#include <utility>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QStringList>

#include "NativeDiagnostics.hpp"

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
  return false;
}

/// The fixed refusal text; never names or repeats the offending value.
QString sensitiveStatusReason() {
  return QStringLiteral("the snapshot carries a URL or a credential-looking value");
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
          QStringLiteral("effective"), QStringLiteral("counters"), QStringLiteral("reason")};
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

}  // namespace

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
    } else if (snapshot.effective.has_value() && counters.interpolated > counters.framesIn) {
      // Each source pair yields at most one extra frame, scene cuts excluded.
      return disagree(QStringLiteral("native.status.counters.inconsistent"),
                      QStringLiteral("the chain reports %1 interpolated frames from %2 source "
                                     "frames")
                          .arg(counters.interpolated)
                          .arg(counters.framesIn));
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
    requested.insert(QStringLiteral("recipe"),
                     redactNativeDiagnosticText(snapshot.requested->recipeId));
    requested.insert(QStringLiteral("backend"), stableName(snapshot.requested->backend));
    document.insert(QStringLiteral("requested"), requested);
  }
  if (snapshot.effective.has_value()) {
    const NativeEffectivePlan& plan = *snapshot.effective;
    QJsonObject effective;
    effective.insert(QStringLiteral("recipe"), redactNativeDiagnosticText(plan.recipe));
    effective.insert(QStringLiteral("provider"), redactNativeDiagnosticText(plan.provider));
    effective.insert(QStringLiteral("model"), redactNativeDiagnosticText(plan.model));
    effective.insert(QStringLiteral("spatialStages"), plan.spatialStages);
    if (plan.interpolation.has_value()) {
      QJsonObject interpolation;
      interpolation.insert(QStringLiteral("num"), plan.interpolation->factor.num);
      interpolation.insert(QStringLiteral("den"), plan.interpolation->factor.den);
      interpolation.insert(QStringLiteral("model"),
                           redactNativeDiagnosticText(plan.interpolation->model));
      effective.insert(QStringLiteral("interpolation"), interpolation);
    } else {
      // A spatial-only chain states the member as null rather than omitting it.
      effective.insert(QStringLiteral("interpolation"), QJsonValue(QJsonValue::Null));
    }
    effective.insert(QStringLiteral("order"), redactNativeDiagnosticText(plan.order));
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
    effective.insert(QStringLiteral("decoder"), redactNativeDiagnosticText(plan.decoder));
    effective.insert(QStringLiteral("csp"), redactNativeDiagnosticText(plan.csp));
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
  // Every string is redacted, not only the reason: the writer may receive an
  // unvalidated snapshot, and a credential must not be emitted in the clear.
  document.insert(QStringLiteral("reason"), redactNativeDiagnosticText(snapshot.reason));
  return document;
}

QByteArray serializeStatusSnapshot(const StatusSnapshot& snapshot) {
  return QJsonDocument(statusSnapshotToJson(snapshot)).toJson(QJsonDocument::Compact);
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

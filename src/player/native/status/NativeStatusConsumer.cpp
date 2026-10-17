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


namespace status_detail {

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


}  // namespace status_detail

using namespace status_detail;

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

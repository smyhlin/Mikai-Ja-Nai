#pragma once

#include "NativeStatus.hpp"

namespace mplayer {
namespace native {
namespace status_detail {

bool rateComponentOk(int value);
bool rateOk(const Rational& rate);
bool sameRate(const Rational& a, const Rational& b);
bool scaledRate(const Rational& out, const Rational& source, const Rational& factor);
bool countWithinRatio(qint64 frames, qint64 count, qint64 numerator, qint64 den);
bool countWithinCeilRatio(qint64 frames, qint64 count, qint64 numerator, qint64 den);
QString showRate(const Rational& rate);
QString showRate(const std::optional<Rational>& rate);

bool statusCarriesSensitiveText(const StatusSnapshot& snapshot);
QString sensitiveStatusReason();

bool isPlaceholderProbeToken(const QString& value);
bool probeIdOk(const QString& value);
QString deviceUuidHex(const QString& value);
bool isActualDeviceUuid(const QString& value);
bool factoryBackendOk(const QString& backend);
bool factoryBackendNameOk(const QString& backend);
bool factoryRuntimeOk(const QString& value);
bool factoryReasonKeyOk(const QString& key);

}  // namespace status_detail
}  // namespace native
}  // namespace mplayer

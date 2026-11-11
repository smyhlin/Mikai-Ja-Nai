// Internal JSON-reading primitives shared by the bundle and manifest parsers.
//
// X-D18: `jsonInteger` / `jsonString` / `noUnknownKeys` were duplicated between
// NativeBundle.cpp and NativeManifest.cpp (anonymous namespace in both, so no
// ODR clash — pure drift risk, and the two `jsonInteger` copies had already
// drifted). One home now. The `jsonInteger` kept here is the NativeBundle
// spelling: it bounds-checks before the double-to-int64 cast, where the
// NativeManifest spelling cast first and compared after, which is undefined
// behaviour for |d| past the int64 range. That also loosens the manifest's
// ceiling from 2^53 to 9.2e18 for integral doubles; every field either side
// parses (versions, ranks, byte counts) is orders of magnitude below both.
//
// NOTE: three more file-local copies of `jsonString` / `noUnknownKeys` remain
// (NativeModelInventory.cpp, NativeSpatialAssets.cpp, NativeStatus.cpp, plus a
// QString-returning `jsonString` in NativeRecipe.cpp / NativeStatus.cpp).
// They differ in signature — no `offender` out-param on `noUnknownKeys`,
// value-returning `jsonString` — so unifying them means touching their call
// sites; left as a follow-up, not smuggled into this move.

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cmath>

namespace mplayer {
namespace native {

/// An integer-valued JSON number as qint64. A QJsonValue double is exact only
/// for integral values inside the int64 range; anything else (fraction,
/// non-finite, out of range) refuses.
inline bool jsonInteger(const QJsonValue& value, qint64* out) {
    if (!value.isDouble()) return false;
    const double d = value.toDouble();
    if (!std::isfinite(d)) return false;
    if (std::floor(d) != d) return false;
    if (d < -9.2e18 || d > 9.2e18) return false;
    *out = static_cast<qint64>(d);
    return true;
}

/// The string member `key`, or false when it is absent or not a string.
inline bool jsonString(const QJsonObject& object, const QString& key, QString* out) {
    const QJsonValue value = object.value(key);
    if (!value.isString()) return false;
    *out = value.toString();
    return true;
}

/// Refuses a member this schema version does not define. A silently ignored
/// typo ("modulePath", "sha2560") produces a bundle whose manifest says
/// something other than what was verified.
inline bool noUnknownKeys(const QJsonObject& object, const QStringList& known,
                          QString* offender) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (known.contains(it.key())) continue;
        if (offender) *offender = it.key();
        return false;
    }
    return true;
}

}  // namespace native
}  // namespace mplayer

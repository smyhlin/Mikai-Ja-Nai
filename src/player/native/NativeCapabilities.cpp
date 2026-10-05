#include "NativeCapabilities.hpp"

#include "NativeStatus.hpp"

#include <QCoreApplication>

namespace mplayer::native {

NativeCapabilities nativeCapabilitiesFrom(const NativeFactoryObservation& observation) {
    NativeCapabilities capabilities;
    // A refusal, an unexecuted query or an unavailable producer is not a
    // capability: every flag stays off instead of being approximated.
    if (!observation.isUsable()) {
        return capabilities;
    }

    // The token the writer only publishes for a query it actually executed; the
    // spatial role is a role and never stands in for a backend. This build's
    // producer checks the caller's adopted CUDA context, so TensorRT is the token
    // it names; the DirectML token is accepted for a producer that checks that
    // path, and none publishes one yet.
    if (observation.backend == QLatin1String("tensorrt")) {
        capabilities.hasTensorRT = true;
    } else if (observation.backend == QLatin1String("directml")) {
        capabilities.hasDirectML = true;
    }
    capabilities.supportsRife = observation.rife;
    return capabilities;
}

QString unavailableReason(const NativeCapabilities& caps, const QString& detail) {
    if (!caps.hasTensorRT && !caps.hasDirectML) {
        return QCoreApplication::translate(
            "NativeCapabilities",
            "Native unavailable: no inference backend present (TensorRT or DirectML).");
    }
    // A reported 0 is "that side observed no protocol", not a mismatch: a
    // mismatch needs two observed versions, and a capability set projected from a
    // factory observation carries neither this field nor an engine schema. Such a
    // set is reported by the backend branch above and by `detail`.
    if (caps.protocolVersion != 0 && caps.protocolVersion != kNativeProtocolVersion) {
        return QCoreApplication::translate(
                   "NativeCapabilities",
                   "Native unavailable: protocol mismatch (backend %1, player %2).")
            .arg(caps.protocolVersion)
            .arg(kNativeProtocolVersion);
    }
    if (!detail.isEmpty()) {
        return detail;
    }
    return QCoreApplication::translate(
        "NativeCapabilities",
        "Native unavailable: required capability not present.");
}

} // namespace mplayer::native

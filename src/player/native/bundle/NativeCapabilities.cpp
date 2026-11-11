#include "NativeCapabilities.hpp"

#include "native/status/NativeStatus.hpp"


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
    //
    // `tensorrt-rtx` is deliberately absent: the Standard AJI dispatcher can ask
    // for it, but the private Native ABI-8 producer cannot run it, so turning on
    // a TensorRT flag here would claim an engine this path does not have. The
    // caller refuses the token by name instead of reading it as DirectML.
    if (observation.backend == QLatin1String("tensorrt")) {
        capabilities.hasTensorRT = true;
    } else if (observation.backend == QLatin1String("directml")) {
        capabilities.hasDirectML = true;
    }
    capabilities.supportsRife = observation.rife;
    return capabilities;
}

} // namespace mplayer::native

#pragma once

#include <cstdint>
#include <optional>
#include <QString>

namespace mplayer::native {

// The two version numbers the Qt side owns. They are deliberately NOT the
// producer wire's: a factory observation states its own query protocol (1), its
// native protocol (2), its native schema (2) and the AJI source ABI (8), and each
// of those is validated on its own in NativeStatus.hpp. Conflating them with the
// pair below would make independent agreements - the status wire, and the engine
// schema a prepared manifest carries - look like one, so neither constant is ever
// aliased to, derived from or bumped to the producer's numbers.
//
// Additive native protocol version negotiated at load time. The Qt side and the
// inference backend must agree on this version for any structured snapshot;
// mismatch yields NativeUnavailable with both versions reported. No legacy log
// parsing is used.
inline constexpr uint32_t kNativeProtocolVersion = 1;

// Engine schema version carried in manifests and status.
inline constexpr uint32_t kEngineSchemaVersion = 1;

/// Declared, not included: a caller that only reads projected flags must not pull
/// the status reader's headers in. The definition lives in NativeStatus.hpp.
struct NativeFactoryObservation;

// Probe-only passthrough state: the filter attaches with no engine,
// no inference and no RIFE. It reports the real decoder device
// (CUdevice UUID or D3D11 adapter LUID) and geometry/CSP so that
// preparation can compute a correct build key. Status must be
// Probe (never Active). This state exists because mpv's public
// video-params surface does not expose the actual AVHWFramesContext
// device identity.
struct ProbeOnlyPassthrough {
    std::optional<QString> cuDeviceUuid;   // CUDA path
    std::optional<QString> d3dAdapterLuid; // DirectML path
    // geometry and CSP are taken from the existing mpv property surface
};

// Capability flags reported by the loaded backend.
// Absent capability is reported as absent, never approximated.
struct NativeCapabilities {
    bool hasTensorRT = false;
    bool hasDirectML = false;
    bool supportsProbePassthrough = false; // the additive path above
    bool supportsRife = false;
    /// The status protocol the observed side reported; 0 is "not observed".
    /// Never the producer's query protocol, native protocol/schema or source
    /// ABI: those are separate agreements checked on the wire, and echoing one
    /// of them here would report an agreement that was not made.
    uint32_t protocolVersion = 0;
    /// The engine schema the observed side carries; 0 is "not observed". A
    /// factory observation carries no engine schema, so a projection of one
    /// leaves this 0 rather than adopting the snapshot schema of the message
    /// that carried it.
    uint32_t engineSchemaVersion = 0;
    // provider identity strings are factual (e.g. "TensorRT 11.3.0.99"); empty
    // when nothing named one
    QString tensorrtProvider;
    QString directmlProvider;
};

/// The truthful projection of one parsed factory observation onto the flags
/// above. Pure and total: it reads the observation and nothing else - no
/// preference, manifest, catalogue, file or global state - so the same
/// observation always projects the same flags.
///
/// A flag turns on only for an observation `isUsable()`: an executed query that
/// answered success for the exact adopted source/device context it names. The
/// checked backend token then names the backend (`tensorrt` -> `hasTensorRT`,
/// `directml` -> `hasDirectML`) and the answered RIFE role turns `supportsRife`
/// on. The spatial role is a ROLE, not a backend, so it never stands in for one;
/// `runtimeVersion`, `deviceIdentity` and `smMajor`/`smMinor` are observed
/// identities, not capability flags. An unobserved or refused observation
/// projects every flag off and carries no claim. Provider strings and
/// probe-passthrough support are not carried by a factory observation, and
/// `protocolVersion`/`engineSchemaVersion` stay 0: absent is reported absent
/// rather than approximated.
NativeCapabilities nativeCapabilitiesFrom(const NativeFactoryObservation& observation);

// One-line factual reason when a capability or whole native path is unavailable.
QString unavailableReason(const NativeCapabilities& caps, const QString& detail = {});

}  // namespace mplayer::native

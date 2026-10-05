// The manifest runner: executes one declarative ProbeManifest against the real
// application and reports a pass/fail line for every assertion.
//
// It owns exactly one surface per run - the render surface alone, or the real
// player dialog - builds it from the manifest, walks the manifest's steps by
// objectName, then decides the manifest's assertions. Every widget it touches is
// reached through QObject::findChild(objectName); a target that is not there is
// a named failure, never a coordinate or a pixel guess.
//
// Per-assertion "[PASS]"/"[FAIL]" lines are printed as they are decided; this
// header only reports the counts a caller needs to print its own summary.

#pragma once

#include <QString>

#include "ProbeManifest.hpp"

namespace mikai::probes {

/// What one manifest run produced. `detail` names the first fatal problem - a
/// refused surface, a step whose target was missing, a teardown that did not
/// settle - and is empty on a clean run.
struct ProbeRunResult {
    bool ok = false;
    int assertions = 0;  ///< assertions actually evaluated
    int failed = 0;      ///< assertions that did not hold
    QString detail;
};

/// Runs manifests one at a time against surfaces it builds and tears down
/// itself. A QApplication MUST already exist; run() spins its event loop.
class ProbeRunner {
public:
    ProbeRunner();
    ~ProbeRunner();

    ProbeRunner(const ProbeRunner&) = delete;
    ProbeRunner& operator=(const ProbeRunner&) = delete;

    /// Builds the surface the manifest names, runs its steps and assertions,
    /// then closes and settles the surface. Never throws. `manifest.media` is
    /// required when the surface is the player dialog; an absent one refuses
    /// the manifest by name.
    ProbeRunResult run(const ProbeManifest& manifest);
};

}  // namespace mikai::probes

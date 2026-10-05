#pragma once

#include <cstdio>

#include <QtCore/QFileInfo>
#include <QtCore/QString>
#include <QtGlobal>

#include "native/bundle/NativeBundle.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"

// The one core selection every GUI probe starts from.
//
// With no `MIKAI_PROBE_BUNDLE` this is exactly what it always was: the app's
// own Standard selection, which picks and loads from what is already installed.
// Nothing else about the default path changes, so a probe run without the
// variable proves nothing about an owned bundle and never claims to.
//
// `MIKAI_PROBE_BUNDLE` names one absolute bundle root - the immutable
// `<kind>-<version>` directory an install published - and it is taken at face
// value. The manifest is parsed strictly and the bytes are validated in full
// before the loader is asked, then the validated descriptor itself is what
// `commitRuntime` loads, dependency closure and protocol included. That is the
// difference from a module path: a DLL on its own is not a bundle, so naming
// one is refused here instead of being half-trusted.
//
// There is no discovery fallback behind an explicit request. A bad root, a
// manifest this build refuses, a payload that does not match its digests or a
// loader refusal all end the probe with the requested root and the reason
// named, because the whole point of naming a root is that stock mpv must not
// silently stand in for it.
//
// Nothing here writes QSettings, edits PATH or rewrites the installed runtime
// tree: the variable selects what this process loads and is forgotten with it.

namespace mplayer {
namespace probes {

namespace detail {

/// The line that makes the observed proof legible: which module, which
/// descriptor and which kind actually loaded. Without it a desktop run on the
/// stock runtime and a desktop run on an owned one produce the same silence.
inline void reportSelected(const native::RuntimeSelection& selection, const char* origin) {
    std::fprintf(stderr, "[probe-runtime] %s: kind=%s module=%s descriptor=%s version=%s\n", origin,
                 qPrintable(native::bundleKindName(selection.kind)),
                 qPrintable(selection.modulePath), qPrintable(selection.descriptorId),
                 qPrintable(selection.version));
}

inline bool refuse(const QString& requestedRoot, const QString& reason) {
    std::fprintf(stderr, "[probe-runtime] MIKAI_PROBE_BUNDLE=%s refused: %s\n",
                 qPrintable(requestedRoot), qPrintable(reason));
    return false;
}

/// Reads, validates and loads the one bundle the caller named. `root` is the
/// string the caller wrote, kept verbatim for every diagnostic: a refusal that
/// reported a cleaned-up path would name a directory the user never asked for.
inline bool selectNamedBundle(const QString& requestedRoot) {
    const QFileInfo info(requestedRoot);
    if (!info.isAbsolute()) {
        return refuse(requestedRoot, QStringLiteral("the root is not an absolute path"));
    }
    if (!info.isDir()) {
        return refuse(requestedRoot, QStringLiteral("no such directory"));
    }
    const QString root = info.canonicalFilePath();
    if (root.isEmpty()) {
        return refuse(requestedRoot, QStringLiteral("the directory could not be resolved"));
    }

    QString reason;
    const std::optional<native::Bundle> bundle = native::readBundle(root, &reason);
    if (!bundle.has_value()) {
        return refuse(requestedRoot,
                      reason.isEmpty() ? QStringLiteral("no readable bundle.json") : reason);
    }
    if (!native::validateBundle(*bundle, &reason)) {
        return refuse(requestedRoot, reason.isEmpty()
                                        ? QStringLiteral("the bundle does not validate against its files")
                                        : reason);
    }

    // The selection is built from the descriptor that was just checked, and it
    // carries that descriptor whole, so `commitRuntime` can prove the display
    // fields still match it and hand the loader exactly these bytes. The
    // requested kind is the kind the manifest declared: an owned fork may be a
    // Standard bundle, and claiming otherwise would be the label-only inference
    // this codebase refuses everywhere else.
    native::RuntimeSelection resolved;
    resolved.requested = bundle->kind == native::BundleKind::Native ? native::RuntimeChoice::Native
                                                                    : native::RuntimeChoice::Standard;
    resolved.kind = bundle->kind;
    resolved.root = bundle->root;
    resolved.modulePath = bundle->modulePath;
    resolved.descriptorId = bundle->descriptorId;
    resolved.version = bundle->version;
    resolved.resolvedBundle = *bundle;

    const native::RuntimeSelection loaded = native::commitRuntime(resolved);
    if (!loaded.loaded) {
        return refuse(requestedRoot, loaded.reason.isEmpty()
                                        ? QStringLiteral("the validated bundle did not load")
                                        : loaded.reason);
    }
    reportSelected(loaded, requestedRoot.toUtf8().constData());
    return true;
}

}  // namespace detail

/// Selects the runtime this process will play on, and says which one it was.
/// Returns false with the refusal already printed when no runtime could be
/// produced; the caller is expected to exit rather than run on nothing.
inline bool selectRuntime() {
    const QString requestedRoot = qEnvironmentVariable("MIKAI_PROBE_BUNDLE");
    if (!requestedRoot.isEmpty()) {
        return detail::selectNamedBundle(requestedRoot);
    }

    const native::RuntimeSelection selection = native::selectStandardRuntime();
    if (!selection.loaded) {
        // The refusal names whichever check rejected the module - a check that is
        // one message away from being silently read as "no mpv on this machine".
        std::fprintf(stderr, "Standard runtime unavailable: %s\n", qPrintable(selection.reason));
        return false;
    }
    detail::reportSelected(selection, "standard selection");
    return true;
}

}  // namespace probes
}  // namespace mplayer
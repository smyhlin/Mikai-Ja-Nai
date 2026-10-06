#include "NativeRuntimeSelection.hpp"

#include "MpvApi.hpp"

#include <backend/Runtime.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtGlobal>

#include <atomic>
#include <exception>
#include <optional>

namespace mplayer {
namespace native {
namespace {

RuntimeSelection& recorded() {
    static RuntimeSelection selection;
    return selection;
}

std::atomic_bool g_recoveryInFlight{false};

/// The one lookup with this file's context. Every literal it is handed must be
/// wrapped in QT_TRANSLATE_NOOP: that macro is what lupdate reads, and at run
/// time it is the literal itself.
QString translated(const char* message) {
    return QCoreApplication::translate("RuntimeSelection", message);
}

/// The libmpv the stock discovery matched, reduced from whatever shape it
/// reports: it hands back the entry it found, and callers used to pass that
/// string to a search path without caring whether it named the directory or the
/// module itself. Both are accepted here, because a wrong guess means the
/// Standard fallback silently disappears. The name list belongs to the backend,
/// which is the only place that knows which names mpv ships under.
QString moduleFrom(const QDir& found) {
    const QFileInfo entry(found.path());
    if (entry.isFile()) {
        return QDir::cleanPath(entry.absoluteFilePath());
    }
    // The stock discovery searches recursively, so the module may sit below the
    // directory it matched.
    QDirIterator it(found.absolutePath(), mikai::backend::libraryNames(), QDir::Files,
                    QDirIterator::Subdirectories);
    return it.hasNext() ? QDir::cleanPath(it.next()) : QString{};
}

/// Turns a failed resolve into a selection that names what is missing, so every
/// caller reports the same reason instead of inventing its own wording.
RuntimeSelection unavailable(RuntimeChoice choice, const QString& reason) {
    RuntimeSelection selection;
    selection.requested = choice;
    selection.kind = BundleKind::Standard;
    selection.loaded = false;
    selection.runtimeUnavailable = true;
    selection.nativeNotApplied = choice == RuntimeChoice::Native;
    selection.reason = reason;
    return selection;
}

/// Fills the observed fields from a validated descriptor. Capability and native
/// state deliberately stay unset: a bundle that calls itself native has proven
/// nothing until a loaded core registers its filter.
RuntimeSelection describe(const Bundle& bundle, RuntimeChoice choice) {
    RuntimeSelection selection;
    selection.requested = choice;
    selection.kind = bundle.kind;
    selection.root = bundle.root;
    selection.modulePath = bundle.modulePath;
    selection.descriptorId = bundle.descriptorId;
    selection.version = bundle.version;
    selection.loaded = false;
    // The display fields above are a lossy view; the whole validated bundle
    // travels with the selection so commit can hand the loader the exact
    // descriptor that was checked, dependencies and protocol included.
    selection.resolvedBundle = bundle;
    return selection;
}

/// Every legacy candidate module, described in turn, so one corrupt DLL cannot
/// mask a valid one beside it: a candidate that fails validation is named and
/// the search continues. The stock descriptor describes what is already on
/// disk and writes nothing.
RuntimeSelection describeLegacyStandard(const QString& hint) {
    const QStringList modules = mikai::backend::runtimeCandidates(hint);
    if (modules.isEmpty()) {
        return unavailable(RuntimeChoice::Standard,
                           translated(QT_TRANSLATE_NOOP(
                               "RuntimeSelection", "No mpv runtime was found.")));
    }
    QStringList rejections;
    for (const QString& module : modules) {
        QString reason;
        const std::optional<Bundle> bundle = describeStandardRuntimeCached(module, &reason);
        if (bundle.has_value()) {
            return describe(*bundle, RuntimeChoice::Standard);
        }
        rejections.append(QStringLiteral("%1: %2").arg(module, reason));
    }
    return unavailable(RuntimeChoice::Standard,
                       translated(QT_TRANSLATE_NOOP(
                           "RuntimeSelection", "No usable mpv runtime: %1"))
                           .arg(rejections.join(QStringLiteral("; "))));
}

/// Answers from what is already on disk. Never downloads, so the GUI thread can
/// call it during startup without blocking on the network. Installed Standard
/// bundles are considered first, because they are the ones this app validated
/// and published; the legacy locations are the fallback, never the reverse.
RuntimeSelection resolveStandardLocal(const QString& hint) {
    QStringList installedRejections;
    const QList<Bundle> installed = discoverBundles({}, &installedRejections);
    for (const Bundle& bundle : installed) {
        if (bundle.kind == BundleKind::Standard) {
            return describe(bundle, RuntimeChoice::Standard);
        }
    }
    RuntimeSelection legacy = describeLegacyStandard(hint);
    if (!legacy.resolvedBundle.has_value() && !installedRejections.isEmpty()) {
        legacy.reason = legacy.reason.isEmpty()
                            ? installedRejections.join(QStringLiteral("; "))
                            : QStringLiteral("%1; %2").arg(
                                  legacy.reason,
                                  installedRejections.join(QStringLiteral("; ")));
    }
    return legacy;
}

/// The provisioning half: when nothing already installed validates or loads,
/// the stock archive is fetched and published as an app-owned Standard bundle.
/// The download lands in app-owned scratch and is validated before the atomic
/// installer publishes it, so a reference install and the stock cache are never
/// written, replaced or deleted.
RuntimeSelection provisionStandard() {
    const QString scratchRoot = QDir(nativeStorageRoot()).filePath(QStringLiteral("scratch"));
    if (!QDir().mkpath(scratchRoot)) {
        return unavailable(RuntimeChoice::Standard,
                           translated(QT_TRANSLATE_NOOP(
                               "RuntimeSelection", "No app-owned scratch directory is available.")));
    }
    QTemporaryDir work(QDir(scratchRoot).filePath(QStringLiteral("provision-XXXXXX")));
    if (!work.isValid()) {
        return unavailable(RuntimeChoice::Standard,
                           translated(QT_TRANSLATE_NOOP(
                               "RuntimeSelection", "No app-owned scratch directory is available.")));
    }
    try {
        const QDir extracted = mikai::backend::provisionRuntime(work.path());
        const QString module = moduleFrom(extracted);
        QString describeReason;
        const std::optional<Bundle> described = describeStandardRuntime(module, &describeReason);
        if (!described.has_value()) {
            return unavailable(RuntimeChoice::Standard, describeReason);
        }
        QString installReason;
        QString publishedRoot;
        if (!installAtomic(*described, runtimeStorageRoot(), &installReason, &publishedRoot)) {
            return unavailable(RuntimeChoice::Standard, installReason);
        }
        // Re-read and revalidate the published descriptor rather than trusting
        // the source it was copied from: what commit loads has to be exactly
        // what the installer published.
        QString readReason;
        const std::optional<Bundle> published = readBundle(publishedRoot, &readReason);
        if (!published.has_value()) {
            return unavailable(RuntimeChoice::Standard, readReason);
        }
        QString validateReason;
        if (!validateBundle(*published, &validateReason)) {
            return unavailable(RuntimeChoice::Standard, validateReason);
        }
        return describe(*published, RuntimeChoice::Standard);
    } catch (const std::exception& error) {
        return unavailable(RuntimeChoice::Standard, QString::fromUtf8(error.what()));
    } catch (...) {
        return unavailable(RuntimeChoice::Standard,
                           translated(QT_TRANSLATE_NOOP(
                               "RuntimeSelection",
                               "The mpv runtime could not be provisioned.")));
    }
}

/// The stock discovery with provisioning: the only path that may download, so
/// it belongs off the GUI thread. A download failure is named rather than
/// thrown, because the app stays usable either way.
RuntimeSelection resolveStandardProvisioned(const QString& hint) {
    RuntimeSelection local = resolveStandardLocal(hint);
    if (local.resolvedBundle.has_value()) {
        return local;
    }
    return provisionStandard();
}

/// The native half: the first Native bundle that validates, described and
/// nothing more. A missing or unusable bundle names why in `reason`, so the
/// Standard fallback carries that sentence instead of inventing its own.
RuntimeSelection resolveNative(const QString& hint, QString* reason) {
    QStringList rejections;
    // The hint is searched before the installed locations, exactly as the
    // Standard half searches it: one resolver, one order, so a probe pointing
    // at a scratch tree never needs a second selection path to exist.
    QStringList roots;
    if (!hint.isEmpty()) {
        roots.append(hint);
    }
    const QList<Bundle> candidates = discoverBundles(roots, &rejections);
    for (const Bundle& bundle : candidates) {
        if (bundle.kind == BundleKind::Native) {
            return describe(bundle, RuntimeChoice::Native);
        }
    }
    if (reason != nullptr) {
        *reason = rejections.isEmpty()
                      ? translated(QT_TRANSLATE_NOOP(
                            "RuntimeSelection", "No native runtime bundle is installed."))
                      : translated(QT_TRANSLATE_NOOP(
                            "RuntimeSelection", "No usable native runtime bundle: %1"))
                            .arg(rejections.join(QStringLiteral("; ")));
    }
    return unavailable(RuntimeChoice::Native, QString());
}

/// The one place the native-first-then-Standard rule is stated. `standard` is
/// the resolver that answers the fallback, so the startup answer stays local
/// while the recovery answer may provision.
template <typename StandardResolver>
RuntimeSelection resolveWith(RuntimeChoice choice, StandardResolver standard, const QString& hint) {
    if (choice != RuntimeChoice::Native) {
        return standard(hint);
    }
    // Native first, then Standard as the one module this process will ever
    // load. The refusal rides along on the fallback so the surface can say why
    // the request was not honoured; the loaded kind never claims to be native.
    QString reason;
    const RuntimeSelection native = resolveNative(hint, &reason);
    if (!native.modulePath.isEmpty()) {
        return native;
    }
    // The refusal is always named, even when Standard stands in for playback:
    // what is unavailable here is the *native* runtime, not playback. An empty
    // reason would let the surface read as though the request was ignored.
    if (reason.isEmpty()) {
        reason = translated(QT_TRANSLATE_NOOP(
            "RuntimeSelection", "No native runtime bundle is installed."));
    }
    RuntimeSelection fallback = standard(hint);
    fallback.requested = RuntimeChoice::Native;
    // Both flags travel together, as plan 8.4 requires: the requested runtime is
    // unavailable and native was not applied, while Standard carries playback.
    fallback.nativeNotApplied = true;
    fallback.runtimeUnavailable = true;
    // Two named failures compose into one line: which native bundle was refused,
    // and why Standard could not stand in for it either. A Standard that did
    // load keeps only the native half, because its own state needs no excuse.
    if (fallback.resolvedBundle.has_value() || fallback.reason.isEmpty()) {
        fallback.reason = reason;
    } else {
        fallback.reason = QStringLiteral("%1; %2").arg(reason, fallback.reason);
    }
    return fallback;
}

/// Why the choice cannot be honoured without a restart: the pinned module and
/// the asked-for kind, both named, because "restart" alone does not say what a
/// restart would change.
QString restartReason(BundleKind pinned, RuntimeChoice requested) {
    return translated(QT_TRANSLATE_NOOP("RuntimeSelection",
                                       "%1 is loaded; %2 takes effect after a restart."))
        .arg(bundleKindName(pinned),
             runtimeChoiceToString(requested) == QLatin1String("native")
                 ? translated(QT_TRANSLATE_NOOP("RuntimeSelection", "native"))
                 : translated(QT_TRANSLATE_NOOP("RuntimeSelection", "standard")));
}

/// The GUI-thread half: never downloads, so startup cannot block on the network.
RuntimeSelection resolveRuntimeLocal(RuntimeChoice choice, const QString& hint) {
    return resolveWith(choice, &resolveStandardLocal, hint);
}

}  // namespace

QString runtimeChoiceSettingsGroup() { return QStringLiteral("player/native"); }

QString runtimeChoiceSettingsKey() { return QStringLiteral("runtimeChoice"); }

QString runtimeChoiceToString(RuntimeChoice choice) {
    return choice == RuntimeChoice::Native ? QStringLiteral("native") : QStringLiteral("standard");
}

RuntimeChoice runtimeChoiceFromString(const QString& stored) {
    return stored.trimmed().compare(QLatin1String("native"), Qt::CaseInsensitive) == 0
               ? RuntimeChoice::Native
               : RuntimeChoice::Standard;
}

RuntimeChoice storedRuntimeChoice() {
    QSettings settings;
    settings.beginGroup(runtimeChoiceSettingsGroup());
    const QString stored = settings.value(runtimeChoiceSettingsKey()).toString();
    settings.endGroup();
    return runtimeChoiceFromString(stored);
}

bool storeRuntimeChoice(RuntimeChoice choice, QString* reason) {
    QSettings settings;
    settings.beginGroup(runtimeChoiceSettingsGroup());
    settings.setValue(runtimeChoiceSettingsKey(), runtimeChoiceToString(choice));
    settings.endGroup();
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (reason != nullptr) {
            *reason = translated(QT_TRANSLATE_NOOP(
                          "RuntimeSelection", "The runtime choice could not be saved."));
        }
        return false;
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return true;
}

bool restartRequiredFor(RuntimeChoice choice) {
    const RuntimeSelection& selection = recorded();
    if (!selection.loaded) {
        // Nothing is pinned, so the next selection reads whatever was stored.
        return false;
    }
    const BundleKind wanted =
        choice == RuntimeChoice::Native ? BundleKind::Native : BundleKind::Standard;
    return selection.kind != wanted;
}

RuntimeSelection resolveRuntime(RuntimeChoice choice, const QString& hint) {
    return resolveWith(choice, &resolveStandardProvisioned, hint);
}

RuntimeSelection commitRuntime(const RuntimeSelection& resolved) {
    if (recorded().loaded) {
        // One module per process. The pinned module is never replaced, so a
        // commit that arrives for the other kind is reported as the restart it
        // is: dropping the request here would let a recovery attempt look as
        // though it had been honoured.
        RuntimeSelection selection = recorded();
        if (resolved.nativeNotApplied) {
            // A native request that fell back to Standard is not a restart
            // question - it is the same kind that is already pinned - but the
            // reason why native is not running has to survive the commit.
            selection.requested = resolved.requested;
            selection.nativeNotApplied = true;
            selection.runtimeUnavailable = true;
            if (!resolved.reason.isEmpty()) {
                selection.reason = resolved.reason;
            }
            return selection;
        }
        if (selection.kind != resolved.kind) {
            selection.requested = resolved.requested;
            selection.restartRequired = true;
            selection.reason = restartReason(selection.kind, resolved.requested);
        }
        return selection;
    }
    // Nothing validated travels with this selection, so there is no descriptor
    // to trust and nothing to load. The refusal names the condition rather than
    // letting a partial selection reach the loader.
    if (!resolved.resolvedBundle.has_value()) {
        RuntimeSelection refused = resolved;
        refused.loaded = false;
        refused.runtimeUnavailable = true;
        refused.reason = resolved.reason.isEmpty()
                             ? translated(QT_TRANSLATE_NOOP(
                                   "RuntimeSelection", "No validated mpv runtime was resolved."))
                             : resolved.reason;
        return refused;
    }
    const Bundle& bundle = *resolved.resolvedBundle;
    // The display fields and the descriptor are one selection. A caller that
    // changes a path without the descriptor is refused here, before the loader
    // is asked, rather than half-trusted.
    if (resolved.kind != bundle.kind || resolved.root != bundle.root ||
        resolved.modulePath != bundle.modulePath || resolved.descriptorId != bundle.descriptorId) {
        RuntimeSelection forged = resolved;
        forged.loaded = false;
        forged.runtimeUnavailable = true;
        forged.reason = translated(QT_TRANSLATE_NOOP(
                            "RuntimeSelection",
                            "The resolved selection does not match its validated bundle."));
        return forged;
    }
    // Revalidate the exact bytes the descriptor was built from: a bundle that
    // was replaced or edited since resolve is refused by name here instead of
    // being loaded stale. This is the check the resolve half deliberately skips.
    QString reason;
    if (!validateBundleCached(bundle, &reason)) {
        // A bundle that stops validating is dropped from the cache as well, so
        // the next start re-hashes it rather than trusting a stale verdict.
        invalidateBundleValidation(bundle.root);
        RuntimeSelection stale = resolved;
        stale.loaded = false;
        stale.runtimeUnavailable = true;
        stale.reason = reason.isEmpty()
                           ? translated(QT_TRANSLATE_NOOP(
                                 "RuntimeSelection",
                                 "The resolved mpv runtime no longer validates."))
                           : reason;
        return stale;
    }
    // Load exactly the descriptor that was validated: its dependency
    // directories and protocol travel with it, so nothing the resolve checked is
    // reconstructed or lost on the way here.
    if (!MpvApi::instance().load(bundle.toMpvBundleDescriptor(), &reason)) {
        // This is the "something broke in the flow" case: the bundle passed its
        // checks but the loader refused it, so the remembered verdict is dropped
        // and the next start proves the bytes again instead of trusting them.
        invalidateBundleValidation(bundle.root);
        RuntimeSelection failed = resolved;
        failed.loaded = false;
        failed.runtimeUnavailable = true;
        failed.reason = reason.isEmpty() ? MpvApi::instance().error() : reason;
        return failed;
    }

    RuntimeSelection selection = resolved;
    // The display fields are re-derived from the descriptor that actually
    // loaded, so they cannot drift from what the loader was given.
    selection.kind = bundle.kind;
    selection.root = bundle.root;
    selection.modulePath = bundle.modulePath;
    selection.descriptorId = bundle.descriptorId;
    selection.version = bundle.version;
    selection.loaded = true;
    selection.apiVersion = MpvApi::instance().apiVersion();
    // A refused native request keeps its named reason across the fallback, so
    // the surface can still say why the loaded kind is not the one asked for;
    // an ordinary successful selection has nothing left to explain.
    if (!selection.nativeNotApplied) {
        selection.reason.clear();
    }
    recorded() = selection;
    return selection;
}

RuntimeSelection selectRuntime(RuntimeChoice choice, const QString& hint) {
    if (recorded().loaded) {
        // One module per process. A different choice is a restart, never a swap.
        const BundleKind wanted =
            choice == RuntimeChoice::Native ? BundleKind::Native : BundleKind::Standard;
        RuntimeSelection selection = recorded();
        if (selection.kind != wanted) {
            selection.requested = choice;
            selection.restartRequired = true;
            selection.reason = restartReason(selection.kind, choice);
        }
        return selection;
    }
    // A descriptor the loader itself rejects is reported as-is: loading a
    // second module to replace it would be the hot swap the loader refuses.
    // The resolution stays local: provisioning may download, and that belongs to
    // the recovery path's `resolveRuntime()` on the pool, never to a GUI-thread
    // selection that a startup is waiting on.
    return commitRuntime(resolveRuntimeLocal(choice, hint));
}

RuntimeSelection selectRuntime() { return selectRuntime(storedRuntimeChoice()); }

RuntimeSelection selectStandardRuntime(const QString& hint) {
    return selectRuntime(RuntimeChoice::Standard, hint);
}

RuntimeSelection selectStartupRuntime(const QString& hint) {
    return selectRuntime(storedRuntimeChoice(), hint);
}

const RuntimeSelection& currentSelection() { return recorded(); }

bool runtimeSelected() { return recorded().loaded; }

QString runtimeStatusText() {
    const RuntimeSelection& selection = recorded();
    if (!selection.loaded) {
        return selection.reason.isEmpty() ? translated(QT_TRANSLATE_NOOP(
                                                "RuntimeSelection", "No mpv runtime is loaded."))
                                          : selection.reason;
    }
    QString status = bundleKindName(selection.kind);
    if (!selection.version.isEmpty()) {
        status += QLatin1Char(' ') + selection.version;
    }
    if (selection.nativeNotApplied || selection.restartRequired) {
        status += QStringLiteral(" — ") +
                  (selection.reason.isEmpty() ? translated(QT_TRANSLATE_NOOP(
                                                    "RuntimeSelection", "native not applied"))
                                               : selection.reason);
    }
    return status;
}

QString runtimeUnavailablePlaybackText(const QString& reason) {
    const QString detail = reason.isEmpty() ? runtimeStatusText() : reason;
    return translated(QT_TRANSLATE_NOOP(
               "RuntimeSelection", "Playback runtime unavailable — %1"))
        .arg(detail);
}

bool beginRecovery() {
    if (recorded().loaded) {
        return false;
    }
    bool expected = false;
    return g_recoveryInFlight.compare_exchange_strong(expected, true);
}

void endRecovery() { g_recoveryInFlight.store(false); }

bool recoveryInFlight() { return g_recoveryInFlight.load(); }

}  // namespace native
}  // namespace mplayer
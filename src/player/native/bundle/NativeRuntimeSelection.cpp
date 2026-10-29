#include "NativeRuntimeSelection.hpp"

#include "MpvApi.hpp"
#include "NativePeImage.hpp"

#include <backend/Runtime.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QtGlobal>

#include <algorithm>
#include <atomic>
#include <optional>

namespace mplayer {
namespace native {
namespace {

RuntimeSelection& recorded() {
    static RuntimeSelection selection;
    return selection;
}

std::atomic_bool g_recoveryInFlight{false};
/// The one resolution the recovery path currently owns. Zero while no recovery
/// holds one. The lease, not this, is what a recovery releases, which is what
/// keeps a suppressed completion from leaving it latched forever.
std::atomic<quint64> g_recoveryResolution{0};

/// The one resolution a process may run at a time, plus its owner and the
/// waiters that asked to hear the terminal state. GUI thread only: the atomics
/// below are a single reader's consistency aid, not a lock.
std::atomic<quint64> g_resolutionId{0};
/// Issues lease ids and never repeats one. A counter that restarted at one
/// would hand a replacement resolution the identity the lease it replaced still
/// carries, and releasing the stale lease would then release the live one.
std::atomic<quint64> g_resolutionCounter{0};

/// One waiter for `whenRuntimeResolutionFinishes`. The context is held weakly
/// and the callback is copied once: a caller whose context dies mid-resolution
/// is discarded rather than delivered to a dead widget.
struct ResolutionWaiter {
    ResolutionWaiter* next = nullptr;
    QPointer<QObject> context;
    std::function<void()> callback;
    /// 0 for a waiter that fires on the next turn even though nothing is in
    /// flight (an already-settled registration), otherwise the id it waits for.
    quint64 id = 0;
};

ResolutionWaiter* g_waiters = nullptr;

/// Takes `waiter` back out of the shared list. Called for a waiter that
/// resolves instantly, so a registration made while nothing is in flight never
/// clears the waiters a later resolution still owes.
void unlinkWaiter(ResolutionWaiter* waiter) {
    ResolutionWaiter** link = &g_waiters;
    while (*link != nullptr) {
        if (*link == waiter) {
            *link = waiter->next;
            return;
        }
        link = &(*link)->next;
    }
}

/// Publishes each waiter once, in registration order, through a queued call so
/// a registration made from inside another waiter's delivery still runs on its
/// own turn instead of re-entering the caller that registered it. Waiter nodes
/// are plain allocations with no owner, so the last one out frees them.
void deliverWaiters(ResolutionWaiter* first) {
    while (first != nullptr) {
        ResolutionWaiter* const waiter = first;
        first = waiter->next;
        QObject* const context = waiter->context.data();
        if (context != nullptr && waiter->callback) {
            std::function<void()> callback = std::move(waiter->callback);
            QMetaObject::invokeMethod(context, [callback] { callback(); }, Qt::QueuedConnection);
        }
        delete waiter;
    }
}

/// The one lookup with this file's context. Every literal it is handed must be
/// wrapped in QT_TRANSLATE_NOOP: that macro is what lupdate reads, and at run
/// time it is the literal itself.
QString translated(const char* message) {
    return QCoreApplication::translate("RuntimeSelection", message);
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

#ifdef Q_OS_LINUX
QString nativePlatformRefusal() {
    return translated(QT_TRANSLATE_NOOP(
        "RuntimeSelection", "The private Native runtime is not supported on Linux."));
}
#endif

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

/// Every candidate module, described in turn, so one corrupt DLL cannot mask a
/// valid one beside it: a candidate that fails validation is named and the
/// search continues. The descriptor describes what is already on disk.
RuntimeSelection describePackagedStandard(const QString& hint) {
#ifdef Q_OS_LINUX
#if defined(MIKAI_LINUX_BUNDLE_MPV) && MIKAI_LINUX_BUNDLE_MPV
    Q_UNUSED(hint);
#ifndef MIKAI_LINUX_MPV_MODULE_RELATIVE
#define MIKAI_LINUX_MPV_MODULE_RELATIVE "mpv/libmpv.so.2"
#endif
    const QString module =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QString::fromUtf8(MIKAI_LINUX_MPV_MODULE_RELATIVE));
    QString reason;
    const std::optional<Bundle> bundle = describeStandardRuntimeCached(module, &reason);
    if (bundle.has_value() && validateBundleCached(*bundle, &reason)) {
        return describe(*bundle, RuntimeChoice::Standard);
    }
    return unavailable(
        RuntimeChoice::Standard,
        translated(QT_TRANSLATE_NOOP("RuntimeSelection", "The bundled mpv runtime is unusable: %1"))
            .arg(reason.isEmpty() ? module : reason));
#elif defined(MIKAI_LINUX_SYSTEM_MPV_MODULE)
    if (!hint.isEmpty()) {
        const QFileInfo hinted(hint);
        if (hinted.isFile()) {
            QString reason;
            const std::optional<Bundle> bundle = describeSystemRuntimeCached(hint, &reason);
            if (bundle.has_value()) return describe(*bundle, RuntimeChoice::Standard);
            return unavailable(
                RuntimeChoice::Standard,
                translated(QT_TRANSLATE_NOOP("RuntimeSelection", "The selected system mpv runtime is unusable: %1"))
                    .arg(reason.isEmpty() ? hint : reason));
        }
    }
    const QString module = QString::fromUtf8(MIKAI_LINUX_SYSTEM_MPV_MODULE);
    QString reason;
    const std::optional<Bundle> bundle = describeSystemRuntimeCached(module, &reason);
    if (bundle.has_value()) return describe(*bundle, RuntimeChoice::Standard);
    return unavailable(
        RuntimeChoice::Standard,
        translated(QT_TRANSLATE_NOOP("RuntimeSelection", "The system mpv runtime is unusable: %1"))
            .arg(reason.isEmpty() ? module : reason));
#elif defined(MIKAI_LINUX_BUNDLE_MPV)
    Q_UNUSED(hint);
    return unavailable(
        RuntimeChoice::Standard,
        translated(QT_TRANSLATE_NOOP(
            "RuntimeSelection", "The configured Linux system mpv module path is missing.")));
#endif
#endif
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
        if (bundle.has_value() && validateBundleCached(*bundle, &reason)) {
            return describe(*bundle, RuntimeChoice::Standard);
        }
        rejections.append(QStringLiteral("%1: %2").arg(module, reason));
    }
    return unavailable(RuntimeChoice::Standard,
                       translated(QT_TRANSLATE_NOOP(
                           "RuntimeSelection", "No usable mpv runtime: %1"))
                           .arg(rejections.join(QStringLiteral("; "))));
}

/// Resolves an explicit developer/probe hint first, then the packaged mpv
/// module. No installed cache or arbitrary DLL is considered.
RuntimeSelection resolveStandardLocal(const QString& hint) {
    return describePackagedStandard(hint);
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
/// the resolver that answers the fallback, so both startup and recovery stay
/// local and never download a runtime.
template <typename StandardResolver>
RuntimeSelection resolveWith(RuntimeChoice choice, StandardResolver standard, const QString& hint) {
    if (choice != RuntimeChoice::Native) {
        return standard(hint);
    }
#ifdef Q_OS_LINUX
    return unavailable(RuntimeChoice::Native, nativePlatformRefusal());
#endif
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

/// The one system directory Windows itself owns, which is the only place a
/// supporting loader may come from: nothing is copied into the bundle, no
/// dependency is added, and no folder beside the module is searched.
QString windowsSystemDirectory() {
    const QString root = qEnvironmentVariable("SystemRoot");
    const QString system32 =
        QDir::cleanPath((root.isEmpty() ? QLatin1String("C:/Windows") : root) +
                        QLatin1String("/System32"));
    return QFileInfo(system32).isDir() ? system32 : QString();
}

/// The loader the module's own import table asks for and the Windows system
/// exceptions already name, so a module that declares no such dependency is
/// never asked about one it does not have. Read from the image, so a synthetic
/// fixture with no import table yields an empty answer rather than a guess.
const QStringList& kSystemDriverImports() {
    static const QStringList required = {QStringLiteral("vulkan-1.dll")};
    return required;
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

RuntimeChoice storedRuntimeChoice(QString* reason) {
    QSettings settings;
    settings.beginGroup(runtimeChoiceSettingsGroup());
    const QString stored = settings.value(runtimeChoiceSettingsKey()).toString();
    settings.endGroup();
    if (reason != nullptr) {
        reason->clear();
    }
    const RuntimeChoice choice = runtimeChoiceFromString(stored);
#ifdef Q_OS_LINUX
    if (choice == RuntimeChoice::Native) {
        if (reason != nullptr) {
            *reason = nativePlatformRefusal();
        }
        return RuntimeChoice::Standard;
    }
#endif
    return choice;
}

bool storeRuntimeChoice(RuntimeChoice choice, QString* reason) {
#ifdef Q_OS_LINUX
    if (choice == RuntimeChoice::Native) {
        if (reason != nullptr) {
            *reason = nativePlatformRefusal();
        }
        return false;
    }
#endif
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
#ifdef Q_OS_LINUX
    // Restarting cannot make the Windows-only runtime available.
    if (choice == RuntimeChoice::Native) {
        return false;
    }
#endif
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
    return resolveWith(choice, &resolveStandardLocal, hint);
}
RuntimeSelection resolveStoredRuntime(const QString& hint) {
    QString normalizationReason;
    const RuntimeChoice choice = storedRuntimeChoice(&normalizationReason);
    RuntimeSelection selection = resolveRuntime(choice, hint);
    if (!normalizationReason.isEmpty()) {
        selection.nativeNotApplied = true;
        selection.runtimeUnavailable = true;
        if (!selection.resolvedBundle.has_value() && !selection.reason.isEmpty()) {
            selection.reason = QStringLiteral("%1; %2").arg(normalizationReason, selection.reason);
        } else {
            selection.reason = normalizationReason;
        }
    }
    return selection;
}
RuntimeSelection resolveNamedBundle(const QString& root) {
    const QFileInfo rootInfo(root);
    if (!rootInfo.isAbsolute()) {
        return unavailable(
            RuntimeChoice::Standard,
            translated(QT_TRANSLATE_NOOP("RuntimeSelection",
                                         "The named mpv bundle path must be absolute.")));
    }
    if (!rootInfo.isDir()) {
        return unavailable(
            RuntimeChoice::Standard,
            translated(QT_TRANSLATE_NOOP("RuntimeSelection",
                                         "The named mpv bundle directory does not exist.")));
    }
    QString reason;
    const std::optional<Bundle> bundle = readBundle(root, &reason);
    if (!bundle.has_value()) {
        return unavailable(
            RuntimeChoice::Standard,
            reason.isEmpty()
                ? translated(QT_TRANSLATE_NOOP("RuntimeSelection",
                                               "The named mpv bundle has no readable manifest."))
                : reason);
    }
#ifdef Q_OS_LINUX
    if (bundle->kind == BundleKind::Native) {
        return unavailable(RuntimeChoice::Native, nativePlatformRefusal());
    }
#endif
    if (!validateBundleCached(*bundle, &reason)) {
        return unavailable(
            bundle->kind == BundleKind::Native ? RuntimeChoice::Native
                                               : RuntimeChoice::Standard,
            reason.isEmpty()
                ? translated(QT_TRANSLATE_NOOP(
                      "RuntimeSelection", "The named mpv bundle does not validate against its files."))
                : reason);
    }

    const RuntimeChoice choice =
        bundle->kind == BundleKind::Native ? RuntimeChoice::Native : RuntimeChoice::Standard;
    return describe(*bundle, choice);
}

RuntimeSelection commitRuntime(const RuntimeSelection& resolved) {
#ifdef Q_OS_LINUX
    if (resolved.requested == RuntimeChoice::Native || resolved.kind == BundleKind::Native ||
        (resolved.resolvedBundle.has_value() &&
         resolved.resolvedBundle->kind == BundleKind::Native)) {
        RuntimeSelection refused = unavailable(RuntimeChoice::Native, nativePlatformRefusal());
        if (recorded().loaded) {
            refused = recorded();
            refused.requested = RuntimeChoice::Native;
            refused.runtimeUnavailable = true;
            refused.nativeNotApplied = true;
            refused.restartRequired = false;
            refused.reason = nativePlatformRefusal();
        } else {
            refused.kind = resolved.kind;
        }
        recorded() = refused;
        return refused;
    }
#endif
    if (recorded().loaded) {
        // One module per process. The pinned module is never replaced, so a
        // commit that arrives for the other kind is reported as the restart it
        // is: dropping the request here would let a recovery attempt look as
        // though it had been honoured.
        RuntimeSelection selection = recorded();
        if (!resolved.loaded &&
            (resolved.runtimeUnavailable || !resolved.reason.isEmpty())) {
            // A terminal refusal must remain visible without replacing the
            // loader's pinned-module truth. This also covers a Standard request
            // whose resolution failed while a Native module remains loaded.
            selection.requested = resolved.requested;
            selection.nativeNotApplied = resolved.nativeNotApplied;
            selection.runtimeUnavailable = true;
            if (!resolved.reason.isEmpty()) {
                selection.reason = resolved.reason;
            }
            recorded() = selection;
            return selection;
        }
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
            recorded() = selection;
            return selection;
        }
        selection.requested = resolved.requested;
        selection.restartRequired = selection.kind != resolved.kind;
        selection.nativeNotApplied = false;
        selection.runtimeUnavailable = false;
        selection.reason = selection.restartRequired
                               ? restartReason(selection.kind, resolved.requested)
                               : QString();
        recorded() = selection;
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
        recorded() = refused;
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
        recorded() = forged;
        return forged;
    }
    // Resolution proved the exact bytes off-thread. Commit only checks that its
    // exact descriptor and stat-tree identity still match that proof: a cold or
    // changed cache must never fall back to hashing payloads on the GUI thread.
    QString reason;
    if (!isBundleValidationCurrent(bundle, &reason)) {
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
        recorded() = stale;
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
        recorded() = failed;
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
#ifdef Q_OS_LINUX
    if (choice == RuntimeChoice::Native) {
        return commitRuntime(unavailable(choice, nativePlatformRefusal()));
    }
#endif
    if (recorded().loaded) {
        // One module per process. A different choice is a restart, never a swap.
        const BundleKind wanted =
            choice == RuntimeChoice::Native ? BundleKind::Native : BundleKind::Standard;
        RuntimeSelection selection = recorded();
        selection.requested = choice;
        selection.restartRequired = selection.kind != wanted;
        selection.nativeNotApplied = false;
        selection.runtimeUnavailable = false;
        selection.reason = selection.restartRequired
                               ? restartReason(selection.kind, choice)
                               : QString();
        recorded() = selection;
        return selection;
    }
    // A descriptor the loader itself rejects is reported as-is: loading a
    // second module to replace it would be the hot swap the loader refuses.
    // Resolution uses only the explicit hint and packaged mpv module; it never
    // provisions a runtime or performs network access.
    return commitRuntime(resolveWith(choice, &resolveStandardLocal, hint));
}

RuntimeSelection selectRuntime() {
    return selectStartupRuntime();
}

RuntimeSelection selectStandardRuntime(const QString& hint) {
    return selectRuntime(RuntimeChoice::Standard, hint);
}

RuntimeSelection selectStartupRuntime(const QString& hint) {
    if (recorded().loaded) {
        QString normalizationReason;
        const RuntimeChoice choice = storedRuntimeChoice(&normalizationReason);
        if (normalizationReason.isEmpty()) {
            return selectRuntime(choice, hint);
        }
        RuntimeSelection normalized;
        normalized.requested = choice;
        normalized.nativeNotApplied = true;
        normalized.runtimeUnavailable = true;
        normalized.reason = normalizationReason;
        return commitRuntime(normalized);
    }
    return commitRuntime(resolveStoredRuntime(hint));
}

namespace {

/// Whether the module carries the fork's `animejanai` filter name. A stock mpv
/// has no such string, so it separates the fork from any host runtime without
/// depending on a byte digest. Linux modules cannot be compared by digest: the
/// install step rewrites their RUNPATH with patchelf, so the same archive yields
/// a different file on every build host.
bool moduleCarriesAnimeJanaiFilter(const QString& modulePath) {
    QFile module(modulePath);
    if (!module.open(QIODevice::ReadOnly)) return false;
    static const QByteArray marker = QByteArrayLiteral("animejanai");
    QByteArray carry;
    while (!module.atEnd()) {
        const QByteArray chunk = module.read(1 << 20);
        if (chunk.isEmpty()) break;
        carry += chunk;
        if (carry.contains(marker)) return true;
        // Keep a marker-length tail so a match split across chunks is not lost.
        if (carry.size() > marker.size()) carry = carry.right(marker.size());
    }
    return carry.contains(marker);
}

}  // namespace

bool isAnimeJanaiForkRuntime(const RuntimeSelection& selection) {
    constexpr qint64 kForkModuleBytes = 122146816;
    constexpr quint64 kForkApiVersion = 131077;
    constexpr auto kForkModuleSha256 = "90de8f89fa1421eaec510e5cfe11de75846b445489864220ec811fd0f08b4649";

    if (!selection.loaded || selection.kind != BundleKind::Standard ||
        !selection.resolvedBundle.has_value()) {
        return false;
    }
#if defined(Q_OS_UNIX)
    // The Unix fork builds report 2.6; the app only needs the 2.x major it was
    // compiled against, which MpvApi::load already enforces.
    if (selection.apiVersion < kForkApiVersion) {
        return false;
    }
#else
    if (selection.apiVersion != kForkApiVersion) {
        return false;
    }
#endif

    const Bundle& bundle = *selection.resolvedBundle;
    if (bundle.kind != BundleKind::Standard || selection.modulePath != bundle.modulePath) {
        return false;
    }

    for (const BundleFile& file : bundle.files) {
        if (QDir::cleanPath(QDir(bundle.root).filePath(file.path)) != bundle.modulePath) {
            continue;
        }
        if (file.bytes == kForkModuleBytes &&
            file.sha256 == QLatin1String(kForkModuleSha256)) {
            return true;
        }
#if defined(Q_OS_UNIX)
        return moduleCarriesAnimeJanaiFilter(bundle.modulePath);
#else
        return false;
#endif
    }
    return false;
}

const RuntimeSelection& currentSelection() { return recorded(); }

bool runtimeSelected() { return recorded().loaded; }

bool runtimeResolutionPending() { return g_resolutionId.load() != 0; }

QString runtimePendingPlaybackText() {
    return translated(QT_TRANSLATE_NOOP(
        "RuntimeSelection", "Checking the installed mpv runtime\u2026"));
}

void whenRuntimeResolutionFinishes(QObject* context, const std::function<void()>& callback) {
    auto* waiter = new ResolutionWaiter();
    waiter->context = context;
    waiter->callback = callback;
    const quint64 id = g_resolutionId.load();
    waiter->id = id;
    waiter->next = g_waiters;
    g_waiters = waiter;
    if (id != 0) {
        // A resolution is in flight, so this waiter waits for its terminal
        // state with the rest of them.
        return;
    }
    // Nothing is in flight, so the recorded state is already terminal and this
    // waiter belongs to nobody. It is unlinked and published on its own turn
    // rather than synchronously: a caller that just registered must not be
    // re-entered by its own registration, and a verdict snapshotted before the
    // queued call arrives would go stale.
    unlinkWaiter(waiter);
    deliverWaiters(waiter);
}

ResolutionLease beginResolution() {
    quint64 current = g_resolutionId.load();
    for (;;) {
        if (current != 0 || recorded().loaded) return ResolutionLease();
        const quint64 next = g_resolutionCounter.fetch_add(1) + 1;
        if (g_resolutionId.compare_exchange_weak(current, next)) return ResolutionLease(next);
    }
}

void endResolution(const ResolutionLease& lease) noexcept {
    if (!lease) return;
    const quint64 ended = lease.id();
    if (g_resolutionId.load() != ended) return;  // Not this lease's resolution.
    g_resolutionId.store(0);
    ResolutionWaiter* toDeliver = g_waiters;
    g_waiters = nullptr;
    deliverWaiters(toDeliver);
}

QString runtimeStatusText() {
    const RuntimeSelection& selection = recorded();
    if (!selection.loaded) {
        if (runtimeResolutionPending()) return runtimePendingPlaybackText();
        return selection.reason.isEmpty() ? translated(QT_TRANSLATE_NOOP(
                                                "RuntimeSelection", "No mpv runtime is loaded."))
                                          : selection.reason;
    }
    QString status = bundleKindName(selection.kind);
    if (!selection.version.isEmpty()) {
        status += QLatin1Char(' ') + selection.version;
    }
    if (selection.nativeNotApplied || selection.restartRequired ||
        selection.runtimeUnavailable) {
        status += QStringLiteral(" — ") +
                  (selection.reason.isEmpty() ? translated(QT_TRANSLATE_NOOP(
                                                    "RuntimeSelection", "native not applied"))
                                               : selection.reason);
    }
    return status;
}

QString runtimeUnavailablePlaybackText(const QString& reason) {
    const QString detail = reason.isEmpty() ? runtimeStatusText() : reason;
    if (runtimeResolutionPending()) {
        // Still resolving. Reporting the runtime as unavailable here would be a
        // named failure for a module that has not been refused yet.
        return translated(QT_TRANSLATE_NOOP("RuntimeSelection",
                                           "Playback runtime pending — %1"))
            .arg(runtimePendingPlaybackText());
    }
    return translated(QT_TRANSLATE_NOOP(
               "RuntimeSelection", "Playback runtime unavailable — %1"))
        .arg(detail);
}

QStringList moduleSystemDriverImports(const QString& modulePath) {
    QStringList required;
    if (modulePath.trimmed().isEmpty()) return required;
    const QString canonical = QFileInfo(modulePath).canonicalFilePath();
    if (canonical.isEmpty()) return required;
    QFile image(canonical);
    if (!image.open(QIODevice::ReadOnly)) return required;
    const PeImageInfo info = inspectPeImage(image.readAll());
    if (!info.ok) return required;
    const QStringList exceptions = systemImportExceptions();
    for (const QString& imported : info.imports + info.delayImports) {
        if (!exceptions.contains(imported, Qt::CaseInsensitive)) continue;
        if (!kSystemDriverImports().contains(imported, Qt::CaseInsensitive)) continue;
        if (required.contains(imported, Qt::CaseInsensitive)) continue;
        required.append(imported);
    }
    std::sort(required.begin(), required.end());
    return required;
}

QString runtimeLoaderDependencyRefusal(const QString& modulePath) {
    const QStringList required = moduleSystemDriverImports(modulePath);
    if (required.isEmpty()) return QString();
    const QString system32 = windowsSystemDirectory();
    if (system32.isEmpty()) {
        // Without the system directory there is no trusted source for the
        // loader, so the module cannot be honoured and says so by name rather
        // than being loaded against something nobody can account for.
        return translated(QT_TRANSLATE_NOOP("RuntimeSelection",
                                            "mpv runtime unavailable: the Windows system directory "
                                            "could not be determined, so %1 cannot be checked for "
                                            "the %2 it imports"))
            .arg(QDir::toNativeSeparators(modulePath), required.join(QLatin1String(", ")));
    }
    QStringList missing;
    for (const QString& name : required) {
        if (!QFileInfo::exists(QDir(system32).filePath(name))) missing.append(name);
    }
    if (missing.isEmpty()) return QString();
    return translated(QT_TRANSLATE_NOOP(
               "RuntimeSelection",
               "mpv runtime unavailable: %1 requires the Windows Vulkan loader (%2), "
               "which is not installed on this PC; install your graphics driver so the "
               "module can be loaded"))
        .arg(QDir::toNativeSeparators(modulePath), missing.join(QLatin1String(", ")));
}

bool beginRecovery() {
    // The resolution lease is the owner, so a recovery whose owner is destroyed
    // still releases the one resolution: the latch records the attempt, it
    // does not hold it. A recovery that finds a module pinned or another
    // resolution running starts nothing and holds nothing.
    const ResolutionLease lease = beginResolution();
    if (!lease) {
        return false;
    }
    g_recoveryResolution.store(lease.id());
    g_recoveryInFlight.store(true);
    return true;
}

void endRecovery() {
    // Releases the resolution this recovery took, if it still holds it. A
    // recovery whose delivery Runner suppressed, or whose owner went away, still
    // ends here, so the one resolution is never latched forever. A newer
    // resolution that replaced it is left alone, because releasing is by id.
    const ResolutionLease lease(g_recoveryResolution.exchange(0));
    endResolution(lease);
    g_recoveryInFlight.store(false);
}

bool recoveryInFlight() { return g_recoveryInFlight.load(); }

}  // namespace native
}  // namespace mplayer
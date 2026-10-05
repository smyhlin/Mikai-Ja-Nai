
#include "MpvApi.hpp"
#include "ProbePicture.hpp"
#include "VideoSurface.hpp"
#include "native/bundle/NativeBundle.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"

#include <backend/Runtime.hpp>

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <optional>

using mplayer::MpvApi;
using mplayer::VideoSurface;
using mplayer::native::Bundle;
using mplayer::native::BundleKind;
using mplayer::native::RuntimeChoice;
using mplayer::native::RuntimeSelection;

namespace {

int failures = 0;
/// Set when a fact could not be established on this machine. Reported as a skip
/// after every observation, never instead of one.
bool inconclusive = false;

void check(bool ok, const QString& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.toUtf8().constData());
    std::fflush(stdout);
    if (!ok) ++failures;
}

void note(const QString& text) {
    std::printf("     %s\n", qPrintable(text));
    std::fflush(stdout);
}

void reportInconclusive(const QString& text) {
    inconclusive = true;
    std::printf("SKIP %s\n", qPrintable(text));
    std::fflush(stdout);
}

/// Every file's relative path, size and modification time, in a stable order:
/// the shape of "not one byte changed", which is what a reference install has
/// to look like after recovery.
QString treeFingerprint(const QString& root) {
    QStringList rows;
    const QDir base(root);
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info(it.next());
        rows.append(QStringLiteral("%1|%2|%3")
                        .arg(base.relativeFilePath(info.absoluteFilePath()))
                        .arg(info.size())
                        .arg(info.lastModified().toMSecsSinceEpoch()));
    }
    rows.sort();
    return rows.join(QLatin1Char('\n'));
}

/// The real libmpv this run is proved against. Named by the environment so the
/// probe exercises the module the machine actually ships.
QString stockModule() {
    const QString named = qEnvironmentVariable("MIKAI_MPV_MODULE");
    return named.isEmpty() || !QFileInfo(named).isFile() ? QString{}
                                                         : QFileInfo(named).canonicalFilePath();
}

bool copyTree(const QString& from, const QString& to) {
    QDir().mkpath(to);
    const QDir source(from);
    QDirIterator files(from, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        const QString target = QDir(to).filePath(source.relativeFilePath(path));
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) return false;
        QFile::remove(target);
        if (!QFile::copy(path, target)) return false;
    }
    return true;
}

/// ── the shared invariants ────────────────────────────────────────────────

/// A runtime that could not be produced is never a silent result: it names
/// what it rejected or what it did not find, and claims nothing about playback.
void checkNamedFailure(const RuntimeSelection& selection) {
    check(!selection.loaded, QStringLiteral("no module is loaded"));
    check(selection.runtimeUnavailable,
          QStringLiteral("a runtime that could not be produced is reported unavailable"));
    check(!selection.reason.isEmpty(),
          QStringLiteral("a runtime that could not be produced is named: %1").arg(selection.reason));
    check(!selection.nativeNotApplied,
          QStringLiteral("a Standard recovery never claims a native runtime"));
    check(mplayer::native::runtimeStatusText().contains(selection.reason),
          QStringLiteral("the status a user reads carries the same reason"));
    check(!mplayer::native::runtimeUnavailablePlaybackText(selection.reason).isEmpty(),
          QStringLiteral("playback surfaces the same named reason"));
}

/// Recovery published a bundle, the selection resolved that published bundle,
/// and the commit loaded exactly the module it validated.
void checkPublishedAndLoaded(const RuntimeSelection& selection, const QString& publishedRoot) {
    check(selection.loaded,
          QStringLiteral("the published bundle's module loaded: %1").arg(selection.reason));
    check(selection.kind == BundleKind::Standard, QStringLiteral("the loaded module is Standard"));
    check(selection.apiVersion > 0,
          QStringLiteral("the loaded module reports the API version it was loaded against (%1)")
              .arg(selection.apiVersion));
    check(QDir::cleanPath(selection.root).compare(QDir::cleanPath(publishedRoot),
                                                  Qt::CaseInsensitive) == 0,
          QStringLiteral("the loaded module came from the published bundle, not the stock tree (%1)")
              .arg(selection.root));
    check(QFileInfo::exists(QDir(publishedRoot).filePath(QStringLiteral("bundle.json"))),
          QStringLiteral("the published directory carries its own manifest"));
    check(mplayer::native::runtimeSelected(), QStringLiteral("the process has pinned one module"));

    // The loader was handed the resolved closure, not a descriptor rebuilt from
    // the display fields: a rebuilt one would lose the dependency directories
    // that make the module loadable beside its own tree.
    check(selection.resolvedBundle.has_value(),
          QStringLiteral("the committed selection carries the bundle it validated"));
    if (!selection.resolvedBundle.has_value()) return;
    const mplayer::MpvBundle loaded = MpvApi::instance().bundle();
    check(loaded.root == selection.resolvedBundle->root,
          QStringLiteral("the loader was given the resolved root"));
    check(loaded.modulePath == selection.resolvedBundle->modulePath,
          QStringLiteral("the loader was given the resolved module"));
    check(loaded.descriptorId == selection.resolvedBundle->descriptorId,
          QStringLiteral("the loader was given the resolved descriptor identity"));
    check(loaded.deps == selection.resolvedBundle->toMpvBundleDescriptor().deps,
          QStringLiteral("the loader was given the resolved dependency closure"));
}

/// The published module is a working core, not a file that merely validated: a
/// real frame from the surface's own framebuffer is the only evidence that
/// counts here.
void checkRendersRealFrames(VideoSurface& surface) {
    surface.resize(640, 360);
    surface.show();
    if (!surface.waitForContext(8000)) {
        ++failures;
        return;
    }
    QString mediaError;
    QObject::connect(&surface, &VideoSurface::errorOccurred,
                     [&mediaError](const QString& message) { mediaError = message; });
    if (!surface.open(QStringLiteral("av://lavfi:testsrc2=size=320x180:rate=25"))) {
        ++failures;
        reportInconclusive(QStringLiteral("the published module refused to create a core"));
        return;
    }

    mplayer::probes::PlaybackSampler sampler(surface);
    QElapsedTimer waited;
    waited.start();
    while (!sampler.complete() && waited.elapsed() < 20000) {
        QCoreApplication::processEvents();
        QThread::msleep(40);
        sampler.tick();
    }
    check(sampler.complete(),
          QStringLiteral("the published core rendered on the media clock (%1 s)").arg(surface.position()));
    if (!sampler.complete()) return;

    const mplayer::probes::PlaybackSample& earlier = sampler.earlier();
    const mplayer::probes::PlaybackSample& later = sampler.later();
    check(mediaError.isEmpty(), QStringLiteral("playback reported no media error (%1)").arg(mediaError));
    check(later.contextReady, QStringLiteral("the render context is ready"));
    check(later.hasVideo, QStringLiteral("the core reports a video track"));
    check(later.videoWidth == 320 && later.videoHeight == 180,
          QStringLiteral("the source geometry is real (%1x%2)")
              .arg(later.videoWidth)
              .arg(later.videoHeight));
    check(!later.framebuffer.isNull() && later.framebuffer.width() > 0 &&
              later.framebuffer.height() > 0,
          QStringLiteral("the surface framebuffer was captured (%1x%2)")
              .arg(later.framebuffer.width())
              .arg(later.framebuffer.height()));
    const QSize expected(qRound(later.surfaceSize.width() * later.devicePixelRatio),
                         qRound(later.surfaceSize.height() * later.devicePixelRatio));
    check(qAbs(later.framebuffer.width() - expected.width()) <= 1 &&
              qAbs(later.framebuffer.height() - expected.height()) <= 1,
          QStringLiteral("the frame is the surface's own, not a widget grab (%1x%2 vs %3x%4)")
              .arg(later.framebuffer.width())
              .arg(later.framebuffer.height())
              .arg(expected.width())
              .arg(expected.height()));
    check(earlier.position > 0.0 && later.position > earlier.position,
          QStringLiteral("the media clock advanced (%1 -> %2 s)")
              .arg(earlier.position, 0, 'f', 2)
              .arg(later.position, 0, 'f', 2));

    check(later.framebuffer.save(QStringLiteral("runtime_recovery_published.png")),
          QStringLiteral("the rendered frame saves to runtime_recovery_published.png"));
    const mplayer::probes::Picture picture =
        mplayer::probes::measurePicture(later.framebuffer, later.videoWidth, later.videoHeight);
    if (picture.isMostlyBlack()) {
        check(false, QStringLiteral("the known testsrc2 fixture rendered a black picture"));
        return;
    }
    check(true, QStringLiteral("the picture band carries real content (%1% lit)")
                      .arg(100.0 * picture.litFraction(), 0, 'f', 1));
}

/// ── scenario: startup-missing ────────────────────────────────────────────

/// Nothing is installed and nothing is downloaded: startup inspects what is
/// there, names the absence, and writes nothing of its own.
void runStartupMissing(const QString& emptyHint) {
    const QString storage = mplayer::native::runtimeStorageRoot();
    const bool hadStorage = QFileInfo::exists(storage);
    const QString before = hadStorage ? treeFingerprint(storage) : QString();

    // The selection runs and prints its own result before anything is judged:
    // skipping here first is what turned a missing runtime into an unobserved
    // one.
    const RuntimeSelection selection = mplayer::native::selectStartupRuntime(emptyHint);

    // Whatever it decided has to be one of the two honest shapes.
    if (!selection.loaded) {
        checkNamedFailure(selection);
    } else {
        check(selection.kind == BundleKind::Standard && selection.apiVersion > 0,
              QStringLiteral("a runtime that did load is a real Standard module"));
    }

    // A runtime reachable outside this run's own directories is judged after
    // the observation, and named, so the machine this ran on is readable.
    const QStringList reachable = mikai::backend::runtimeCandidates(emptyHint);
    if (!reachable.isEmpty()) {
        reportInconclusive(
            QStringLiteral("this machine has a runtime reachable outside the isolated "
                           "directories (%1), so an absent runtime cannot be observed here")
                .arg(reachable.join(QStringLiteral(", "))));
        return;
    }

    // No network: the point of the startup half. A published bundle here would
    // mean a download ran during selection.
    check(!QFileInfo::exists(QDir(storage).filePath(
              mplayer::native::bundlePointerFileName(BundleKind::Standard))),
          QStringLiteral("startup published no Standard bundle pointer"));
    check(!hadStorage || treeFingerprint(storage) == before,
          QStringLiteral("startup wrote nothing into the app-owned runtime storage"));
    check(!mplayer::native::runtimeSelected(), QStringLiteral("nothing was pinned"));
}

/// ── scenario: startup-corrupt ────────────────────────────────────────────

/// The corrupt candidate is skipped rather than being the last word: the valid
/// module beside it is what gets selected and loaded.
void runStartupCorrupt(const QString& scratch, const QString& module) {
    // Two candidate trees under one hint. The corrupt one is walked first,
    // because the search visits every match for a library name before the
    // next name and both are libmpv-2.dll.
    const QString corruptRoot = QDir(scratch).filePath(QStringLiteral("a-corrupt"));
    const QString validRoot = QDir(scratch).filePath(QStringLiteral("b-valid"));
    QDir().mkpath(corruptRoot);
    const QString corruptModule = QDir(corruptRoot).filePath(QStringLiteral("libmpv-2.dll"));
    // A truncated PE: the MZ stamp is there, so only the image parser can
    // refuse it, which is the check this scenario is about.
    QFile corrupt(corruptModule);
    if (!corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ++failures;
        return;
    }
    corrupt.write(QByteArray("MZ") + QByteArray(4096, '\0'));
    corrupt.close();

    if (!copyTree(QFileInfo(module).absolutePath(), validRoot)) {
        ++failures;
        return;
    }

    const QStringList order = mikai::backend::runtimeCandidates(scratch);
    check(order.size() >= 2 && order.first().compare(QDir::cleanPath(corruptModule),
                                                     Qt::CaseInsensitive) == 0,
          QStringLiteral("the corrupt candidate is the first one the search walks (%1)")
              .arg(order.isEmpty() ? QStringLiteral("no candidates") : order.first()));

    // Describing the corrupt candidate on its own is what proves it is refused
    // by name rather than by absence.
    QString corruptReason;
    check(!mplayer::native::describeStandardRuntime(corruptModule, &corruptReason).has_value(),
          QStringLiteral("the corrupt candidate is refused by name: %1").arg(corruptReason));

    const RuntimeSelection selection = mplayer::native::selectStartupRuntime(scratch);
    check(selection.loaded,
          QStringLiteral("a valid candidate beside the corrupt one is selected: %1")
              .arg(selection.reason));
    if (!selection.loaded) return;
    check(QDir::cleanPath(selection.modulePath).startsWith(QDir::cleanPath(validRoot),
                                                          Qt::CaseInsensitive),
          QStringLiteral("the corrupt candidate was skipped, not selected (%1)")
              .arg(selection.modulePath));
    check(selection.apiVersion > 0,
          QStringLiteral("the module that loaded reports its API version (%1)")
              .arg(selection.apiVersion));
    check(mplayer::native::runtimeSelected(), QStringLiteral("the process pinned one module"));
    check(QFileInfo(selection.root).canonicalFilePath().compare(
              QFileInfo(stockModule()).absolutePath(), Qt::CaseInsensitive) != 0,
          QStringLiteral("the loaded module is the staged copy, not the reference install"));
}

/// ── scenario: publish ────────────────────────────────────────────────────

/// The whole publication path, with no network: the real stock closure is
/// staged into app-owned scratch, described where it lies, published atomically
/// as an immutable Standard bundle, then resolved, committed and rendered.
void runPublish(const QString& scratch, const QString& module) {
    const QString staging = QDir(scratch).filePath(QStringLiteral("stock"));
    if (!copyTree(QFileInfo(module).absolutePath(), staging)) {
        ++failures;
        return;
    }
    const QString stagedModule = QDir(staging).filePath(QFileInfo(module).fileName());

    // Described where it lies: the stock closure carries no manifest, and this
    // read-only half must not invent one.
    QString reason;
    const std::optional<Bundle> described =
        mplayer::native::describeStandardRuntime(stagedModule, &reason);
    if (!described.has_value()) {
        ++failures;
        note(QStringLiteral("the staged stock closure did not describe: %1").arg(reason));
        return;
    }
    check(described->kind == BundleKind::Standard && described->mpvApiVersion == 0,
          QStringLiteral("the stock closure describes as an unmanifested Standard core"));
    check(!QFileInfo::exists(QDir(staging).filePath(QStringLiteral("bundle.json"))),
          QStringLiteral("describing a stock runtime writes no manifest beside it"));

    QString publishReason;
    QString publishedRoot;
    if (!mplayer::native::installAtomic(*described, mplayer::native::runtimeStorageRoot(),
                                       &publishReason, &publishedRoot)) {
        ++failures;
        note(QStringLiteral("the stock closure was not published: %1").arg(publishReason));
        return;
    }
    note(QStringLiteral("published %1").arg(publishedRoot));
    check(QFileInfo(publishedRoot).isDir(), QStringLiteral("the published directory exists"));
    check(QFileInfo::exists(QDir(publishedRoot).filePath(QStringLiteral("bundle.json"))),
          QStringLiteral("publication materialised the manifest inside the published bundle"));
    check(!QFileInfo::exists(QDir(staging).filePath(QStringLiteral("bundle.json"))),
          QStringLiteral("publication wrote the manifest only in staging, never into the source"));

    const QString pointerPath = QDir(mplayer::native::runtimeStorageRoot())
                                     .filePath(mplayer::native::bundlePointerFileName(
                                         BundleKind::Standard));
    QFile pointer(pointerPath);
    QString pointerName;
    if (pointer.open(QIODevice::ReadOnly)) pointerName = QString::fromUtf8(pointer.readAll()).trimmed();
    check(pointerName == QFileInfo(publishedRoot).fileName(),
          QStringLiteral("the current Standard pointer names the published bundle (%1)")
              .arg(pointerName));

    // The published bundle is re-read and revalidated from disk rather than
    // trusted because it was copied from a valid source.
    QString readReason;
    const std::optional<Bundle> published = mplayer::native::readBundle(publishedRoot, &readReason);
    check(published.has_value(),
          QStringLiteral("the published bundle is readable: %1").arg(readReason));
    if (published.has_value()) {
        QString validateReason;
        check(mplayer::native::validateBundle(*published, &validateReason),
              QStringLiteral("the published bundle validates against its own bytes: %1")
                  .arg(validateReason));
    }

    // Republishing the same inventory is refused: published bundles are
    // immutable, so a retry cannot rewrite what a load may already be using.
    QString secondReason;
    check(!mplayer::native::installAtomic(*described, mplayer::native::runtimeStorageRoot(),
                                         &secondReason),
          QStringLiteral("republishing the same inventory is refused: %1").arg(secondReason));

    // The recovery resolve half answers from what is installed, so this run
    // provisions nothing: the bundle published above is the one it finds.
    const RuntimeSelection resolved = mplayer::native::resolveRuntime(RuntimeChoice::Standard);
    check(!resolved.loaded, QStringLiteral("resolve does not load; commit alone pins"));
    check(resolved.resolvedBundle.has_value(), QStringLiteral("resolve produced a validated bundle"));
    if (!resolved.resolvedBundle.has_value()) return;
    check(QDir::cleanPath(resolved.root).compare(QDir::cleanPath(publishedRoot), Qt::CaseInsensitive) == 0,
          QStringLiteral("resolve preferred the published bundle over the staged stock tree (%1)")
              .arg(resolved.root));

    const RuntimeSelection committed = mplayer::native::commitRuntime(resolved);
    checkPublishedAndLoaded(committed, publishedRoot);

    VideoSurface surface;
    checkRendersRealFrames(surface);

    // One module per process: recovery must refuse a second attempt now.
    check(!mplayer::native::beginRecovery(),
          QStringLiteral("recovery refuses to run once a module is pinned"));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    const QStringList arguments = QApplication::arguments();
    const QString scenario = arguments.size() > 1 ? arguments.at(1) : QString();
    std::printf("scenario=%s\n", qPrintable(scenario));
    if (scenario != QLatin1String("startup-missing") && scenario != QLatin1String("startup-corrupt") &&
        scenario != QLatin1String("publish")) {
        std::fprintf(stderr,
                     "usage: runtime_recovery_probe "
                     "<startup-missing|startup-corrupt|publish>\n");
        return 1;
    }

    const QString module = stockModule();
    if (module.isEmpty()) {
        std::printf(
            "RESULT: SKIP (set MIKAI_MPV_MODULE to a real libmpv-2.dll; load and render evidence "
            "cannot be faked)\n");
        return 77;
    }

    // The storage and the settings a run may write are its own. The identity
    // carries the pid so two concurrent runs never share a published bundle.
    QTemporaryDir localAppData;
    QTemporaryDir scratch;
    QTemporaryDir settingsDir;
    if (!localAppData.isValid() || !scratch.isValid() || !settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create isolated temporary directories\n");
        return 1;
    }
    qputenv("LOCALAPPDATA", localAppData.path().toLocal8Bit());
    QCoreApplication::setOrganizationName(
        QStringLiteral("MikaiRecoveryProbe-%1").arg(QCoreApplication::applicationPid()));
    QCoreApplication::setApplicationName(QStringLiteral("RuntimeRecoveryProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    const QString storage = mplayer::native::runtimeStorageRoot();
    const QString stockRoot = QFileInfo(module).absolutePath();
    const QString stockBefore = treeFingerprint(stockRoot);
    note(QStringLiteral("stock fixture: %1").arg(module));
    note(QStringLiteral("app-owned runtime storage: %1").arg(storage));
    note(QStringLiteral("owned scratch directory: %1").arg(scratch.path()));

    if (scenario == QLatin1String("startup-missing")) {
        // An empty hint is the whole search: with the stock cache redirected to
        // this run's LOCALAPPDATA there is nothing beside the executable either.
        const QString emptyHint = QDir(scratch.path()).filePath(QStringLiteral("empty"));
        QDir().mkpath(emptyHint);
        runStartupMissing(emptyHint);
    } else if (scenario == QLatin1String("startup-corrupt")) {
        runStartupCorrupt(scratch.path(), module);
    } else {
        runPublish(scratch.path(), module);
    }

    check(treeFingerprint(stockRoot) == stockBefore,
          QStringLiteral("the stock reference install is byte-for-byte unchanged"));

    // The loaded DLL stays pinned until exit; the caller removes these owned paths afterwards.
    if (QFileInfo::exists(storage)) {
        note(QStringLiteral("cleanup after process exit: %1").arg(storage));
    }

    if (failures > 0) {
        std::printf("RESULT: FAIL (%d failed)\n", failures);
        return 1;
    }
    if (inconclusive) {
        std::printf("RESULT: SKIP (reported above)\n");
        return 77;
    }
    std::printf("RESULT: PASS\n");
    return 0;
}

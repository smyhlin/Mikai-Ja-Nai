#include "TestMpvAsync.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QXmlStreamReader>

#include <clocale>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "MpvApi.hpp"
#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"
#include "VideoSurface.hpp"
#include "native/bundle/NativeRuntimeSelection.hpp"
#include "dialogs/settings/BenchmarkPlayback.hpp"

using mplayer::AsyncReply;
using mplayer::AsyncRequestId;
using mplayer::MpvApi;
using mplayer::MpvBundle;
using mplayer::MpvError;
using mplayer::MpvHandle;
using mplayer::NodeValue;
using mplayer::Property;

using mikai::app::BenchmarkFrameSample;
using mplayer::isRealNewFrame;

namespace {
class NumericLocaleRestore {
public:
    NumericLocaleRestore() {
        const char* current = std::setlocale(LC_NUMERIC, nullptr);
        if (current != nullptr) saved_ = current;
    }
    ~NumericLocaleRestore() {
        if (!saved_.isEmpty()) std::setlocale(LC_NUMERIC, saved_.constData());
    }

    bool valid() const { return !saved_.isEmpty(); }
    QByteArray saved() const { return saved_; }

private:
    QByteArray saved_;
};


// A descriptor over a real directory, so the refusal under test is the one the
// test names rather than "no such folder".
MpvBundle descriptorIn(const QString& root, const QString& moduleName) {
    MpvBundle bundle;
    bundle.root = root;
    bundle.modulePath = QDir(root).filePath(moduleName);
    bundle.descriptorId = QStringLiteral("test-descriptor");
    bundle.protocolVersion = 1;
    return bundle;
}

// The module the transport tests run on: an explicit path, never a search. The
// environment variable is the only place it can come from, so a test run
// neither provisions nor downloads a runtime.
MpvBundle envBundle() {
    const QString module = qEnvironmentVariable("MIKAI_MPV_MODULE");
    if (module.isEmpty()) return MpvBundle{};
    const QFileInfo info(module);
    MpvBundle bundle;
    bundle.root = info.absolutePath();
    bundle.modulePath = info.absoluteFilePath();
    bundle.descriptorId = QStringLiteral("env-standard-runtime");
    bundle.protocolVersion = 1;
    return bundle;
}

// Runs a selection that has to fail, and checks the process is no worse for it.
// With nothing pinned the refusal names the specific fault; with a module
// pinned the same call is the hot-swap refusal, and the module in use must
// still be the one that survives.
void expectRefusal(const MpvBundle& bundle, const QString& namedFault) {
    const bool pinned = MpvApi::instance().loaded();
    const QString pinnedPath = pinned ? MpvApi::instance().bundle().modulePath : QString();
    QString reason;
    QVERIFY(!MpvApi::instance().load(bundle, &reason));
    QVERIFY2(!reason.isEmpty(), "a refused selection must carry a reason");
    if (pinned) {
        QVERIFY2(reason.contains(QStringLiteral("Refusing to replace")), qPrintable(reason));
        QVERIFY(MpvApi::instance().loaded());
        QCOMPARE(MpvApi::instance().bundle().modulePath, pinnedPath);
    } else {
        QVERIFY2(reason.contains(namedFault), qPrintable(reason));
        QVERIFY(!MpvApi::instance().loaded());
    }
}

// Drains the way the player's 50 ms pump does, and reports whether the
// condition held in time.
template <typename Predicate>
bool pumpUntil(MpvHandle& handle, Predicate ready, int timeoutMs = 5000) {
    QElapsedTimer clock;
    clock.start();
    while (!ready() && clock.elapsed() < timeoutMs) {
        handle.drainEvents();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(5);
    }
    return ready();
}

MpvHandle makeHandle() {
    // The same options the surface passes, so the tests drive the core the
    // player actually drives rather than a differently configured one.
    return MpvHandle({{"vo", "libmpv"}, {"idle", "yes"}, {"input-default-bindings", "no"}});
}

}  // namespace

void TestMpvAsync::initTestCase() {
    // Selection happens here, once, and only from an explicit module path. A
    // suite that inherited some earlier selection would report a pass against a
    // module it never asked for, and one that searched would provision a runtime
    // as a side effect of running the tests. Without the variable the transport
    // tests skip; the loader-boundary tests still run in either state.
    if (MpvApi::instance().loaded()) return;
    const MpvBundle bundle = envBundle();
    if (bundle.modulePath.isEmpty()) return;
    QString reason;
    if (!MpvApi::instance().load(bundle, &reason)) selectionError_ = reason;
}

// ── loader boundary ────────────────────────────────────────────────────────

void TestMpvAsync::anEmptyDescriptorIsRefusedByName() {
    expectRefusal(MpvBundle{}, QStringLiteral("mpv runtime unavailable"));
}

void TestMpvAsync::aRelativeModulePathIsRefusedByName() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    MpvBundle bundle = descriptorIn(root.path(), QStringLiteral("libmpv-2.dll"));
    bundle.modulePath = QStringLiteral("libmpv-2.dll");
    expectRefusal(bundle, QStringLiteral("absolute"));
}

void TestMpvAsync::aMissingModuleIsRefusedByName() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    // The user has to be able to tell "nothing is installed" from "the install
    // is broken", so the reason names the file that is not there.
    expectRefusal(descriptorIn(root.path(), QStringLiteral("libmpv-2.dll")),
                  QStringLiteral("libmpv-2.dll"));
}

void TestMpvAsync::aModuleOutsideTheOwnedRootIsRefusedByName() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    // An absolute path to a file that really exists, but not under the root:
    // the loader must refuse a core it does not own rather than load it.
    MpvBundle bundle = descriptorIn(root.path(), QStringLiteral("libmpv-2.dll"));
    bundle.modulePath = QFileInfo(QCoreApplication::applicationFilePath()).absoluteFilePath();
    expectRefusal(bundle, QStringLiteral("outside the owned runtime root"));
}

void TestMpvAsync::linuxRuntimeSelectionMatchesTheConfiguredModule() {
#ifdef Q_OS_LINUX
    const mplayer::native::RuntimeSelection selection =
        mplayer::native::resolveRuntime(mplayer::native::RuntimeChoice::Standard);
    QVERIFY2(selection.resolvedBundle.has_value(), qPrintable(selection.reason));
    QCOMPARE(selection.kind, mplayer::native::BundleKind::Standard);
    const MpvBundle descriptor = selection.resolvedBundle->toMpvBundleDescriptor();
    QCOMPARE(descriptor.modulePath, selection.modulePath);
#if defined(MIKAI_LINUX_BUNDLE_MPV) && MIKAI_LINUX_BUNDLE_MPV
#ifndef MIKAI_LINUX_MPV_MODULE_RELATIVE
#define MIKAI_LINUX_MPV_MODULE_RELATIVE "mpv/libmpv.so.2"
#endif
    const QString expected =
        QFileInfo(QDir(QCoreApplication::applicationDirPath())
                      .filePath(QString::fromUtf8(MIKAI_LINUX_MPV_MODULE_RELATIVE)))
            .canonicalFilePath();
    QVERIFY2(!expected.isEmpty(), "the configured executable-local mpv module must exist");
    QCOMPARE(selection.modulePath, expected);
    QCOMPARE(selection.resolvedBundle->layout, mplayer::native::BundleLayout::OwnedDirectory);
#elif defined(MIKAI_LINUX_SYSTEM_MPV_MODULE)
    const QString expected =
        QFileInfo(QString::fromUtf8(MIKAI_LINUX_SYSTEM_MPV_MODULE)).canonicalFilePath();
    QVERIFY2(!expected.isEmpty(), "the configured system mpv module must exist");
    QCOMPARE(selection.modulePath, expected);
    QCOMPARE(selection.resolvedBundle->layout, mplayer::native::BundleLayout::SystemModule);
#else
    QSKIP("this Linux test target has no configured runtime mode");
#endif
#else
    QSKIP("Linux runtime selection is not applicable on this platform");
#endif
}

void TestMpvAsync::aDifferentModuleIsRefusedWhileOneIsPinned() {
    if (!requireRuntime()) return;
    const MpvBundle pinned = MpvApi::instance().bundle();
    const quint64 version = MpvApi::instance().apiVersion();

    // A different file AND a different id, so the selection is a different
    // module whichever of the two decides identity.
    MpvBundle other = pinned;
    other.root = QCoreApplication::applicationDirPath();
    other.modulePath = QDir(other.root).filePath(QStringLiteral("not-the-pinned-module.dll"));
    other.descriptorId = QStringLiteral("another-runtime");

    QString reason;
    QVERIFY(!MpvApi::instance().load(other, &reason));
    QVERIFY2(reason.contains(QStringLiteral("Refusing to replace")), qPrintable(reason));
    // Refused means refused: the pinned module is still the one in use.
    QVERIFY(MpvApi::instance().loaded());
    QCOMPARE(MpvApi::instance().bundle().modulePath, pinned.modulePath);
    QCOMPARE(MpvApi::instance().apiVersion(), version);

    // And the selected descriptor is still a plain no-op success, not a reload.
    QVERIFY(MpvApi::instance().load(pinned, &reason));
    QVERIFY(reason.isEmpty());
    QCOMPARE(MpvApi::instance().bundle().modulePath, pinned.modulePath);
}

void TestMpvAsync::theHeaderApiVersionIsAlwaysReported() {
    // Independent of any module: it is what a selected runtime is compared to.
    QCOMPARE(MpvApi::headerApiVersion(), static_cast<quint64>(MPV_CLIENT_API_VERSION));
    if (MpvApi::instance().loaded()) {
        // Once a core is selected the handle reports what that module signed up
        // to, which is the version the loader already accepted.
        QCOMPARE(MpvHandle::apiVersion(), MpvApi::instance().apiVersion());
    } else {
        QCOMPARE(MpvHandle::apiVersion(), MpvApi::headerApiVersion());
    }
}

void TestMpvAsync::aHandleWithoutASelectedRuntimeIsRefused() {
    if (MpvApi::instance().loaded()) {
        QSKIP("a runtime is already selected in this process");
    }
    // The handle never searches for a core, so this is the failure a caller
    // sees when it skipped runtime selection.
    bool refused = false;
    try {
        MpvHandle handle;
    } catch (const MpvError& error) {
        refused = true;
        QVERIFY(QString::fromStdString(error.what()).contains(QStringLiteral("MpvApi::load")));
    }
    QVERIFY(refused);
}

void TestMpvAsync::aCoreUsesTheCLocaleForNumericFormatting() {
    constexpr auto childMarker = "MIKAI_TEST_MPV_NUMERIC_LOCALE_CHILD";
    if (qEnvironmentVariableIntValue(childMarker) != 1) {
        // The runner protects executable-local native fixtures. A fresh copy is
        // essential even when the parent started in an isolated directory: other
        // suites may already have created its native tree.
        QTemporaryDir directory;
        QVERIFY2(directory.isValid(), "cannot create locale test executable directory");
        const QString executable = directory.filePath(
            QFileInfo(QCoreApplication::applicationFilePath()).fileName());
        QVERIFY2(QFile::copy(QCoreApplication::applicationFilePath(), executable),
                 "cannot copy locale test executable");

        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QString::fromLatin1(childMarker), QStringLiteral("1"));
#ifdef Q_OS_WIN
        environment.insert(QStringLiteral("PATH"),
            QLibraryInfo::path(QLibraryInfo::BinariesPath) + QDir::listSeparator() +
            environment.value(QStringLiteral("PATH")));
#endif
        environment.insert(QStringLiteral("QT_PLUGIN_PATH"),
                           QLibraryInfo::path(QLibraryInfo::PluginsPath));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
            QDir(QLibraryInfo::path(QLibraryInfo::PluginsPath)).filePath(QStringLiteral("platforms")));
        if (environment.value(QStringLiteral("QT_QPA_PLATFORM")).isEmpty())
            environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));

        QProcess child;
        child.setProcessEnvironment(environment);
        child.setWorkingDirectory(directory.path());
        child.start(executable, {QStringLiteral("TestMpvAsync"),
                                 QStringLiteral("aCoreUsesTheCLocaleForNumericFormatting"),
                                 QStringLiteral("-o"), QStringLiteral("-,xml")});
        QVERIFY2(child.waitForStarted(10000), qPrintable(child.errorString()));
        if (!child.waitForFinished(30000)) {
            child.kill();
            child.waitForFinished(5000);
            const QString details = QStringLiteral("locale test child timed out\nstdout:\n%1\nstderr:\n%2")
                .arg(QString::fromUtf8(child.readAllStandardOutput()),
                     QString::fromUtf8(child.readAllStandardError()));
            QFAIL(qPrintable(details));
        }
        const QByteArray output = child.readAllStandardOutput();
        const QByteArray errors = child.readAllStandardError();
        const QString details = QStringLiteral("locale test child output:\n%1\nstderr:\n%2")
            .arg(QString::fromUtf8(output), QString::fromUtf8(errors));
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                 qPrintable(details));

        QXmlStreamReader xml(output);
        bool target = false;
        bool found = false;
        bool passed = false;
        bool skipped = false;
        QString skipReason;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("TestFunction")) {
                target = xml.attributes().value(QLatin1String("name")) ==
                         QLatin1String("aCoreUsesTheCLocaleForNumericFormatting");
                found |= target;
            } else if (xml.isEndElement() && xml.name() == QLatin1String("TestFunction")) {
                target = false;
            } else if (target && xml.isStartElement() && xml.name() == QLatin1String("Incident")) {
                const auto type = xml.attributes().value(QLatin1String("type"));
                if (type == QLatin1String("pass")) passed = true;
                if (type == QLatin1String("skip")) {
                    skipped = true;
                    while (xml.readNextStartElement()) {
                        if (xml.name() == QLatin1String("Description"))
                            skipReason = xml.readElementText();
                        else
                            xml.skipCurrentElement();
                    }
                }
            }
        }
        QVERIFY2(!xml.hasError() && found && (passed || skipped), qPrintable(details));
        if (skipped) {
            QVERIFY2(!skipReason.isEmpty(), qPrintable(details));
            QSKIP(qPrintable(skipReason));
        }
        return;
    }

    if (!requireRuntime()) return;
    NumericLocaleRestore restore;
    QVERIFY(restore.valid());
    const QByteArray savedLocale = restore.saved();
    const char* nonCLocale = nullptr;
#ifdef _WIN32
    const char* candidates[] = {"de-DE", "German_Germany.1252", "German_Germany"};
#else
    const char* candidates[] = {"uk_UA.UTF-8", "uk_UA.utf8", "de_DE.UTF-8",
                                "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8"};
#endif
    for (const char* candidate : candidates) {
        const char* selected = std::setlocale(LC_NUMERIC, candidate);
        if (selected != nullptr && std::localeconv()->decimal_point != nullptr &&
            std::string(std::localeconv()->decimal_point) == ",") {
            nonCLocale = selected;
            break;
        }
    }
    if (nonCLocale == nullptr) QSKIP("no installed decimal-comma numeric locale");
    const QByteArray localizedLocale(nonCLocale);
    QVERIFY(std::setlocale(LC_NUMERIC, localizedLocale.constData()) != nullptr);

    bool coreCreated = false;
    QString coreError;
    double speed = 0.0;
    try {
        MpvHandle handle({{"vo", "libmpv"}, {"idle", "yes"},
                          {"input-default-bindings", "no"}});
        coreCreated = true;
        handle.setString("speed", "1.25");
        speed = handle.getNumber("speed");
    } catch (const std::exception& error) {
        coreError = QString::fromUtf8(error.what());
    }

    QVERIFY2(coreCreated && coreError.isEmpty(), qPrintable(coreError));
    QCOMPARE(std::string(std::localeconv()->decimal_point), std::string("."));
    QVERIFY(qAbs(speed - 1.25) < 0.000001);
    QVERIFY(std::setlocale(LC_NUMERIC, savedLocale.constData()) != nullptr);
}

// ── public FPS provenance policy; no mpv runtime required ──────────────────

void TestMpvAsync::fpsSelectorPrefersValidMpvOutput() {
    const auto resolved =
        mplayer::resolveStreamFps(std::optional<double>{59.94}, 24.0, true);

    QCOMPARE(resolved.framesPerSecond, 59.94);
    QCOMPARE(resolved.source, mplayer::StreamFpsSource::MpvOutput);
}

void TestMpvAsync::fpsSelectorFallsBackToContainerForInvalidOutput_data() {
    QTest::addColumn<bool>("hasOutput");
    QTest::addColumn<double>("output");
    QTest::addColumn<double>("container");

    QTest::newRow("absent-output") << false << 0.0 << 23.976;
    QTest::newRow("zero-output") << true << 0.0 << 23.976;
    QTest::newRow("negative-output") << true << -1.0 << 23.976;
    QTest::newRow("nan-output") << true << std::numeric_limits<double>::quiet_NaN() << 23.976;
    QTest::newRow("infinite-output") << true << std::numeric_limits<double>::infinity() << 23.976;
}

void TestMpvAsync::fpsSelectorFallsBackToContainerForInvalidOutput() {
    QFETCH(bool, hasOutput);
    QFETCH(double, output);
    QFETCH(double, container);

    const std::optional<double> observed =
        hasOutput ? std::optional<double>{output} : std::nullopt;
    const auto resolved = mplayer::resolveStreamFps(observed, container, true);

    QCOMPARE(resolved.framesPerSecond, container);
    QCOMPARE(resolved.source, mplayer::StreamFpsSource::Container);
}

void TestMpvAsync::fpsSelectorIgnoresStaleRatesWhenInactive_data() {
    QTest::addColumn<bool>("hasOutput");
    QTest::addColumn<double>("output");
    QTest::addColumn<double>("container");

    QTest::newRow("stale-output-and-container") << true << 60.0 << 24.0;
    QTest::newRow("stale-container-only") << false << 0.0 << 24.0;
}

void TestMpvAsync::fpsSelectorIgnoresStaleRatesWhenInactive() {
    QFETCH(bool, hasOutput);
    QFETCH(double, output);
    QFETCH(double, container);

    const std::optional<double> observed =
        hasOutput ? std::optional<double>{output} : std::nullopt;
    const auto resolved = mplayer::resolveStreamFps(observed, container, false);

    QCOMPARE(resolved.framesPerSecond, 0.0);
    QCOMPARE(resolved.source, mplayer::StreamFpsSource::Unknown);
}

void TestMpvAsync::fpsSelectorReturnsUnknownWithoutFinitePositiveRate_data() {
    QTest::addColumn<bool>("hasOutput");
    QTest::addColumn<double>("output");
    QTest::addColumn<double>("container");

    QTest::newRow("both-absent-or-zero") << false << 0.0 << 0.0;
    QTest::newRow("nonpositive") << true << -1.0 << -24.0;
    QTest::newRow("nan-candidates") << true << std::numeric_limits<double>::quiet_NaN()
                                      << std::numeric_limits<double>::quiet_NaN();
    QTest::newRow("infinite-candidates") << true << std::numeric_limits<double>::infinity()
                                           << std::numeric_limits<double>::infinity();
    QTest::newRow("invalid-container-only") << false << 0.0
                                               << std::numeric_limits<double>::infinity();
}

void TestMpvAsync::fpsSelectorReturnsUnknownWithoutFinitePositiveRate() {
    QFETCH(bool, hasOutput);
    QFETCH(double, output);
    QFETCH(double, container);

    const std::optional<double> observed =
        hasOutput ? std::optional<double>{output} : std::nullopt;
    const auto resolved = mplayer::resolveStreamFps(observed, container, true);

    QCOMPARE(resolved.framesPerSecond, 0.0);
    QCOMPARE(resolved.source, mplayer::StreamFpsSource::Unknown);
}

// ── native-status polling policy; no mpv runtime required ──────────────────

void TestMpvAsync::probeSnapshotsKeepTheStatusReaderPolling_data() {
    QTest::addColumn<int>("state");
    QTest::addColumn<bool>("keepsPolling");

    // The producer publishes Probe before it has adopted a decoded frame, and
    // its own document names exactly that ("the decoder device identity is not
    // available"). Halting there is what left an attached probe unreadable.
    QTest::newRow("probe-before-any-frame") << static_cast<int>(mplayer::native::NativeState::Probe)
                                            << true;
    QTest::newRow("active") << static_cast<int>(mplayer::native::NativeState::Active) << true;
    QTest::newRow("preparing") << static_cast<int>(mplayer::native::NativeState::Preparing)
                               << true;
    // Every other state repeats identically until a new request arrives.
    QTest::newRow("off") << static_cast<int>(mplayer::native::NativeState::Off) << false;
    QTest::newRow("needs-preparation")
        << static_cast<int>(mplayer::native::NativeState::NeedsPreparation) << false;
    QTest::newRow("suspended") << static_cast<int>(mplayer::native::NativeState::Suspended)
                               << false;
    QTest::newRow("failed") << static_cast<int>(mplayer::native::NativeState::Failed) << false;
    QTest::newRow("unsupported") << static_cast<int>(mplayer::native::NativeState::Unsupported)
                                 << false;
    QTest::newRow("deactivated") << static_cast<int>(mplayer::native::NativeState::Deactivated)
                                 << false;
}

void TestMpvAsync::probeSnapshotsKeepTheStatusReaderPolling() {
    QFETCH(int, state);
    QFETCH(bool, keepsPolling);

    QCOMPARE(mplayer::nativeSnapshotKeepsPolling(
                 static_cast<mplayer::native::NativeState>(state)),
             keepsPolling);
}

// ── transport against a real core ──────────────────────────────────────────

void TestMpvAsync::anAsyncPropertyReplyCarriesItsOwnValue() {
    if (!requireRuntime()) return;
    MpvHandle handle = makeHandle();

    std::vector<AsyncReply> replies;
    const AsyncRequestId id = handle.getPropertyAsync(
        "mpv-version", MPV_FORMAT_STRING, [&replies](const AsyncReply& reply) { replies.push_back(reply); });
    QVERIFY(id.valid());
    QCOMPARE(handle.pendingAsyncCount(), size_t{1});
    QVERIFY(pumpUntil(handle, [&replies] { return !replies.empty(); }));

    QCOMPARE(replies.size(), size_t{1});
    QCOMPARE(replies.front().error, 0);
    QCOMPARE(replies.front().format, MPV_FORMAT_STRING);
    QVERIFY(!replies.front().text.empty());
    // Delivered exactly once: the request retires when its reply lands.
    handle.drainEvents();
    QCOMPARE(replies.size(), size_t{1});
    QCOMPARE(handle.pendingAsyncCount(), size_t{0});
}

void TestMpvAsync::eventBacklogYieldsToTheGui() {
    if (!requireRuntime()) return;
    auto handle = makeHandle();
    int completed = 0;
    int submitted = 0;
    for (int index = 0; index < 512; ++index) {
        const auto request = handle.getPropertyAsync("mpv-version", MPV_FORMAT_STRING,
                    [&completed](const AsyncReply&) {
                        ++completed;
                        QThread::msleep(5);
                    });
        if (!request.valid()) break;
        ++submitted;
    }
    QVERIFY(submitted >= 64);
    QTest::qWait(100);
    bool timerRan = false;
    QTimer::singleShot(0, this, [&] { timerRan = true; });
    QElapsedTimer clock;
    clock.start();
    handle.drainEvents();
    QCoreApplication::processEvents();
    QVERIFY(timerRan);
    QVERIFY2(clock.elapsed() < 250,
             qPrintable(QStringLiteral("one event drain blocked the GUI for %1 ms").arg(clock.elapsed())));
    QVERIFY(pumpUntil(handle, [&] { return completed == submitted; }, 10000));
}

void TestMpvAsync::nestedEventDrainsPreserveTheCurrentReply() {
    if (!requireRuntime()) return;
    auto handle = makeHandle();
    bool nested = false;
    handle.setEventSink([&](mpv_event* event) {
        if (event->event_id != MPV_EVENT_GET_PROPERTY_REPLY || nested) return;
        nested = true;
        handle.drainEvents();
    });
    std::string version;
    std::string directory;
    QVERIFY(handle.getPropertyAsync("mpv-version", MPV_FORMAT_STRING,
                [&](const AsyncReply& reply) { version = reply.text; }).valid());
    QVERIFY(handle.getPropertyAsync("working-directory", MPV_FORMAT_STRING,
                [&](const AsyncReply& reply) { directory = reply.text; }).valid());
    QVERIFY(pumpUntil(handle, [&] { return !version.empty() && !directory.empty(); }));
    QVERIFY(nested);
    QVERIFY(version != directory);
}

void TestMpvAsync::selfUnobservingCallbacksKeepTheirCapturesAlive() {
    if (!requireRuntime()) return;
    auto handle = makeHandle();
    auto token = std::make_shared<int>(7);
    const std::weak_ptr<int> lifetime = token;
    bool delivered = false;
    bool aliveDuringCallback = false;
    uint64_t observer = 0;
    observer = handle.observe("idle-active", MPV_FORMAT_FLAG,
        [&, token](const Property&) {
            handle.unobserve(observer);
            aliveDuringCallback = !lifetime.expired();
            delivered = true;
        });
    token.reset();
    QVERIFY(pumpUntil(handle, [&] { return delivered; }));
    QVERIFY(aliveDuringCallback);
    QVERIFY(lifetime.expired());
}

void TestMpvAsync::selfReplacingEventSinksKeepTheirCapturesAlive() {
    if (!requireRuntime()) return;
    auto handle = makeHandle();
    auto token = std::make_shared<int>(7);
    const std::weak_ptr<int> lifetime = token;
    bool observed = false;
    bool aliveDuringCallback = false;
    handle.setEventSink([&, token](mpv_event* event) {
        if (event->event_id != MPV_EVENT_GET_PROPERTY_REPLY) return;
        handle.setEventSink({});
        aliveDuringCallback = !lifetime.expired();
    });
    token.reset();
    QVERIFY(handle.getPropertyAsync("mpv-version", MPV_FORMAT_STRING,
                [&](const AsyncReply& reply) { observed = !reply.text.empty(); }).valid());
    QVERIFY(pumpUntil(handle, [&] { return observed; }));
    QVERIFY(aliveDuringCallback);
    QVERIFY(lifetime.expired());
}

void TestMpvAsync::anUnknownPropertyRepliesWithAnError() {
    if (!requireRuntime()) return;
    MpvHandle handle = makeHandle();

    std::vector<AsyncReply> replies;
    const AsyncRequestId id = handle.getPropertyAsync(
        "no-such-property", MPV_FORMAT_DOUBLE,
        [&replies](const AsyncReply& reply) { replies.push_back(reply); });
    QVERIFY(id.valid());
    QVERIFY(pumpUntil(handle, [&replies] { return !replies.empty(); }));
    QCOMPARE(replies.size(), size_t{1});
    QVERIFY(replies.front().error < 0);
    QVERIFY(!replies.front().ok());
}

void TestMpvAsync::anAsyncNodeReplySurvivesTheNextDrain() {
    if (!requireRuntime()) return;
    MpvHandle handle = makeHandle();

    std::vector<AsyncReply> replies;
    const AsyncRequestId id = handle.getPropertyAsync(
        "mpv-version", MPV_FORMAT_NODE, [&replies](const AsyncReply& reply) { replies.push_back(reply); });
    QVERIFY(id.valid());
    QVERIFY(pumpUntil(handle, [&replies] { return !replies.empty(); }));
    QCOMPARE(replies.size(), size_t{1});
    QCOMPARE(replies.front().error, 0);
    QCOMPARE(replies.front().format, MPV_FORMAT_NODE);
    // The event carries the property's own node shape, and the reply holds a
    // copy of it rather than a pointer into mpv's event storage.
    const NodeValue held = replies.front().node;
    QVERIFY(held.format != MPV_FORMAT_NONE);

    // mpv reclaims event storage on the next wait_event, so a borrowed node
    // would be freed memory by now. The copy has to still read back what it was
    // after both drains and the traffic of an unrelated request.
    handle.drainEvents();
    handle.drainEvents();
    replies.clear();
    QVERIFY(handle
                .getPropertyAsync("mpv-version", MPV_FORMAT_NODE,
                                  [&replies](const AsyncReply& reply) { replies.push_back(reply); })
                .valid());
    QVERIFY(pumpUntil(handle, [&replies] { return !replies.empty(); }));
    QCOMPARE(held.format, replies.front().node.format);
    QCOMPARE(held.text, replies.front().node.text);
}

void TestMpvAsync::aCancelledRequestNeverCallsBack() {
    if (!requireRuntime()) return;
    MpvHandle handle = makeHandle();

    int calls = 0;
    const AsyncRequestId id = handle.getPropertyAsync(
        "mpv-version", MPV_FORMAT_STRING, [&calls](const AsyncReply&) { ++calls; });
    QVERIFY(id.valid());
    handle.cancelAsync(id);
    QCOMPARE(handle.pendingAsyncCount(), size_t{0});

    // The reply still arrives and is consumed; what must not happen is a
    // callback for a request nobody is waiting on any more.
    pumpUntil(handle, [] { return false; }, 250);
    QCOMPARE(calls, 0);
}

void TestMpvAsync::cancellingAllDropsEveryCallback() {
    if (!requireRuntime()) return;
    MpvHandle handle = makeHandle();

    int calls = 0;
    QVERIFY(handle
                .getPropertyAsync("mpv-version", MPV_FORMAT_STRING,
                                  [&calls](const AsyncReply&) { ++calls; })
                .valid());
    QVERIFY(handle
                .getPropertyAsync("mpv-version", MPV_FORMAT_NODE,
                                  [&calls](const AsyncReply&) { ++calls; })
                .valid());
    QCOMPARE(handle.pendingAsyncCount(), size_t{2});

    // What a source switch or a superseded generation does: drop every
    // outstanding callback at once rather than waiting for the destructor.
    handle.cancelAllAsync();
    QCOMPARE(handle.pendingAsyncCount(), size_t{0});
    pumpUntil(handle, [] { return false; }, 250);
    QCOMPARE(calls, 0);
}

void TestMpvAsync::aForeignCoreCancelLeavesThisCoreAlone() {
    if (!requireRuntime()) return;
    MpvHandle first = makeHandle();
    MpvHandle second = makeHandle();
    QVERIFY(first.raw() != second.raw());

    std::vector<AsyncReply> firstReplies;
    std::vector<AsyncReply> secondReplies;
    const AsyncRequestId firstId = first.getPropertyAsync(
        "mpv-version", MPV_FORMAT_STRING,
        [&firstReplies](const AsyncReply& reply) { firstReplies.push_back(reply); });
    const AsyncRequestId secondId = second.getPropertyAsync(
        "mpv-version", MPV_FORMAT_STRING,
        [&secondReplies](const AsyncReply& reply) { secondReplies.push_back(reply); });
    QVERIFY(firstId.valid());
    QVERIFY(secondId.valid());
    // Both cores number their first request 1, so the ids collide on the number
    // alone - which is exactly what the identity has to disambiguate.
    QCOMPARE(firstId.value, secondId.value);
    QVERIFY(firstId.identity != secondId.identity);
    QCOMPARE(first.pendingAsyncCount(), size_t{1});
    QCOMPARE(second.pendingAsyncCount(), size_t{1});

    // Cancelling through the wrong handle must be refused: the id names the
    // other core, so it can never retire this handle's unrelated request.
    second.cancelAsync(firstId);
    QCOMPARE(second.pendingAsyncCount(), size_t{1});
    QCOMPARE(first.pendingAsyncCount(), size_t{1});

    // Both requests are still live and reach the handle that issued them.
    QVERIFY(pumpUntil(first, [&firstReplies] { return !firstReplies.empty(); }));
    QVERIFY(pumpUntil(second, [&secondReplies] { return !secondReplies.empty(); }));
    QCOMPARE(firstReplies.size(), size_t{1});
    QCOMPARE(secondReplies.size(), size_t{1});
    QCOMPARE(firstReplies.front().error, 0);
    QCOMPARE(secondReplies.front().error, 0);
}

void TestMpvAsync::aPendingReplyIsDroppedWhenTheSourceChanges() {
    if (!requireRuntime()) return;
    MpvHandle handle = makeHandle();

    int calls = 0;
    const uint64_t before = handle.sourceEpoch();
    const AsyncRequestId id = handle.getPropertyAsync(
        "mpv-version", MPV_FORMAT_STRING, [&calls](const AsyncReply&) { ++calls; });
    QVERIFY(id.valid());
    QCOMPARE(handle.coreIdentity(), id.identity);
    QCOMPARE(id.epoch, before);
    QCOMPARE(handle.pendingAsyncCount(), size_t{1});

    // A new source. The outstanding request belonged to the old one, so it is
    // retired before the load rather than delivered against what comes next.
    handle.loadFile("av://lavfi:testsrc=duration=1:size=16x16:rate=1");
    QCOMPARE(handle.pendingAsyncCount(), size_t{0});
    QVERIFY(handle.sourceEpoch() != before);

    // The reply for the old source may still land; draining must discard it.
    pumpUntil(handle, [] { return false; }, 250);
    QCOMPARE(calls, 0);
}

void TestMpvAsync::aPendingReplyIsDroppedWhenPlaybackStops() {
    if (!requireRuntime()) return;
    MpvHandle handle = makeHandle();
    // A source is playing, so stop is a real state change rather than a no-op
    // on an idle core.
    handle.loadFile("av://lavfi:testsrc=duration=1:size=16x16:rate=1");

    int calls = 0;
    const uint64_t before = handle.sourceEpoch();
    QVERIFY(handle
                .getPropertyAsync("mpv-version", MPV_FORMAT_STRING,
                                  [&calls](const AsyncReply&) { ++calls; })
                .valid());
    QCOMPARE(handle.pendingAsyncCount(), size_t{1});

    handle.stop();
    QCOMPARE(handle.pendingAsyncCount(), size_t{0});
    QVERIFY(handle.sourceEpoch() != before);

    pumpUntil(handle, [] { return false; }, 250);
    QCOMPARE(calls, 0);
}

void TestMpvAsync::observerIdsAreUniqueAndUnobserveStopsDelivery() {
    if (!requireRuntime()) return;
    MpvHandle handle = makeHandle();

    int first = 0;
    int second = 0;
    const uint64_t firstId =
        handle.observe("pause", MPV_FORMAT_FLAG, [&first](const Property&) { ++first; });
    const uint64_t secondId =
        handle.observe("pause", MPV_FORMAT_FLAG, [&second](const Property&) { ++second; });
    // Two observers of one property must not share a slot, or one of them
    // swallows the other's notifications.
    QVERIFY(firstId != 0);
    QVERIFY(secondId != firstId);

    handle.setPaused(false);
    handle.setPaused(true);
    QVERIFY(pumpUntil(handle, [&first, &second] { return first >= 1 && second >= 1; }));

    handle.unobserve(firstId);
    // An id that was released is never handed out again, so a notification
    // still in flight cannot land on whoever registers next.
    const uint64_t thirdId =
        handle.observe("pause", MPV_FORMAT_FLAG, [&second](const Property&) { ++second; });
    QVERIFY(thirdId != firstId);
    QVERIFY(thirdId != secondId);

    const int delivered = first;
    handle.setPaused(false);
    handle.setPaused(true);
    pumpUntil(handle, [] { return false; }, 250);
    QCOMPARE(first, delivered);
}

void TestMpvAsync::aMovedHandleStillDrivesTheSameCore() {
    if (!requireRuntime()) return;
    MpvHandle handle = makeHandle();
    mpv_handle* core = handle.raw();

    std::vector<AsyncReply> replies;
    const AsyncRequestId id = handle.getPropertyAsync(
        "mpv-version", MPV_FORMAT_STRING, [&replies](const AsyncReply& reply) { replies.push_back(reply); });
    QVERIFY(id.valid());
    QCOMPARE(handle.pendingAsyncCount(), size_t{1});

    // The identity token travels with the core, so a request made before the
    // move is still recognised and delivered by the handle that now owns it.
    MpvHandle moved = std::move(handle);
    QCOMPARE(moved.raw(), core);
    QVERIFY(!handle);
    QCOMPARE(moved.coreIdentity(), id.identity);
    QCOMPARE(moved.pendingAsyncCount(), size_t{1});

    QVERIFY(pumpUntil(moved, [&replies] { return !replies.empty(); }));
    QCOMPARE(replies.size(), size_t{1});
    QCOMPARE(replies.front().error, 0);
}

void TestMpvAsync::twoCoresKeepTheirRepliesApart() {
    if (!requireRuntime()) return;
    // One module, two cores: that is the main surface and the PiP float, and
    // each core answers only the handle that owns it.
    MpvHandle first = makeHandle();
    MpvHandle second = makeHandle();
    QVERIFY(first.raw() != second.raw());

    std::vector<AsyncReply> firstReplies;
    std::vector<AsyncReply> secondReplies;
    QVERIFY(first
                .getPropertyAsync("mpv-version", MPV_FORMAT_STRING,
                                  [&firstReplies](const AsyncReply& reply) { firstReplies.push_back(reply); })
                .valid());
    QVERIFY(second
                .getPropertyAsync("mpv-version", MPV_FORMAT_STRING,
                                  [&secondReplies](const AsyncReply& reply) { secondReplies.push_back(reply); })
                .valid());

    // Draining one handle pumps only that core's queue, so the other reply must
    // not be delivered by it or retired from the other handle's table.
    QVERIFY(pumpUntil(first, [&firstReplies] { return !firstReplies.empty(); }));
    QCOMPARE(firstReplies.size(), size_t{1});
    QCOMPARE(secondReplies.size(), size_t{0});
    QCOMPARE(first.pendingAsyncCount(), size_t{0});
    QCOMPARE(second.pendingAsyncCount(), size_t{1});

    QVERIFY(pumpUntil(second, [&secondReplies] { return !secondReplies.empty(); }));
    QCOMPARE(secondReplies.size(), size_t{1});
    QCOMPARE(secondReplies.front().error, 0);
    QCOMPARE(firstReplies.front().text, secondReplies.front().text);
}

void TestMpvAsync::aRequestOnAReplacedCoreIsNeverDelivered() {
    if (!requireRuntime()) return;
    MpvHandle handle = makeHandle();
    int calls = 0;
    QVERIFY(handle
                .getPropertyAsync("mpv-version", MPV_FORMAT_STRING,
                                  [&calls](const AsyncReply&) { ++calls; })
                .valid());
    QCOMPARE(handle.pendingAsyncCount(), size_t{1});

    // Replace the core in place. The request belonged to the core that is gone;
    // its callback has to be dropped, not handed to whatever now owns the slot.
    handle = makeHandle();
    QCOMPARE(handle.pendingAsyncCount(), size_t{0});
    pumpUntil(handle, [] { return false; }, 250);
    QCOMPARE(calls, 0);

    // The replacement is fully usable, which is what proves the drop was the
    // stale request being retired rather than the handle dying under it.
    std::vector<AsyncReply> replies;
    QVERIFY(handle
                .getPropertyAsync("mpv-version", MPV_FORMAT_STRING,
                                  [&replies](const AsyncReply& reply) { replies.push_back(reply); })
                .valid());
    QVERIFY(pumpUntil(handle, [&replies] { return !replies.empty(); }));
    QCOMPARE(replies.size(), size_t{1});
    QCOMPARE(replies.front().error, 0);
}

void TestMpvAsync::onlyNewVideoFramesContributeToThroughput_data() {
    QTest::addColumn<qulonglong>("flags");
    QTest::addColumn<bool>("counted");
    QTest::newRow("no-frame") << qulonglong{0} << false;
    QTest::newRow("new-frame") << qulonglong{MPV_RENDER_FRAME_INFO_PRESENT} << true;
    QTest::newRow("redraw")
        << qulonglong{MPV_RENDER_FRAME_INFO_PRESENT | MPV_RENDER_FRAME_INFO_REDRAW} << false;
    QTest::newRow("repeat")
        << qulonglong{MPV_RENDER_FRAME_INFO_PRESENT | MPV_RENDER_FRAME_INFO_REPEAT} << false;
    QTest::newRow("vsync-without-frame")
        << qulonglong{MPV_RENDER_FRAME_INFO_BLOCK_VSYNC} << false;
    QTest::newRow("new-vsynced-frame")
        << qulonglong{MPV_RENDER_FRAME_INFO_PRESENT | MPV_RENDER_FRAME_INFO_BLOCK_VSYNC} << true;
}

void TestMpvAsync::onlyNewVideoFramesContributeToThroughput() {
    QFETCH(qulonglong, flags);
    QFETCH(bool, counted);
    mpv_render_frame_info frame{};
    frame.flags = flags;
    QCOMPARE(isRealNewFrame(frame), counted);
}

void TestMpvAsync::preparationAndWarmupAreExcludedWhenMeasurementRestarts() {
    BenchmarkFrameSample sample;
    sample.recordFrame(21'000'000'000);
    QCOMPARE(sample.frames, 0);
    QCOMPARE(sample.seconds(21'000'000'000), 0.0);
    sample.recordFrame(21'250'000'000);
    sample.recordFrame(21'500'000'000);
    QCOMPARE(sample.frames, 2);
    QCOMPARE(sample.seconds(21'500'000'000), 0.5);

    sample.reset();
    QVERIFY(!sample.started());
    QCOMPARE(sample.frames, 0);
    QCOMPARE(sample.seconds(80'000'000'000), 0.0);
    sample.recordFrame(81'000'000'000);
    QCOMPARE(sample.frames, 0);
    sample.recordFrame(81'500'000'000);
    QCOMPARE(sample.frames, 1);
    QCOMPARE(sample.seconds(82'000'000'000), 1.0);
}

// ── runtime selection for the transport tests ──────────────────────────────

bool TestMpvAsync::requireRuntime() {
    if (MpvApi::instance().loaded()) return true;
    if (!selectionError_.isEmpty()) {
        // initTestCase already tried, and the loader's own reason is what says
        // whether the module is missing, corrupt, or incompatible.
        QTest::qFail(qPrintable(QStringLiteral("cannot select %1: %2")
                                    .arg(envBundle().modulePath, selectionError_)),
                     __FILE__, __LINE__);
        return false;
    }
    QTest::qSkip("set MIKAI_MPV_MODULE to the libmpv-2.dll of a real runtime to run this",
                 __FILE__, __LINE__);
    return false;
}

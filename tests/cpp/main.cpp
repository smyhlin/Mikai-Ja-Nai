// One runner for every suite, so ctest has a single hermetic command to run.
//
// QTest normally generates one main() per class. That is fine for six separate
// executables and useless for one ctest entry, so the suites are driven by hand
// here and the first failure decides the exit code.

#include "TestAnimeJanaiRuntime.hpp"
#include "TestAnime4K.hpp"
#include "TestBenchmarkReport.hpp"
#include "TestI18n.hpp"
#include "TestThemeIcons.hpp"
#include "TestLayering.hpp"
#include "TestWatchHistory.hpp"
#include "TestWindowRaise.hpp"
#include "TestLiveOracle.hpp"
#include "TestSystemIntegration.hpp"
#include "TestModels.hpp"
#include "TestUpdateFeed.hpp"
#include "TestPublicApiUi.hpp"
#include "TestPagers.hpp"
#include "TestQualityCache.hpp"
#include "TestRateLimiter.hpp"
#include "TestReleases.hpp"
#include "TestDownloads.hpp"
#include "TestDiagnostics.hpp"
#include "TestApiClient.hpp"
#include "TestFeedRender.hpp"
#include "TestRunner.hpp"
#include "TestStreams.hpp"
#include "TestMpvAsync.hpp"
#include "TestCompareGif.hpp"
#include "TestCompareClip.hpp"
#include "TestCompareLoading.hpp"
#include "TestNativeAssets.hpp"
#include "TestNativeUserRecipes.hpp"
#include "TestNativeBundle.hpp"
#include "TestNativeBuilder.hpp"
#include "TestNativeComponents.hpp"
#include "TestNativePreparationReferences.hpp"
#include "TestNativeStoredModels.hpp"
#include "TestNativePreparedPlan.hpp"
#include "TestNativePlaybackController.hpp"
#include "TestNativeManifest.hpp"
#include "TestNativeMenu.hpp"
#include "TestNativePrefs.hpp"
#include "TestNativePrepPlan.hpp"
#include "TestNativeRecipe.hpp"
#include "TestNativeProfiles.hpp"
#include "TestNativeRife.hpp"
#include "TestNativeStatus.hpp"
#include "TestPlaybackDiagnostics.hpp"
#include "TestMediaInformation.hpp"
#include "TestHdrRenderContracts.hpp"
#include "TestNativeTypes.hpp"
#include "TestRuntimeSelection.hpp"
#ifdef MIKAI_WITH_MODEL_INSPECTION
#include "TestNativeCatalog.hpp"
#endif

#include <cstdio>
#include <vector>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTemporaryFile>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

// Regression suites backed by their own runner functions rather than a
// main()-driven QObject, because they need per-suite setup/teardown around the
// QTest call. Declared here so kSuites and the executable set stay in one place.
int runTestDownloads(int argc, char** argv);
int runTestFeedRender(int argc, char** argv);
int runTestDiagnostics(int argc, char** argv);
int runTestApiClient(int argc, char** argv);
int runTestRunner(int argc, char** argv);

namespace {

constexpr const char* kSuites[] = {
    "TestAnimeJanaiRuntime",
    "TestRuntimeSelection", "TestNativeTypes", "TestNativeRecipe", "TestNativeProfiles", "TestNativeRife",
    "TestNativeBundle", "TestNativePrepPlan", "TestNativeManifest", "TestNativeBuilder",
    "TestNativePreparationReferences", "TestNativeComponents", "TestNativeStoredModels", "TestNativePreparedPlan", "TestNativePlaybackController", "TestNativeStatus",
    "TestPlaybackDiagnostics", "TestMediaInformation", "TestHdrRenderContracts",
    "TestNativePrefs",
    "TestNativeMenu", "TestNativeUserRecipes", "TestNativeAssets", "TestMpvAsync", "TestCompareGif",
    "TestCompareClip", "TestCompareLoading",
#ifdef MIKAI_WITH_MODEL_INSPECTION
    "TestNativeCatalog",
#endif
    "TestModels", "TestReleases", "TestAnime4K", "TestBenchmarkReport", "TestI18n",
    "TestThemeIcons",
    "TestStreams", "TestPagers",
    "TestQualityCache", "TestRateLimiter", "TestWatchHistory", "TestDownloads", "TestDiagnostics",
    "TestApiClient", "TestFeedRender", "TestUpdateFeed", "TestPublicApiUi", "TestRunner",
    "TestLayering", "TestWindowRaise", "TestSystemIntegration", "TestClipboard", "TestLiveOracle"};

bool nativeFixtureRootIsSafe() {
  const QString path = QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("native"));
#ifdef Q_OS_WIN
  const DWORD attributes = GetFileAttributesW(
      reinterpret_cast<LPCWSTR>(path.utf16()));
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
  } else if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
             (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    const QDir directory(path);
    if (directory.isReadable() &&
        directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                            QDir::NoDotAndDotDot).isEmpty()) return true;
  }
#else
  const QFileInfo info(path);
  if (!info.exists() && !info.isSymLink()) return true;
  const QDir directory(path);
  if (info.isDir() && !info.isSymLink() && directory.isReadable() &&
      directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                          QDir::NoDotAndDotDot).isEmpty()) return true;
#endif
  std::fprintf(stderr,
               "[mikai_tests] refusing native fixture mutation beside an existing or unsafe "
               "native tree: %s; run through CTest's isolated launcher instead\n",
               qPrintable(path));
  return false;
}

bool liveOracleAdmitted() {
  return qEnvironmentVariableIntValue("MIKAI_LIVE") == 1 &&
         !qEnvironmentVariable("MIKAI_API_KEY").isEmpty();
}

int runOne(const char* name, QObject* suite, const QStringList& arguments, bool admitted = true) {
  QStringList suiteArguments = arguments;
  QTemporaryFile report;
  const bool captureReport = !arguments.contains(QStringLiteral("-o"));
  if (captureReport) {
    if (!report.open()) {
      std::fprintf(stderr, "[mikai_tests] cannot create report for %s\n", name);
      return 2;
    }
    report.close();
    suiteArguments << QStringLiteral("-o") << report.fileName() + QStringLiteral(",txt");
  }
  const int rc = QTest::qExec(suite, suiteArguments);
  if (captureReport && rc != 0) {
    QFile output(report.fileName());
    if (output.open(QIODevice::ReadOnly)) {
      const QByteArray text = output.readAll();
      std::fwrite(text.constData(), 1, static_cast<size_t>(text.size()), stderr);
    } else {
      std::fprintf(stderr, "[mikai_tests] cannot read report for %s\n", name);
    }
  }
  const char* verdict = rc != 0 ? "FAIL" : (admitted ? "PASS" : "SKIP");
  std::fprintf(stderr, "[mikai_tests] %s: %s (rc=%d)\n", name, verdict, rc);
  std::fflush(stderr);
  return rc;
}

QString selectedSuite(const QStringList& arguments) {
  if (arguments.size() < 2) return {};
  const QString& candidate = arguments.at(1);
  for (const char* suite : kSuites) {
    if (candidate == QString::fromLatin1(suite)) return candidate;
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  // The pagers build real widgets; offscreen keeps ctest usable on a headless
  // agent without a desktop session.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  // Application names, settings paths and cwd do not change nativeRoot().
  // Refuse before constructing any suite or creating/writing any fixture.
  if (!nativeFixtureRootIsSafe()) return 2;
  QApplication::setApplicationName(QStringLiteral("mikai_tests"));
  QApplication::setOrganizationName(QStringLiteral("Mikai"));
  // This belongs to the runner, not whichever suite happens to run first.
  QStandardPaths::setTestModeEnabled(true);
  QTemporaryDir settingsDirectory;
  if (!settingsDirectory.isValid()) {
    std::fprintf(stderr, "[mikai_tests] cannot create isolated settings directory\n");
    return 2;
  }
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());


  // The isolated launcher forwards suite/QTest arguments unchanged. A safe
  // direct invocation can also select a suite: `./mikai_tests TestStreams`.
  QStringList arguments;
  arguments.reserve(argc + 1);
  arguments << QString::fromLocal8Bit(argv[0]);
  for (int i = 1; i < argc; ++i) {
    arguments << QString::fromLocal8Bit(argv[i]);
  }

  TestAnimeJanaiRuntime animeJanaiRuntime;
  TestRuntimeSelection runtimeSelection;
  TestNativeTypes nativeTypes;
  TestNativeRecipe nativeRecipe;
  TestNativeProfiles nativeProfiles;
  TestNativeRife nativeRife;
  TestNativeBundle nativeBundle;
  TestNativePrepPlan nativePrepPlan;
  TestNativeManifest nativeManifest;
  TestNativeBuilder nativeBuilder;
  TestNativePreparationReferences nativePreparationReferences;
  TestNativeComponents nativeComponents;
  TestNativeStoredModels nativeStoredModels;
  TestNativePreparedPlan nativePreparedPlan;
  TestNativePlaybackController nativePlaybackController;
  TestNativeStatus nativeStatus;
  TestPlaybackDiagnostics playbackDiagnostics;
  TestMediaInformation mediaInformation;
  TestHdrRenderContracts hdrRenderContracts;
  TestNativePrefs nativePrefs;
  TestNativeMenu nativeMenu;
  TestNativeUserRecipes nativeUserRecipes;
  TestNativeAssets nativeAssets;
  TestMpvAsync mpvAsync;
  TestCompareGif compareGif;
  TestCompareClip compareClip;
  TestCompareLoading compareLoading;
#ifdef MIKAI_WITH_MODEL_INSPECTION
  TestNativeCatalog nativeCatalog;
#endif
  TestModels models;
  TestReleases releases;
  TestAnime4K anime4k;
  TestBenchmarkReport benchmarkReport;
  TestI18n i18n;
  TestThemeIcons themeIcons;
  TestStreams streams;
  TestPagers pagers;
  TestQualityCache qualityCache;
  TestWatchHistory watchHistory;
  TestRateLimiter rateLimiter;
  TestLayering layering;
  TestWindowRaise windowRaise;
  TestClipboard clipboard;
  TestSystemIntegration systemIntegration;
  TestLiveOracle liveOracle;

  // A suite name on the command line runs only that suite; otherwise every
  // suite runs, because plain `mikai_tests` under ctest must exercise all of it.
  // QTest treats an unconsumed argv entry as a function name to run. A suite
  // name is ours, not its, so it is stripped before the suites see the list;
  // QTest still gets argv[0], which it requires.
  // Only the FIRST positional argument can be a suite selector. Later ones are
  // QTest's own (a function name, -v2, -o ...), and stripping a coincidence
  // there would silently unfilter the run: `-v2` is safe today only because no
  // suite is called that, which is luck, not a rule.
  //
  QStringList qtestArgs;
  qtestArgs.reserve(arguments.size());
  const QString selected = selectedSuite(arguments);
  if (arguments.size() > 1 && arguments.at(1).startsWith(QStringLiteral("Test")) &&
      selected.isEmpty()) {
    std::fprintf(stderr, "[mikai_tests] unknown suite: %s\n",
                 qPrintable(arguments.at(1)));
    return 2;
  }
  qtestArgs << arguments.first();
  for (int i = 1; i < arguments.size(); ++i) {
    if (i == 1 && !selected.isEmpty()) continue;
    qtestArgs << arguments.at(i);
  }
  const bool filtered = !selected.isEmpty();
  int failures = 0;
  // `admitted` carries a suite's own gate: false means the suite is about to
  // skip itself, and the verdict has to say so rather than claim a pass.
  QSet<QString> executed;
  const auto run = [&](const char* name, QObject* suite, bool admitted) {
    executed.insert(QString::fromLatin1(name));
    if (!filtered || selected == QString::fromLatin1(name)) {
      failures |= runOne(name, suite, qtestArgs, admitted);
    }
  };
  // Function-backed suites: the suite owns its own report capture and verdict
  // line, so this records the same execution evidence a QObject suite gets -
  // without it the name would be selectable but never run. The C-string vector
  // is rebuilt from qtestArgs so the wrapper sees QTest's arguments exactly as
  // a QObject suite does, with the suite selector already stripped out.
  std::vector<QByteArray> wrapperArgStorage;
  wrapperArgStorage.reserve(static_cast<size_t>(qtestArgs.size()));
  for (const QString& argument : qtestArgs) wrapperArgStorage.push_back(argument.toLocal8Bit());
  std::vector<char*> wrapperArgv;
  wrapperArgv.reserve(wrapperArgStorage.size() + 1);
  for (QByteArray& storage : wrapperArgStorage) wrapperArgv.push_back(storage.data());
  wrapperArgv.push_back(nullptr);
  // Wrapper suites own their report capture; the verdict line is the runner's to
  // write, exactly as for an object suite. Without it a wrapper suite runs
  // silently: a pass is indistinguishable from a suite that never executed, which
  // is what hid TestFeedRender's results.
  const auto runWrapper = [&](const char* name, int (*wrapper)(int, char**)) {
    executed.insert(QString::fromLatin1(name));
    if (!filtered || selected == QString::fromLatin1(name)) {
      const int rc = wrapper(static_cast<int>(qtestArgs.size()), wrapperArgv.data());
      failures |= rc;
      const char* verdict = rc != 0 ? "FAIL" : "PASS";
      std::fprintf(stderr, "[mikai_tests] %s: %s (rc=%d)\n", name, verdict, rc);
      std::fflush(stderr);
    }
  };
  run("TestAnimeJanaiRuntime", &animeJanaiRuntime, true);
  run("TestRuntimeSelection", &runtimeSelection, true);
  run("TestNativeTypes", &nativeTypes, true);
  run("TestNativeRecipe", &nativeRecipe, true);
  run("TestNativeProfiles", &nativeProfiles, true);
  run("TestNativeRife", &nativeRife, true);
  run("TestNativeBundle", &nativeBundle, true);
  run("TestNativePrepPlan", &nativePrepPlan, true);
  run("TestNativeManifest", &nativeManifest, true);
  run("TestNativePreparationReferences", &nativePreparationReferences, true);
  run("TestNativeComponents", &nativeComponents, true);
  run("TestNativeStoredModels", &nativeStoredModels, true);
  run("TestNativeBuilder", &nativeBuilder, true);
  run("TestNativePreparedPlan", &nativePreparedPlan, true);
  run("TestNativePlaybackController", &nativePlaybackController, true);
  run("TestNativeStatus", &nativeStatus, true);
  run("TestPlaybackDiagnostics", &playbackDiagnostics, true);
  run("TestMediaInformation", &mediaInformation, true);
  run("TestHdrRenderContracts", &hdrRenderContracts, true);
  run("TestNativePrefs", &nativePrefs, true);
  run("TestNativeMenu", &nativeMenu, true);
  run("TestNativeUserRecipes", &nativeUserRecipes, true);
  run("TestNativeAssets", &nativeAssets, true);
  run("TestMpvAsync", &mpvAsync, true);
  run("TestCompareGif", &compareGif, true);
  run("TestCompareClip", &compareClip, true);
  run("TestCompareLoading", &compareLoading, true);
  runWrapper("TestDownloads", runTestDownloads);
  runWrapper("TestDiagnostics", runTestDiagnostics);
  runWrapper("TestApiClient", runTestApiClient);
  runWrapper("TestFeedRender", runTestFeedRender);
  runWrapper("TestUpdateFeed", runTestUpdateFeed);
  runWrapper("TestPublicApiUi", runTestPublicApiUi);
  runWrapper("TestRunner", runTestRunner);
#ifdef MIKAI_WITH_MODEL_INSPECTION
  run("TestNativeCatalog", &nativeCatalog, true);
#endif
  run("TestModels", &models, true);
  run("TestReleases", &releases, true);
  run("TestAnime4K", &anime4k, true);
  run("TestBenchmarkReport", &benchmarkReport, true);
  run("TestI18n", &i18n, true);
  run("TestThemeIcons", &themeIcons, true);
  run("TestStreams", &streams, true);
  run("TestPagers", &pagers, true);
  run("TestQualityCache", &qualityCache, true);
  run("TestWatchHistory", &watchHistory, true);
  run("TestRateLimiter", &rateLimiter, true);
  run("TestLayering", &layering, true);
  run("TestSystemIntegration", &systemIntegration, true);
  run("TestWindowRaise", &windowRaise, true);
  run("TestClipboard", &clipboard, true);
  // Last, and off unless MIKAI_LIVE=1 with a key: it needs the network and the
  // quota, and a hermetic run must not be recorded as having exercised either.
  run("TestLiveOracle", &liveOracle, liveOracleAdmitted());

  // Every name the filter knows must have been handed a suite. A name in
  // `kSuites` with no `run()` call is invisible - it is stripped from QTest's
  // arguments and then never executed - which is how a whole suite can stop
  // running without a single failing line. Reported as a failure rather than a
  // warning, because a suite nobody runs is a suite nobody can trust.
  if (!filtered) {
    for (const char* suite : kSuites) {
      if (executed.contains(QString::fromLatin1(suite))) continue;
      std::fprintf(stderr, "[mikai_tests] %s: registered but never executed\n", suite);
      failures |= 1;
    }
  }
  return failures;
}

// One runner for every suite, so ctest has a single hermetic command to run.
//
// QTest normally generates one main() per class. That is fine for six separate
// executables and useless for one ctest entry, so the suites are driven by hand
// here and the first failure decides the exit code.

#include "TestAnimeJanaiRuntime.hpp"
#include "TestAnime4K.hpp"
#include "TestI18n.hpp"
#include "TestLayering.hpp"
#include "TestLiveOracle.hpp"
#include "TestModels.hpp"
#include "TestPagers.hpp"
#include "TestQualityCache.hpp"
#include "TestRateLimiter.hpp"
#include "TestReleases.hpp"
#include "TestStreams.hpp"
#include "TestMpvAsync.hpp"
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
#include "TestNativeTypes.hpp"
#include "TestRuntimeSelection.hpp"
#ifdef MIKAI_WITH_MODEL_INSPECTION
#include "TestNativeCatalog.hpp"
#endif

#include <cstdio>

#include <QtCore/QFile>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTemporaryFile>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>

namespace {

constexpr const char* kSuites[] = {
    "TestAnimeJanaiRuntime",
    "TestRuntimeSelection", "TestNativeTypes", "TestNativeRecipe", "TestNativeProfiles", "TestNativeRife",
    "TestNativeBundle", "TestNativePrepPlan", "TestNativeManifest", "TestNativeBuilder",
    "TestNativePreparationReferences", "TestNativeComponents", "TestNativeStoredModels", "TestNativePreparedPlan", "TestNativePlaybackController", "TestNativeStatus",
    "TestPlaybackDiagnostics",
    "TestNativePrefs",
    "TestNativeMenu", "TestNativeUserRecipes", "TestNativeAssets", "TestMpvAsync",
#ifdef MIKAI_WITH_MODEL_INSPECTION
    "TestNativeCatalog",
#endif
    "TestModels", "TestReleases", "TestAnime4K", "TestI18n", "TestStreams", "TestPagers",
    "TestQualityCache", "TestRateLimiter", "TestLayering", "TestClipboard", "TestLiveOracle"};

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

bool wantsSuite(const QStringList& arguments, const char* name) {
  for (const QString& arg : arguments) {
    if (arg == QString::fromLatin1(name)) {
      return true;
    }
  }
  return false;
}

bool suiteFilterActive(const QStringList& arguments) {
  for (const char* suite : kSuites) {
    if (wantsSuite(arguments, suite)) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  // The pagers build real widgets; offscreen keeps ctest usable on a headless
  // agent without a desktop session.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("mikai_tests"));
  QApplication::setOrganizationName(QStringLiteral("Mikai"));
  QTemporaryDir settingsDirectory;
  if (!settingsDirectory.isValid()) {
    std::fprintf(stderr, "[mikai_tests] cannot create isolated settings directory\n");
    return 2;
  }
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());


  // Whatever ctest passes past the test name is forwarded to QTest, so a single
  // suite can still be run by name: ctest -R / `./mikai_tests TestStreams`.
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
  TestNativePrefs nativePrefs;
  TestNativeMenu nativeMenu;
  TestNativeUserRecipes nativeUserRecipes;
  TestNativeAssets nativeAssets;
  TestMpvAsync mpvAsync;
#ifdef MIKAI_WITH_MODEL_INSPECTION
  TestNativeCatalog nativeCatalog;
#endif
  TestModels models;
  TestReleases releases;
  TestAnime4K anime4k;
  TestI18n i18n;
  TestStreams streams;
  TestPagers pagers;
  TestQualityCache qualityCache;
  TestRateLimiter rateLimiter;
  TestLayering layering;
  TestClipboard clipboard;
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
  QStringList qtestArgs;
  qtestArgs.reserve(arguments.size());
  qtestArgs << arguments.first();
  {
    bool firstIsSuite = false;
    if (arguments.size() > 1) {
      for (const char* suite : kSuites) {
        if (arguments.at(1) == QString::fromLatin1(suite)) {
          firstIsSuite = true;
          break;
        }
      }
    }
    for (int i = 1; i < arguments.size(); ++i) {
      if (i == 1 && firstIsSuite) {
        continue;
      }
      qtestArgs << arguments.at(i);
    }
  }
  const bool filtered = suiteFilterActive(arguments);
  int failures = 0;
  // `admitted` carries a suite's own gate: false means the suite is about to
  // skip itself, and the verdict has to say so rather than claim a pass.
  QSet<QString> executed;
  const auto run = [&](const char* name, QObject* suite, bool admitted) {
    executed.insert(QString::fromLatin1(name));
    if (!filtered || wantsSuite(arguments, name)) {
      failures |= runOne(name, suite, qtestArgs, admitted);
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
  run("TestNativePrefs", &nativePrefs, true);
  run("TestNativeMenu", &nativeMenu, true);
  run("TestNativeUserRecipes", &nativeUserRecipes, true);
  run("TestNativeAssets", &nativeAssets, true);
  run("TestMpvAsync", &mpvAsync, true);
#ifdef MIKAI_WITH_MODEL_INSPECTION
  run("TestNativeCatalog", &nativeCatalog, true);
#endif
  run("TestModels", &models, true);
  run("TestReleases", &releases, true);
  run("TestAnime4K", &anime4k, true);
  run("TestI18n", &i18n, true);
  run("TestStreams", &streams, true);
  run("TestPagers", &pagers, true);
  run("TestQualityCache", &qualityCache, true);
  run("TestRateLimiter", &rateLimiter, true);
  run("TestLayering", &layering, true);
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

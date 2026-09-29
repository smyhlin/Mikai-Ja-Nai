// One runner for every suite, so ctest has a single hermetic command to run.
//
// QTest normally generates one main() per class. That is fine for six separate
// executables and useless for one ctest entry, so the suites are driven by hand
// here and the first failure decides the exit code.

#include "TestAnime4K.hpp"
#include "TestLayering.hpp"
#include "TestLiveOracle.hpp"
#include "TestModels.hpp"
#include "TestPagers.hpp"
#include "TestQualityCache.hpp"
#include "TestRateLimiter.hpp"
#include "TestReleases.hpp"
#include "TestStreams.hpp"

#include <cstdio>

#include <QtCore/QStringList>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>

namespace {

int runOne(const char* name, QObject* suite, const QStringList& arguments) {
  const int rc = QTest::qExec(suite, arguments);
  std::fprintf(stderr, "[mikai_tests] %s: %s (rc=%d)\n", name, rc == 0 ? "PASS" : "FAIL", rc);
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
  static const char* kSuites[] = {"TestModels",   "TestReleases",     "TestAnime4K",
                                  "TestStreams",  "TestPagers",       "TestQualityCache",
                                  "TestRateLimiter", "TestLayering",  "TestClipboard",
                                  "TestLiveOracle"};
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

  // Whatever ctest passes past the test name is forwarded to QTest, so a single
  // suite can still be run by name: ctest -R / `./mikai_tests TestStreams`.
  QStringList arguments;
  arguments.reserve(argc + 1);
  arguments << QString::fromLocal8Bit(argv[0]);
  for (int i = 1; i < argc; ++i) {
    arguments << QString::fromLocal8Bit(argv[i]);
  }

  TestModels models;
  TestReleases releases;
  TestAnime4K anime4k;
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
      static const char* kSuites[] = {"TestModels",  "TestReleases",  "TestAnime4K",
                                      "TestPagers",  "TestQualityCache", "TestRateLimiter",
                                      "TestLayering", "TestClipboard", "TestLiveOracle"};
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
#define MIKAI_RUN_ONE(name, suite)                                    \
  do {                                                                \
    if (!filtered || wantsSuite(arguments, name)) {                    \
      failures |= runOne(name, &(suite), qtestArgs);                   \
    }                                                                 \
  } while (0)
  MIKAI_RUN_ONE("TestModels", models);
  MIKAI_RUN_ONE("TestReleases", releases);
  MIKAI_RUN_ONE("TestAnime4K", anime4k);
  MIKAI_RUN_ONE("TestStreams", streams);
  MIKAI_RUN_ONE("TestPagers", pagers);
  MIKAI_RUN_ONE("TestQualityCache", qualityCache);
  MIKAI_RUN_ONE("TestRateLimiter", rateLimiter);
  MIKAI_RUN_ONE("TestLayering", layering);
  MIKAI_RUN_ONE("TestClipboard", clipboard);
  // Last, and skipped unless MIKAI_LIVE=1: it needs the network and the quota.
  MIKAI_RUN_ONE("TestLiveOracle", liveOracle);
#undef MIKAI_RUN_ONE
  return failures;
}

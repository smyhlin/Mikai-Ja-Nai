#include "TestAnime4K.hpp"

#include "Anime4K.hpp"

#include <QtCore/QDir>
#include <QtCore/QSet>
#include <QtCore/QFileInfo>

using mplayer::anime4k::allCandidates;
using mplayer::anime4k::candidateForId;
using mplayer::anime4k::chainIsComplete;
using mplayer::anime4k::filesForId;
using mplayer::anime4k::findShadersDir;
using mplayer::anime4k::Kind;
using mplayer::anime4k::Mode;
using mplayer::anime4k::modeId;
using mplayer::anime4k::shaderStringForCandidate;
using mplayer::anime4k::shaderStringForMode;

void TestAnime4K::everyCandidateCarriesAnIdAndALabel() {
  const QVector<mplayer::anime4k::Candidate> all = allCandidates();
  QVERIFY(!all.isEmpty());
  for (const mplayer::anime4k::Candidate& candidate : all) {
    QVERIFY2(!candidate.id.isEmpty(), qPrintable(QStringLiteral("unnamed candidate: ") +
                                                 candidate.label));
    QVERIFY2(!candidate.label.isEmpty(), qPrintable(QStringLiteral("id with no label: ") +
                                                   candidate.id));
  }
}

void TestAnime4K::presetsComeFirstAndAreMarkedAsPresets() {
  const QVector<mplayer::anime4k::Candidate> all = allCandidates();
  QVERIFY(!all.isEmpty());
  QVERIFY(all.first().id == QLatin1String("off"));
  QVERIFY(all.first().kind == Kind::Preset);

  int presets = 0;
  for (const mplayer::anime4k::Candidate& candidate : all) {
    if (candidate.kind == Kind::Preset) ++presets;
  }
  // Ten, not nine: Off is a candidate too, because with nothing else picked it
  // is the honest baseline to compare against.
  QCOMPARE(presets, 10);
}

void TestAnime4K::individualShadersAreGroupedByKind() {
  const QVector<mplayer::anime4k::Candidate> all = allCandidates();
  bool sawRestore = false;
  bool sawUpscale = false;
  for (const mplayer::anime4k::Candidate& candidate : all) {
    if (candidate.kind == Kind::Restore) {
      sawRestore = true;
      QCOMPARE(candidate.files.size(), 1);
    }
    if (candidate.kind == Kind::Upscale) sawUpscale = true;
  }
  QVERIFY(sawRestore);
  QVERIFY(sawUpscale);
}

void TestAnime4K::offIsACandidateWithNoFiles() {
  const mplayer::anime4k::Candidate off = candidateForId(QStringLiteral("off"));
  QVERIFY(off.files.isEmpty());
  QVERIFY(off.id == QLatin1String("off"));
  QVERIFY(shaderStringForCandidate(QStringLiteral("off")).isEmpty());
}

void TestAnime4K::anUnknownIdResolvesToNothing() {
  // The load-bearing case: a stale setting must not quietly resolve to some
  // other chain, which would apply a filter the viewer never chose.
  QVERIFY(filesForId(QStringLiteral("no-such-chain")).isEmpty());
  QVERIFY(shaderStringForCandidate(QStringLiteral("Anime4K_Nope.glsl")).isEmpty());
  QVERIFY(candidateForId(QStringLiteral("nope")).id.isEmpty());
}

void TestAnime4K::anEmptyIdResolvesToNothing() {
  QVERIFY(filesForId(QString()).isEmpty());
  QVERIFY(shaderStringForCandidate(QString()).isEmpty());
}

void TestAnime4K::everyPresetResolvesToFilesThatExist() {
  const QString dir = findShadersDir();
  QVERIFY2(!dir.isEmpty(), "the shaders folder must sit beside the executable");
  for (const mplayer::anime4k::ModeInfo& info : mplayer::anime4k::availableModes()) {
    if (info.mode == Mode::Off) continue;
    const QStringList files = filesForId(info.id, dir);
    QVERIFY2(!files.isEmpty(), qPrintable(info.id));
    for (const QString& file : files) {
      QVERIFY2(QFileInfo::exists(QDir(dir).filePath(file)),
               qPrintable(info.id + QStringLiteral(" names a file that is not there: ") + file));
    }
  }
}

void TestAnime4K::aSingleShaderResolvesToItself() {
  const QString dir = findShadersDir();
  QVERIFY(!dir.isEmpty());
  const QString first =
      QDir(dir).entryList({QStringLiteral("*.glsl")}, QDir::Files, QDir::Name).value(0);
  QVERIFY(!first.isEmpty());
  const QStringList files = filesForId(first, dir);
  QCOMPARE(files.size(), 1);
  QCOMPARE(files.first(), first);
  QVERIFY(chainIsComplete(candidateForId(first, dir), dir));
}

void TestAnime4K::theShadersDirectoryIsFoundBesideTheExecutable() {
  const QString dir = findShadersDir();
  QVERIFY(!dir.isEmpty());
  QVERIFY(QFileInfo(dir).isDir());
  QVERIFY(QFileInfo::exists(QDir(dir).filePath(QStringLiteral("Anime4K_Clamp_Highlights.glsl"))));
}

void TestAnime4K::aMissingDirectoryResolvesNothing() {
  // Pointed at a path that cannot exist: the presets still enumerate (they are
  // a table in code) but no chain can be built, so nothing claims to work.
  const QString absent = QStringLiteral("Z:/definitely-not-here/shaders");
  QVERIFY(shaderStringForCandidate(QStringLiteral("hq_ca"), absent).isEmpty());
  QVERIFY(!chainIsComplete(candidateForId(QStringLiteral("hq_ca")), absent));
}

void TestAnime4K::chainIsCompleteRejectsAnAbsentFile() {
  // A chain naming a deleted shader still reads back complete from mpv, which
  // echoes glsl-shaders without validating it. This is the only check that can
  // tell the two apart, so it has to fail on a missing file.
  const QString dir = findShadersDir();
  QVERIFY(!dir.isEmpty());
  mplayer::anime4k::Candidate candidate;
  candidate.id = QStringLiteral("fake");
  candidate.files = {QStringLiteral("Anime4K_DoesNotExist.glsl")};
  QVERIFY(!chainIsComplete(candidate, dir));

  // A folder that does not exist cannot satisfy a file either, which is why an
  // absent directory is reported as incomplete rather than as "nothing to
  // check" - a chain that names nothing must never read back as runnable.
  QVERIFY(!chainIsComplete(candidate, QStringLiteral("Z:/definitely-not-here")));

  mplayer::anime4k::Candidate real;
  real.id = QStringLiteral("hq_ca");
  real.files = filesForId(QStringLiteral("hq_ca"), dir);
  QVERIFY(chainIsComplete(real, dir));
}

void TestAnime4K::theChainStringJoinsWithThePlatformSeparator() {
  const QString dir = findShadersDir();
  const QString chain = shaderStringForCandidate(QStringLiteral("hq_ca"), dir);
  QVERIFY(!chain.isEmpty());
#ifdef Q_OS_WIN
  const QChar separator = QLatin1Char(';');
#else
  const QChar separator = QLatin1Char(':');
#endif
  const QStringList paths = chain.split(separator);
  QCOMPARE(paths.size(), filesForId(QStringLiteral("hq_ca"), dir).size());
  for (const QString& path : paths) {
    QVERIFY(QFileInfo::exists(path));
  }
}

void TestAnime4K::modeIdIsTheStoredKeyNotTheOrdinal() {
  // The stored value has to survive a preset being added or reordered, so it is
  // the name and never the enum position.
  QVERIFY(modeId(Mode::FastA) == QLatin1String("fast_a"));
  QVERIFY(modeId(Mode::HqCA) == QLatin1String("hq_ca"));
  QVERIFY(modeId(Mode::Off) == QLatin1String("off"));
  for (const mplayer::anime4k::ModeInfo& info : mplayer::anime4k::availableModes()) {
    QVERIFY2(!info.id.isEmpty(), "every mode needs a stored key");
  }
}

void TestAnime4K::everyModeHasAnId() {
  const QVector<mplayer::anime4k::ModeInfo> modes = mplayer::anime4k::availableModes();
  QVERIFY(!modes.isEmpty());
  QSet<QString> seen;
  for (const mplayer::anime4k::ModeInfo& info : modes) {
    QVERIFY(!info.id.isEmpty());
    QVERIFY(!info.label.isEmpty());
    // A duplicate id would make one preset unreachable and quietly repoint the
    // other, which is exactly the failure the id exists to prevent.
    QVERIFY2(!seen.contains(info.id), qPrintable(QStringLiteral("duplicate id: ") + info.id));
    seen.insert(info.id);
  }
}

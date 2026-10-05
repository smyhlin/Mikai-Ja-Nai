#include "TestAnime4K.hpp"

#include "Anime4K.hpp"
#include "native/status/NativeMenu.hpp"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <algorithm>

using mplayer::anime4k::allCandidates;
using mplayer::anime4k::availableModes;
using mplayer::anime4k::candidateForId;
using mplayer::anime4k::categoryContainsPreset;
using mplayer::anime4k::categoryLabel;
using mplayer::anime4k::chainIsComplete;
using mplayer::anime4k::filesForId;
using mplayer::anime4k::findShadersDir;
using mplayer::anime4k::Kind;
using mplayer::anime4k::menuRows;
using mplayer::anime4k::modeForId;
using mplayer::anime4k::modeId;
using mplayer::anime4k::shaderStringForCandidate;
using mplayer::anime4k::Mode;
using mplayer::anime4k::ModeInfo;
using mplayer::anime4k::MenuRow;

namespace {

QStringList shaderFiles(std::initializer_list<const char*> stems) {
  QStringList files;
  for (const char* stem : stems) {
    files.append(QStringLiteral("Anime4K_%1.glsl").arg(QString::fromLatin1(stem)));
  }
  return files;
}

QVector<QPair<QString, QStringList>> originalChains() {
  return {
      {QStringLiteral("off"), {}},
      {QStringLiteral("fast_a"), shaderFiles({"Clamp_Highlights", "Restore_CNN_M",
                                               "Upscale_CNN_x2_M", "AutoDownscalePre_x2",
                                               "AutoDownscalePre_x4", "Upscale_CNN_x2_S"})},
      {QStringLiteral("fast_b"), shaderFiles({"Clamp_Highlights", "Restore_CNN_Soft_M",
                                               "Upscale_CNN_x2_M", "AutoDownscalePre_x2",
                                               "AutoDownscalePre_x4", "Upscale_CNN_x2_S"})},
      {QStringLiteral("fast_c"), shaderFiles({"Clamp_Highlights", "Upscale_Denoise_CNN_x2_M",
                                               "AutoDownscalePre_x2", "AutoDownscalePre_x4",
                                               "Upscale_CNN_x2_S"})},
      {QStringLiteral("hq_a"), shaderFiles({"Clamp_Highlights", "Restore_CNN_VL",
                                             "Upscale_CNN_x2_VL", "AutoDownscalePre_x2",
                                             "AutoDownscalePre_x4", "Upscale_CNN_x2_M"})},
      {QStringLiteral("hq_b"), shaderFiles({"Clamp_Highlights", "Restore_CNN_Soft_VL",
                                             "Upscale_CNN_x2_VL", "AutoDownscalePre_x2",
                                             "AutoDownscalePre_x4", "Upscale_CNN_x2_M"})},
      {QStringLiteral("hq_c"), shaderFiles({"Clamp_Highlights", "Upscale_Denoise_CNN_x2_VL",
                                             "AutoDownscalePre_x2", "AutoDownscalePre_x4",
                                             "Upscale_CNN_x2_M"})},
      {QStringLiteral("hq_aa"), shaderFiles({"Clamp_Highlights", "Restore_CNN_VL",
                                              "Upscale_CNN_x2_VL", "Restore_CNN_M",
                                              "AutoDownscalePre_x2", "AutoDownscalePre_x4",
                                              "Upscale_CNN_x2_M"})},
      {QStringLiteral("hq_bb"), shaderFiles({"Clamp_Highlights", "Restore_CNN_Soft_VL",
                                              "Upscale_CNN_x2_VL", "AutoDownscalePre_x2",
                                              "AutoDownscalePre_x4", "Restore_CNN_Soft_M",
                                              "Upscale_CNN_x2_M"})},
      {QStringLiteral("hq_ca"), shaderFiles({"Clamp_Highlights", "Upscale_Denoise_CNN_x2_VL",
                                              "AutoDownscalePre_x2", "AutoDownscalePre_x4",
                                              "Restore_CNN_M", "Upscale_CNN_x2_M"})},
  };
}

QVector<QPair<QString, QStringList>> experimentalChains() {
  return {
      {QStringLiteral("exp_3d_graphics"),
       shaderFiles({"Clamp_Highlights", "3DGraphics_AA_Upscale_x2_US"})},
      {QStringLiteral("exp_old_film"), shaderFiles({"Clamp_Highlights", "Deblur_Original",
                                                      "Restore_CNN_M", "Upscale_CNN_x2_M"})},
      {QStringLiteral("exp_deblur_strong"), shaderFiles({"Clamp_Highlights", "Deblur_DoG",
                                                           "Restore_CNN_M", "Upscale_CNN_x2_M"})},
      {QStringLiteral("exp_noise_bilateral_mean"), shaderFiles({"Denoise_Bilateral_Mean"})},
      {QStringLiteral("exp_noise_bilateral_median"), shaderFiles({"Denoise_Bilateral_Median"})},
      {QStringLiteral("exp_noise_bilateral_mode"), shaderFiles({"Denoise_Bilateral_Mode"})},
      {QStringLiteral("exp_thin_veryfast"), shaderFiles({"Thin_VeryFast"})},
      {QStringLiteral("exp_thin_fast"), shaderFiles({"Thin_Fast"})},
      {QStringLiteral("exp_thin_hq"), shaderFiles({"Thin_HQ"})},
      {QStringLiteral("exp_texture_dtd_x2"), shaderFiles({"Upscale_DTD_x2"})},
      {QStringLiteral("exp_texture_dog_x2"), shaderFiles({"Upscale_DoG_x2"})},
      {QStringLiteral("exp_darken_veryfast"), shaderFiles({"Darken_VeryFast"})},
      {QStringLiteral("exp_darken_fast"), shaderFiles({"Darken_Fast"})},
      {QStringLiteral("exp_darken_hq"), shaderFiles({"Darken_HQ"})},
  };
}

void verifyChains(const QVector<QPair<QString, QStringList>>& expected) {
  const QString shadersDir = findShadersDir();
  QVERIFY2(!shadersDir.isEmpty(), "the Anime4K shader directory must be available");
  for (const auto& [id, expectedFiles] : expected) {
    const QStringList actualFiles = filesForId(id, shadersDir);
    QVERIFY2(actualFiles == expectedFiles,
             qPrintable(QStringLiteral("ordered chain mismatch for ") + id +
                        QStringLiteral(": expected ") + expectedFiles.join(QLatin1Char(',')) +
                        QStringLiteral(", got ") + actualFiles.join(QLatin1Char(','))));
    if (expectedFiles.isEmpty()) continue;
    QVERIFY2(chainIsComplete(candidateForId(id, shadersDir), shadersDir), qPrintable(id));
  }
}

QSet<QString> rowPresetIds(const QVector<MenuRow>& rows) {
  QSet<QString> ids;
  for (const MenuRow& row : rows) {
    if (row.role == MenuRow::Role::Preset) ids.insert(row.id);
  }
  return ids;
}

}  // namespace

void TestAnime4K::originalPresetChainsPreserveHistoricalOrder() {
  const auto expected = originalChains();
  verifyChains(expected);

  const QVector<QPair<Mode, QString>> enumIds = {
      {Mode::Off, QStringLiteral("off")},
      {Mode::FastA, QStringLiteral("fast_a")},
      {Mode::FastB, QStringLiteral("fast_b")},
      {Mode::FastC, QStringLiteral("fast_c")},
      {Mode::HqA, QStringLiteral("hq_a")},
      {Mode::HqB, QStringLiteral("hq_b")},
      {Mode::HqC, QStringLiteral("hq_c")},
      {Mode::HqAA, QStringLiteral("hq_aa")},
      {Mode::HqBB, QStringLiteral("hq_bb")},
      {Mode::HqCA, QStringLiteral("hq_ca")},
  };
  for (const auto& [mode, id] : enumIds) QCOMPARE(modeId(mode), id);

  for (const auto& [id, files] : expected) {
    Q_UNUSED(files);
    QCOMPARE(modeId(modeForId(id)), id);
  }
}

void TestAnime4K::experimentalPresetsHaveExactCompleteChains() {
  const auto expected = experimentalChains();
  verifyChains(expected);
  for (const auto& [id, files] : expected) {
    Q_UNUSED(files);
    QCOMPARE(modeId(modeForId(id)), id);
  }
}

void TestAnime4K::presetIdsRemainUniqueAndRestorableAfterMenuReordering() {
  const QVector<ModeInfo> modes = availableModes();
  QSet<QString> ids;
  for (const ModeInfo& info : modes) {
    QVERIFY(!info.id.isEmpty());
    QVERIFY2(!ids.contains(info.id), qPrintable(QStringLiteral("duplicate preset id: ") + info.id));
    ids.insert(info.id);
    QCOMPARE(modeId(info.mode), info.id);
    QCOMPARE(modeForId(info.id), info.mode);
    // A preset with no label or no category is a row the menu cannot name and
    // the Compare badge chooser cannot place. That chooser mirrors both fields,
    // so an empty one there renders a blank leaf under no group - and an
    // equality check against it would pass by comparing "" to "".
    QVERIFY2(!info.label.trimmed().isEmpty(),
             qPrintable(QStringLiteral("preset with no label: ") + info.id));
    QVERIFY2(!info.category.isEmpty(),
             qPrintable(QStringLiteral("preset with no category: ") + info.id));
    QVERIFY2(!categoryLabel(info.category).isEmpty(),
             qPrintable(QStringLiteral("preset under an unnamed category: ") + info.category));
  }

  // A caller may filter a category and sort its visible labels. Restoring by
  // the stable row id must still select the same preset and ordered chain.
  QVector<MenuRow> rows = menuRows(QStringLiteral("anime4k/experimental/thin"));
  QSet<QString> visibleIds = rowPresetIds(rows);
  QVERIFY(visibleIds.contains(QStringLiteral("exp_thin_veryfast")));
  QVERIFY(visibleIds.contains(QStringLiteral("exp_thin_fast")));
  QVERIFY(visibleIds.contains(QStringLiteral("exp_thin_hq")));
  std::sort(rows.begin(), rows.end(), [](const MenuRow& left, const MenuRow& right) {
    return left.label < right.label;
  });
  for (const MenuRow& row : rows) {
    if (row.role != MenuRow::Role::Preset) continue;
    const Mode restored = modeForId(row.id);
    QCOMPARE(modeId(restored), row.id);
  }
}

void TestAnime4K::menuCategoriesExposeTheirPresetMembership() {
  const QVector<QPair<QString, QStringList>> memberships = {
      {QStringLiteral("anime4k/speed"), {QStringLiteral("fast_a"), QStringLiteral("fast_b"),
                                         QStringLiteral("fast_c")}},
      {QStringLiteral("anime4k/quality"), {QStringLiteral("hq_a"), QStringLiteral("hq_b"),
                                           QStringLiteral("hq_c")}},
      {QStringLiteral("anime4k/maximum"), {QStringLiteral("hq_aa"), QStringLiteral("hq_bb"),
                                           QStringLiteral("hq_ca")}},
      {QStringLiteral("anime4k/experimental"), {QStringLiteral("exp_3d_graphics"),
                                                QStringLiteral("exp_old_film"),
                                                QStringLiteral("exp_deblur_strong")}},
      {QStringLiteral("anime4k/experimental/noise"), {QStringLiteral("exp_noise_bilateral_mean"),
                                                       QStringLiteral("exp_noise_bilateral_median"),
                                                       QStringLiteral("exp_noise_bilateral_mode")}},
      {QStringLiteral("anime4k/experimental/thin"), {QStringLiteral("exp_thin_veryfast"),
                                                      QStringLiteral("exp_thin_fast"),
                                                      QStringLiteral("exp_thin_hq")}},
      {QStringLiteral("anime4k/experimental/texture"), {QStringLiteral("exp_texture_dtd_x2"),
                                                          QStringLiteral("exp_texture_dog_x2")}},
      {QStringLiteral("anime4k/experimental/darken"), {QStringLiteral("exp_darken_veryfast"),
                                                        QStringLiteral("exp_darken_fast"),
                                                        QStringLiteral("exp_darken_hq")}},
  };

  for (const auto& [category, expectedIds] : memberships) {
    const QSet<QString> expected(expectedIds.begin(), expectedIds.end());
    const QSet<QString> rows = rowPresetIds(menuRows(category));
    QCOMPARE(rows, expected);
    for (const QString& id : expectedIds) QVERIFY(categoryContainsPreset(category, id));
  }
  QVERIFY(!categoryContainsPreset(QStringLiteral("anime4k/speed"), QStringLiteral("hq_a")));
  QVERIFY(!categoryContainsPreset(QStringLiteral("anime4k/experimental/noise"),
                                  QStringLiteral("exp_thin_hq")));

  // The whole menu, walked from the root the shell opens. What has to hold is
  // the shape a viewer can act on, not any one page's row count: a category a
  // viewer can press leads somewhere, a preset row names a real preset, and a
  // placeholder - the row that says a model is not implemented - resolves to
  // no chain at all, so it cannot select anything even if it were pressed. A
  // page that grew a row, lost a row or was renamed is none of that concern.
  QVector<QString> pending{QString()};
  QSet<QString> reachablePresets;
  while (!pending.isEmpty()) {
    const QString level = pending.takeFirst();
    for (const MenuRow& row : menuRows(level)) {
      switch (row.role) {
        case MenuRow::Role::Preset:
          QVERIFY2(!row.id.isEmpty(), qPrintable(QStringLiteral("a preset row with no id at ") + level));
          // The row's id has to round-trip through the mode table, or pressing
          // it would land on Off and quietly turn the chain off.
          QVERIFY2(modeId(modeForId(row.id)) == row.id, qPrintable(row.id));
          reachablePresets.insert(row.id);
          break;
        case MenuRow::Role::Category:
          QVERIFY2(row.hasChildren, qPrintable(QStringLiteral("a category with nothing under it: ") + row.id));
          pending.append(row.id);
          break;
        case MenuRow::Role::Placeholder:
          QVERIFY2(!row.hasChildren, qPrintable(row.id));
          QVERIFY2(filesForId(row.id).isEmpty(), qPrintable(row.id));
          break;
      }
    }
  }

  // Every preset the catalogue holds is reachable from the tree, exactly once:
  // a preset the menu cannot show is one a viewer cannot pick, and a row
  // pointing at a preset under two categories is a menu that can disagree
  // with itself about what is current.
  QStringList catalogued;
  for (const ModeInfo& info : availableModes()) catalogued.append(info.id);
  QCOMPARE(reachablePresets.size(), catalogued.size());
  for (const QString& id : catalogued) {
    QVERIFY2(reachablePresets.contains(id), qPrintable(QStringLiteral("unreachable preset: ") + id));
  }
}

void TestAnime4K::rifeLevelLivesInNativeMenuNotCatalogue() {
  // The RIFE level is owned by the native menu cutover: no catalogue alias or
  // Planned leaf remains, while every Anime4K preset id, label, chain and the
  // other placeholders are untouched.
  QVERIFY(mplayer::native::menu::ownsLevel(QStringLiteral("rife")));
  QVERIFY(menuRows(QStringLiteral("rife")).isEmpty());
  QVERIFY(categoryLabel(QStringLiteral("rife")).isEmpty());
  QVERIFY(filesForId(QStringLiteral("rife/planned")).isEmpty());
  QVERIFY(candidateForId(QStringLiteral("rife/planned")).id.isEmpty());

  // The untouched neighbours: same ids, same placeholder roles, no chains.
  QVERIFY(!categoryLabel(QStringLiteral("realesrgan")).isEmpty());
  QVERIFY(!categoryLabel(QStringLiteral("other-models")).isEmpty());
  for (const MenuRow& row : menuRows(QStringLiteral("realesrgan"))) {
    QVERIFY(row.role == MenuRow::Role::Placeholder);
    QVERIFY(filesForId(row.id).isEmpty());
  }
  for (const MenuRow& row : menuRows(QStringLiteral("other-models"))) {
    QVERIFY(row.role == MenuRow::Role::Placeholder);
    QVERIFY(filesForId(row.id).isEmpty());
  }
  QCOMPARE(availableModes().size(), 24);
}

void TestAnime4K::offAndUnknownIdsResolveNothing() {
  QVERIFY(filesForId(QStringLiteral("off")).isEmpty());
  QVERIFY(filesForId(QStringLiteral("unknown-preset-id")).isEmpty());
  QVERIFY(candidateForId(QStringLiteral("unknown-preset-id")).id.isEmpty());
  QVERIFY(filesForId(QStringLiteral("Anime4K_NotARealShader.glsl")).isEmpty());
}

void TestAnime4K::missingStageMakesTheEntireChainIncomplete() {
  const QString sourceDir = findShadersDir();
  QVERIFY(!sourceDir.isEmpty());
  const QStringList chain = filesForId(QStringLiteral("hq_aa"), sourceDir);
  QVERIFY(chain.size() > 1);

  QTemporaryDir fixture;
  QVERIFY(fixture.isValid());
  for (int index = 0; index < chain.size() - 1; ++index) {
    QVERIFY2(QFile::copy(QDir(sourceDir).filePath(chain.at(index)),
                         QDir(fixture.path()).filePath(chain.at(index))),
             qPrintable(chain.at(index)));
  }

  mplayer::anime4k::Candidate candidate = candidateForId(QStringLiteral("hq_aa"));
  QVERIFY(!chainIsComplete(candidate, fixture.path()));
  QVERIFY(shaderStringForCandidate(QStringLiteral("hq_aa"), fixture.path()).isEmpty());
  QVERIFY(QFile::copy(QDir(sourceDir).filePath(chain.last()),
                      QDir(fixture.path()).filePath(chain.last())));
  QVERIFY(chainIsComplete(candidate, fixture.path()));

  const QString shaderString = shaderStringForCandidate(QStringLiteral("hq_aa"), fixture.path());
  QVERIFY(!shaderString.isEmpty());
#ifdef Q_OS_WIN
  const QChar separator = QLatin1Char(';');
#else
  const QChar separator = QLatin1Char(':');
#endif
  QStringList expectedPaths;
  for (const QString& file : chain) expectedPaths.append(QDir(fixture.path()).filePath(file));
  QCOMPARE(shaderString.split(separator), expectedPaths);
}

void TestAnime4K::individualShaderIdsResolveAndDownscaleStagesAreNotStandalone() {
  const QString shadersDir = findShadersDir();
  QVERIFY(!shadersDir.isEmpty());
  const QVector<mplayer::anime4k::Candidate> candidates = allCandidates(shadersDir);
  QSet<QString> seen;
  for (const auto& candidate : candidates) {
    if (candidate.kind == Kind::Preset) continue;
    QVERIFY(candidate.id.endsWith(QStringLiteral(".glsl")));
    QVERIFY(!candidate.id.contains(QStringLiteral("AutoDownscalePre")));
    QVERIFY(!seen.contains(candidate.id));
    seen.insert(candidate.id);
    QCOMPARE(filesForId(candidate.id, shadersDir), QStringList{candidate.id});
    QCOMPARE(candidate.files, QStringList{candidate.id});
    QVERIFY(QFileInfo::exists(QDir(shadersDir).filePath(candidate.id)));
  }
}

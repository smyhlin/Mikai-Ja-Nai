// The shader candidate catalogue: what the compare window lists, what each
// candidate resolves to, and what refuses to resolve at all.
//
// Hermetic on purpose. The pixel half of the comparison - that a chain really
// changes the picture - needs a GL context and lives in compare_probe, which is
// hand-run. This suite covers everything that can be wrong without rendering
// anything: a candidate that names a file which is not there, an id that
// silently resolves to something, or a preset whose id is the enum ordinal
// rather than its name.

#pragma once

#include <QtTest/QTest>

class TestAnime4K : public QObject {
  Q_OBJECT

 private slots:
  void everyCandidateCarriesAnIdAndALabel();
  void presetsComeFirstAndAreMarkedAsPresets();
  void individualShadersAreGroupedByKind();
  void offIsACandidateWithNoFiles();
  void anUnknownIdResolvesToNothing();
  void anEmptyIdResolvesToNothing();
  void everyPresetResolvesToFilesThatExist();
  void aSingleShaderResolvesToItself();
  void theShadersDirectoryIsFoundBesideTheExecutable();
  void aMissingDirectoryResolvesNothing();
  void chainIsCompleteRejectsAnAbsentFile();
  void theChainStringJoinsWithThePlatformSeparator();
  void modeIdIsTheStoredKeyNotTheOrdinal();
  void everyModeHasAnId();
};

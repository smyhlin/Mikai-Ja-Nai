// Stream extraction: turning a provider page into playable HLS variants.
//
// Hermetic on purpose. The extraction rules are the ones that silently rot —
// MoonAnime's obfuscated payload, Tortuga's seeded token, the resolution the
// user actually asked for — and a suite that only proves them against the live
// providers fails on the day a provider rate-limits CI.

#pragma once

#include <QtTest/QTest>

class TestStreams : public QObject {
  Q_OBJECT

 private slots:
  void detectsEveryKnownSite_data();
  void detectsEveryKnownSite();
  void rejectsAnUnsupportedPageUrl_data();
  void rejectsAnUnsupportedPageUrl();

  void parsesVariantsInPlaylistOrder();
  void resolvesRelativeVariantUrlsAgainstTheMaster();
  void keepsAnAbsoluteVariantUrlUntouched();
  void ignoresLinesThatAreNotVariants();
  void aStreamInfWithoutAResolutionHasNoHeight();

  void qualityUrlPrefersAnExactHeight();
  void qualityUrlFallsBackToTheNearestLowerHeight();
  void qualityUrlClimbsToTheTallestWhenAskedBelowAllOfThem();
  void qualityUrlPrefersTheFirstOfTwoEqualHeights();
  void qualityUrlRejectsAnEmptyVariantList();

  void decodesATortugaToken();
  void rejectsATortugaTokenThatIsNotBase64();

  void theTimeoutIsTwentySeconds();
  void theFetchRetriesTwice();
};

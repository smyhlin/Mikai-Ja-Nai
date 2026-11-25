#include "TestPlaybackLink.hpp"

#include "app/support/PlaybackLink.hpp"

#include <QtTest/QTest>

using namespace mikai::app;

void TestPlaybackLink::roundTripFullLinkWithSpecialEpisodeAndTime() {
  PlaybackLink orig;
  orig.ref = QStringLiteral("7428");
  orig.provider = QStringLiteral("MoonAnime");
  orig.teamSlug = QStringLiteral("fanvoxua");
  orig.episode = QStringLiteral("12.5");
  orig.timeSec = 754;

  const QString encoded = formatPlaybackLink(orig);
  QVERIFY(isPlaybackLinkUrl(encoded));
  QVERIFY(encoded.startsWith(QStringLiteral("mikai-janai://play?")));

  QString err;
  const auto parsed = parsePlaybackLink(encoded, &err);
  QVERIFY2(parsed.has_value(), qPrintable(err));
  QVERIFY(parsed->hasPlaybackTarget());
  QCOMPARE(parsed->ref, QStringLiteral("7428"));
  QCOMPARE(parsed->provider, QStringLiteral("moonanime"));
  QCOMPARE(parsed->teamSlug, QStringLiteral("fanvoxua"));
  QCOMPARE(parsed->episode, QStringLiteral("12.5"));
  QCOMPARE(parsed->timeSec, std::optional<int>(754));
}

void TestPlaybackLink::partialPlaybackParamsDegradeToTitleOnly() {
  QString err;
  const auto parsed = parsePlaybackLink(QStringLiteral("mikai-janai://play?ref=7428&provider=moon"), &err);
  QVERIFY2(parsed.has_value(), qPrintable(err));
  QCOMPARE(parsed->ref, QStringLiteral("7428"));
  QVERIFY(!parsed->hasPlaybackTarget());
}

void TestPlaybackLink::rejectsInvalidSchemesAndBadTimecodes() {
  QString err;
  QVERIFY(!parsePlaybackLink(QStringLiteral("https://mikai.me/anime/frieren"), &err).has_value());
  QVERIFY(!parsePlaybackLink(QStringLiteral("mikai-janai://play?provider=moon"), &err).has_value());
  QVERIFY(!parsePlaybackLink(QStringLiteral("mikai-janai://play?ref=7428&provider=moon&episode=1&t=-5"), &err).has_value());
  QVERIFY(!parsePlaybackLink(QStringLiteral("mikai-janai://play?ref=7428&provider=moon&episode=1&t=NaN"), &err).has_value());
}

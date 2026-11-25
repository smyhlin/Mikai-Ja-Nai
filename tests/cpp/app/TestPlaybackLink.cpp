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
  QVERIFY(!parsePlaybackLink(QStringLiteral("mikai-janai://play?v=2&ref=7428"), &err).has_value());
}

void TestPlaybackLink::roundTripReservedDelimitersAndCompactKeys() {
  PlaybackLink orig;
  orig.ref = QStringLiteral("frieren-100%");
  orig.provider = QStringLiteral("ashdi");
  orig.teamSlug = QStringLiteral("team&co+1");
  orig.episode = QStringLiteral("OVA#1");
  orig.timeSec = 0;

  const QString encoded = formatPlaybackLink(orig);
  QString err;
  const auto parsed = parsePlaybackLink(encoded, &err);
  QVERIFY2(parsed.has_value(), qPrintable(err));
  QCOMPARE(parsed->ref, QStringLiteral("frieren-100%"));
  QCOMPARE(parsed->provider, QStringLiteral("ashdi"));
  QCOMPARE(parsed->teamSlug, QStringLiteral("team&co+1"));
  QCOMPARE(parsed->episode, QStringLiteral("OVA#1"));
  QCOMPARE(parsed->timeSec, std::optional<int>(0));

  const auto compact = parsePlaybackLink(
      QStringLiteral("mikai-janai://play?v=1&ref=7428&p=ashdi&ep=5&t=312"), &err);
  QVERIFY2(compact.has_value(), qPrintable(err));
  QVERIFY(compact->hasPlaybackTarget());
  QCOMPARE(compact->provider, QStringLiteral("ashdi"));
  QCOMPARE(compact->episode, QStringLiteral("5"));
  QCOMPARE(compact->timeSec, std::optional<int>(312));
}

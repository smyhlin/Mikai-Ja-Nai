#include "TestI18n.hpp"

#include <app/I18n.hpp>
#include <app/Labels.hpp>

#include <QtCore/QLocale>

void TestI18n::explicitPreferenceOverridesSystemLocale() {
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("en"), QLocale::Ukrainian),
           QStringLiteral("en"));
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("uk"), QLocale::English),
           QStringLiteral("uk"));
}

void TestI18n::systemPreferenceUsesUkrainianOnlyForUkrainianLocale() {
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("system"), QLocale::Ukrainian),
           QStringLiteral("uk"));
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("system"), QLocale::English),
           QStringLiteral("en"));
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("system"), QLocale::German),
           QStringLiteral("en"));
}

void TestI18n::invalidPreferenceUsesSystemLocale() {
  QCOMPARE(mikai::app::resolveLanguageCode(QStringLiteral("xx"), QLocale::Ukrainian),
           QStringLiteral("uk"));
}

void TestI18n::knownApiErrorsUseEnglishSourceText() {
  QCOMPARE(mikai::app::localizedApiError(QStringLiteral("invalid_api_key")),
           QStringLiteral("The API key is invalid."));
  QVERIFY(mikai::app::localizedApiError(QStringLiteral("unknown_server_code")).isEmpty());
}

void TestI18n::metadataLabelsUseEnglishSources() {
  using namespace mikai::app::labels;
  QCOMPARE(status(QStringLiteral("ongoing")), QStringLiteral("Airing"));
  QCOMPARE(kind(QStringLiteral("voice")), QStringLiteral("Dubbed"));
  QCOMPARE(format(QStringLiteral("tv")), QStringLiteral("TV series"));
  QCOMPARE(source(QStringLiteral("manga")), QStringLiteral("Manga"));
  QCOMPARE(country(QStringLiteral("JP")), QStringLiteral("Japan"));
  QCOMPARE(ageRating(QStringLiteral("pg12")), QStringLiteral("12+"));
}

void TestI18n::seasonLabelsKeepTheExistingMonthMapping() {
  using mikai::app::labels::season;
  QCOMPARE(season(QStringLiteral("2025-01-20")), QStringLiteral("2025"));
  QCOMPARE(season(QStringLiteral("2025-02-01")), QStringLiteral("Winter 2025"));
  QCOMPARE(season(QStringLiteral("2025-03-01")), QStringLiteral("Spring 2025"));
  QCOMPARE(season(QStringLiteral("2025-06-01")), QStringLiteral("Summer 2025"));
  QCOMPARE(season(QStringLiteral("2025-10-01")), QStringLiteral("Autumn 2025"));
  QVERIFY(season(QStringLiteral("invalid")).isEmpty());
}

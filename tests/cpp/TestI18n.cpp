#include "TestI18n.hpp"

#include <app/I18n.hpp>

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

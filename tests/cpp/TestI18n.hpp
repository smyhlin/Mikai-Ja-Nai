#pragma once

#include <QtTest/QTest>

class TestI18n : public QObject {
  Q_OBJECT

 private slots:
  void explicitPreferenceOverridesSystemLocale();
  void systemPreferenceUsesUkrainianOnlyForUkrainianLocale();
  void invalidPreferenceUsesSystemLocale();
};

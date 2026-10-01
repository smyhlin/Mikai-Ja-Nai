#pragma once

#include <QLocale>
#include <QString>

namespace mikai::app {

inline constexpr char kUiLanguageSettingKey[] = "ui/language";

QString languagePreference();
QString localizedApiError(const QString& code);
QString resolveLanguageCode(const QString& preference, QLocale::Language systemLanguage);
QString resolvedLanguageCode();

}  // namespace mikai::app

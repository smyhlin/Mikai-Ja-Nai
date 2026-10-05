#pragma once

#include <QLocale>
#include <QString>

namespace mikai::app {

inline constexpr char kUiLanguageSettingKey[] = "ui/language";

QString languagePreference();
QString localizedApiError(const QString& code);
QString resolveLanguageCode(const QString& preference, QLocale::Language systemLanguage);
QString resolvedLanguageCode();

// Installs the resolved interface language into the running application. The
// preference is "system", "en" or "uk"; "system" maps to Ukrainian only when
// the system locale is Ukrainian, English otherwise.
//
// `applicationCatalog` is a real catalog path for callers that cannot see the
// application's embedded resource (QA probes, tests). It is configured once and
// retained: an empty value afterwards keeps the configured path, which defaults
// to the embedded resource ":/i18n/mikai_uk.qm".
//
// Returns true when the requested language is active. The preference is never
// written here.
bool applyInterfaceLanguage(const QString& preference, const QString& applicationCatalog = {});

// The language the application actually runs in: the one the last successful
// applyInterfaceLanguage call installed, English while none is installed, or the
// language the current preference resolves to before the first call.
QString activeInterfaceLanguage();

}  // namespace mikai::app

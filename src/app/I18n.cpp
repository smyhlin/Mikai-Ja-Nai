#include "I18n.hpp"

#include <QCoreApplication>
#include <QLocale>
#include <QSettings>

namespace mikai::app {

QString languagePreference() {
    QSettings settings;
    const QString preference = settings.value(QString::fromLatin1(kUiLanguageSettingKey),
                                              QStringLiteral("system"))
                                   .toString();
    if (preference == QStringLiteral("en") || preference == QStringLiteral("uk")) {
        return preference;
    }
    return QStringLiteral("system");
}

QString localizedApiError(const QString& code) {
    if (code == QStringLiteral("invalid_parameter")) {
        return QCoreApplication::translate("ApiErrors", "The request contains an invalid parameter.");
    }
    if (code == QStringLiteral("invalid_reference")) {
        return QCoreApplication::translate("ApiErrors", "The title reference is invalid.");
    }
    if (code == QStringLiteral("too_many_ids")) {
        return QCoreApplication::translate("ApiErrors", "Too many title IDs were requested.");
    }
    if (code == QStringLiteral("invalid_request")) {
        return QCoreApplication::translate("ApiErrors", "The request is invalid.");
    }
    if (code == QStringLiteral("not_found")) {
        return QCoreApplication::translate("ApiErrors", "The requested title was not found.");
    }
    if (code == QStringLiteral("invalid_api_key")) {
        return QCoreApplication::translate("ApiErrors", "The API key is invalid.");
    }
    if (code == QStringLiteral("api_key_disabled")) {
        return QCoreApplication::translate("ApiErrors", "This API key has been disabled.");
    }
    if (code == QStringLiteral("rate_limited")) {
        return QCoreApplication::translate("ApiErrors", "The request limit has been reached.");
    }
    if (code == QStringLiteral("internal_error")) {
        return QCoreApplication::translate("ApiErrors", "The API encountered an internal error.");
    }
    if (code == QStringLiteral("network_error")) {
        return QCoreApplication::translate("ApiErrors", "A network error occurred.");
    }
    if (code == QStringLiteral("http_error")) {
        return QCoreApplication::translate("ApiErrors", "The server returned an invalid response.");
    }
    return {};
}

QString resolveLanguageCode(const QString& preference, QLocale::Language systemLanguage) {
    if (preference == QStringLiteral("en") || preference == QStringLiteral("uk")) {
        return preference;
    }
    return systemLanguage == QLocale::Ukrainian ? QStringLiteral("uk") : QStringLiteral("en");
}

QString resolvedLanguageCode() {
    return resolveLanguageCode(languagePreference(), QLocale::system().language());
}

}  // namespace mikai::app

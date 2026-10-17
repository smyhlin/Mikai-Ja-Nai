#include "I18n.hpp"

#include <memory>

#include <QCoreApplication>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QPointer>
#include <QSettings>
#include <QStringList>
#include <QTranslator>

namespace mikai::app {
namespace {

// What an empty catalog falls back to when no explicit one was ever supplied.
constexpr char kEmbeddedUkrainianCatalog[] = ":/i18n/mikai_uk.qm";

// windeployqt and the mikai build both put Qt's own catalogs in a
// "translations" directory beside the executable, which is the layout a
// packaged copy runs in; QLibraryInfo covers an uninstalled run.
QStringList qtTranslationSearchPaths() {
    QStringList paths;
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    if (!applicationDirectory.isEmpty()) {
        paths << applicationDirectory + QStringLiteral("/translations");
    }
    paths << QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    return paths;
}

// The manager is parented to the application and reached through a QPointer,
// so it can never outlive the QCoreApplication that owns the translator list.
class InterfaceLanguageManager final : public QObject {
public:
    explicit InterfaceLanguageManager(QCoreApplication* application)
        : QObject(application), application_(application) {}

    ~InterfaceLanguageManager() override {
        // During teardown the translator list is already gone with the
        // application; removing then would only trip the "instantiate the
        // QApplication object first" warning.
        if (QCoreApplication::instance() != application_) {
            return;
        }
        if (applicationTranslator_) {
            application_->removeTranslator(applicationTranslator_.get());
        }
        if (platformTranslator_) {
            application_->removeTranslator(platformTranslator_.get());
        }
    }

    QString resolvedLanguage() const { return resolvedLanguage_; }

    bool apply(const QString& preference, const QString& applicationCatalog) {
        const QString resolved =
            resolveLanguageCode(preference, QLocale::system().language());
        if (resolved == resolvedLanguage_) {
            // Already running in the requested language: installing the same
            // catalog again would only churn translators and re-post
            // LanguageChange for texts that already match. A different explicit
            // catalog still reloads, so a caller pointing at a new file gets it.
            // The default locale is re-asserted first, so queued observers
            // compute locale text from the resolved language.
            if (resolved != QLatin1String("uk") || applicationCatalog.isEmpty() ||
                applicationCatalog == applicationCatalogPath_) {
                QLocale::setDefault(resolved == QLatin1String("uk")
                                        ? QLocale(QLocale::Ukrainian, QLocale::Ukraine)
                                        : QLocale(QLocale::English, QLocale::UnitedStates));
                return true;
            }
        }
        if (resolved == QLatin1String("uk")) {
            return applyUkrainian(applicationCatalog);
        }
        return applyEnglish();
    }

private:
    bool applyEnglish() {
        if (applicationTranslator_) {
            application_->removeTranslator(applicationTranslator_.get());
            applicationTranslator_.reset();
        }
        if (platformTranslator_) {
            application_->removeTranslator(platformTranslator_.get());
            platformTranslator_.reset();
        }
        resolvedLanguage_ = QStringLiteral("en");
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        return true;
    }

    bool applyUkrainian(const QString& applicationCatalog) {
        const QString path = applicationCatalog.isEmpty()
                                 ? (applicationCatalogPath_.isEmpty()
                                        ? QString::fromLatin1(kEmbeddedUkrainianCatalog)
                                        : applicationCatalogPath_)
                                 : applicationCatalog;

        // Every replacement is loaded before anything installed is removed, so a
        // failure leaves the working language and its translators intact.
        auto applicationCandidate = std::make_unique<QTranslator>();
        if (!applicationCandidate->load(path)) {
            qWarning().noquote() << "mikai: interface language catalog not found:" << path;
            return false;
        }
        if (applicationCandidate->isEmpty()) {
            qWarning().noquote() << "mikai: interface language catalog is empty:" << path;
            return false;
        }

        // The Qt platform catalog is optional: the application catalog decides
        // success, so a missing qtbase_uk warns and the switch continues.
        const QLocale locale(QLocale::Ukrainian, QLocale::Ukraine);
        std::unique_ptr<QTranslator> platformCandidate;
        const QStringList platformSearchPaths = qtTranslationSearchPaths();
        for (const QString& searchPath : platformSearchPaths) {
            auto candidate = std::make_unique<QTranslator>();
            if (candidate->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                                searchPath) &&
                !candidate->isEmpty()) {
                platformCandidate = std::move(candidate);
                break;
            }
        }
        if (!platformCandidate) {
            qWarning().noquote()
                << "mikai: Ukrainian Qt platform translations were not found in"
                << platformSearchPaths;
        }

        // Install the replacements before dropping the working ones, so even a
        // rejected install cannot leave the application translator-less.
        if (!application_->installTranslator(applicationCandidate.get())) {
            qWarning().noquote() << "mikai: the application rejected the Ukrainian translator";
            return false;
        }
        if (platformCandidate) {
            if (!application_->installTranslator(platformCandidate.get())) {
                qWarning().noquote() << "mikai: the application rejected the Qt platform translator";
                application_->removeTranslator(applicationCandidate.get());
                return false;
            }
        }
        if (applicationTranslator_) {
            application_->removeTranslator(applicationTranslator_.get());
        }
        if (platformTranslator_) {
            application_->removeTranslator(platformTranslator_.get());
        }
        applicationTranslator_ = std::move(applicationCandidate);
        platformTranslator_ = std::move(platformCandidate);
        applicationCatalogPath_ = path;
        resolvedLanguage_ = QStringLiteral("uk");
        QLocale::setDefault(locale);
        return true;
    }

    QCoreApplication* application_ = nullptr;
    QString resolvedLanguage_ = QStringLiteral("en");
    QString applicationCatalogPath_;
    std::unique_ptr<QTranslator> applicationTranslator_;
    std::unique_ptr<QTranslator> platformTranslator_;
};

InterfaceLanguageManager* interfaceLanguageManager(QCoreApplication* application, bool create) {
    static QPointer<InterfaceLanguageManager> manager;
    if (manager && manager->parent() == application) {
        return manager.data();
    }
    if (!create || application == nullptr) {
        return nullptr;
    }
    manager = new InterfaceLanguageManager(application);
    return manager.data();
}

}  // namespace

bool applyInterfaceLanguage(const QString& preference, const QString& applicationCatalog) {
    QCoreApplication* application = QCoreApplication::instance();
    if (application == nullptr) {
        qWarning("mikai: cannot apply an interface language without a running application");
        return false;
    }
    return interfaceLanguageManager(application, true)->apply(preference, applicationCatalog);
}

QString activeInterfaceLanguage() {
    if (auto* manager = interfaceLanguageManager(QCoreApplication::instance(), false)) {
        return manager->resolvedLanguage();
    }
    return resolveLanguageCode(languagePreference(), QLocale::system().language());
}

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

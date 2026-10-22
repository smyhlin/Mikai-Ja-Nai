#include "state/AppearanceStore.hpp"

#include <QApplication>
#include <QEvent>
#include <QWidget>

#include <QSettings>

namespace mikai::app {
namespace {

constexpr char kThemeKey[] = "customization/theme";
constexpr char kTitleLanguageKey[] = "customization/titleLanguage";
constexpr char kNameLanguageKey[] = "customization/nameLanguage";
constexpr char kCoverGradientKey[] = "customization/coverGradient";
constexpr char kAdultContentKey[] = "customization/adultContent";
constexpr char kAccentKey[] = "customization/accent";
constexpr char kRadiusKey[] = "customization/radiusStep";
constexpr char kBackgroundGradientKey[] = "customization/backgroundGradient";
constexpr char kGradientIntensityKey[] = "customization/gradientIntensity";
constexpr char kGradientHeightKey[] = "customization/gradientHeight";
constexpr char kGradientColorKey[] = "customization/gradientColor";

theme::Appearance& stored() {
    static theme::Appearance appearance = loadAppearance();
    return appearance;
}

}  // namespace

theme::Appearance loadAppearance() {
    QSettings settings;
    theme::Appearance appearance;
    appearance.siteTheme =
        theme::themeFromId(settings.value(QString::fromLatin1(kThemeKey)).toString());
    appearance.titleLanguage = theme::titleLanguageFromId(
        settings.value(QString::fromLatin1(kTitleLanguageKey)).toString());
    appearance.nameLanguage = theme::nameLanguageFromId(
        settings.value(QString::fromLatin1(kNameLanguageKey)).toString());
    appearance.coverGradient =
        settings.value(QString::fromLatin1(kCoverGradientKey), true).toBool();
    appearance.showAdult =
        settings.value(QString::fromLatin1(kAdultContentKey), false).toBool();
    const QString accent = settings.value(QString::fromLatin1(kAccentKey)).toString();
    if (QColor(accent).isValid()) {
        appearance.accent = accent;
    }
    appearance.radiusStep = qBound(
        0, settings.value(QString::fromLatin1(kRadiusKey), appearance.radiusStep).toInt(),
        theme::radiusStepCount() - 1);
    appearance.backgroundGradient =
        settings.value(QString::fromLatin1(kBackgroundGradientKey), true).toBool();
    appearance.gradientIntensity =
        qBound(0, settings.value(QString::fromLatin1(kGradientIntensityKey), 30).toInt(), 100);
    appearance.gradientHeight =
        qBound(10, settings.value(QString::fromLatin1(kGradientHeightKey), 100).toInt(), 100);
    const QString gradient =
        settings.value(QString::fromLatin1(kGradientColorKey)).toString();
    if (QColor(gradient).isValid()) {
        appearance.gradientColor = gradient;
    }
    return appearance;
}

void saveAppearance(const theme::Appearance& appearance) {
    QSettings settings;
    settings.setValue(QString::fromLatin1(kThemeKey), theme::themeId(appearance.siteTheme));
    settings.setValue(QString::fromLatin1(kTitleLanguageKey),
                      theme::languageId(appearance.titleLanguage));
    settings.setValue(QString::fromLatin1(kNameLanguageKey),
                      theme::nameLanguageId(appearance.nameLanguage));
    settings.setValue(QString::fromLatin1(kCoverGradientKey), appearance.coverGradient);
    settings.setValue(QString::fromLatin1(kAdultContentKey), appearance.showAdult);
    settings.setValue(QString::fromLatin1(kAccentKey), theme::hexOf(theme::accentColor(appearance)));
    settings.setValue(QString::fromLatin1(kRadiusKey),
                      qBound(0, appearance.radiusStep, theme::radiusStepCount() - 1));
    settings.setValue(QString::fromLatin1(kBackgroundGradientKey),
                      appearance.backgroundGradient);
    settings.setValue(QString::fromLatin1(kGradientIntensityKey),
                      qBound(0, appearance.gradientIntensity, 100));
    settings.setValue(QString::fromLatin1(kGradientHeightKey),
                      qBound(10, appearance.gradientHeight, 100));
    settings.setValue(QString::fromLatin1(kGradientColorKey),
                      theme::hexOf(theme::gradientColor(appearance)));
    settings.sync();
    setCurrentAppearance(appearance);
}

const theme::Appearance& currentAppearance() { return stored(); }

void setCurrentAppearance(const theme::Appearance& appearance) {
    if (stored() == appearance) return;
    const theme::Appearance previous = stored();
    stored() = appearance;
    // Content names are ordinary translatable strings; the pages already know
    // how to restate them on LanguageChange, which is cheaper and more complete
    // than teaching every page a second refresh path.
    if (previous.titleLanguage != appearance.titleLanguage ||
        previous.nameLanguage != appearance.nameLanguage) {
        QEvent event(QEvent::LanguageChange);
        const QList<QWidget*> windows = QApplication::topLevelWidgets();
        for (QWidget* window : windows) {
            if (window != nullptr && window->isVisible()) {
                QCoreApplication::sendEvent(window, &event);
            }
        }
    }
    Q_EMIT appearanceNotifier().changed(previous);
}

AppearanceNotifier& appearanceNotifier() {
    static AppearanceNotifier notifier;
    return notifier;
}

}  // namespace mikai::app

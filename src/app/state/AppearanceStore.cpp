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
constexpr char kGradientModeKey[] = "customization/gradientMode";
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
    // Explicit content is allowed unless the viewer turns it off: the warning
    // dialog is the exception, not the default reading mode.
    appearance.showAdult =
        settings.value(QString::fromLatin1(kAdultContentKey), true).toBool();
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
    // The gradient used to be an independent colour, so every profile that ever
    // saved customization carries one. A profile with no mode key but with a
    // stored colour chose that colour deliberately and keeps it; one with
    // neither key never made the choice, so it joins the accent instead of
    // inheriting the old blue default.
    if (settings.contains(QString::fromLatin1(kGradientModeKey))) {
        appearance.gradientMode = theme::gradientModeFromId(
            settings.value(QString::fromLatin1(kGradientModeKey)).toString());
    } else {
        appearance.gradientMode = settings.contains(QString::fromLatin1(kGradientColorKey))
                                      ? theme::GradientMode::Custom
                                      : theme::GradientMode::Auto;
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
    settings.setValue(QString::fromLatin1(kGradientModeKey),
                      theme::gradientModeId(appearance.gradientMode));
    // Only a Custom gradient is written: while Auto is selected the stored
    // colour is the viewer's last custom choice, and saving the accent over it
    // would silently destroy a preference they can return to.
    if (appearance.gradientMode == theme::GradientMode::Custom) {
        settings.setValue(QString::fromLatin1(kGradientColorKey),
                          theme::hexOf(theme::gradientColor(appearance)));
    }
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
            // X-M12: no isVisible() filter — a hidden PlayerDialog (PiP
            // owns the pooled float) missed the event and was never
            // re-notified. Qt's own installTranslator path posts to all
            // top-levels regardless of visibility.
            if (window != nullptr) {
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

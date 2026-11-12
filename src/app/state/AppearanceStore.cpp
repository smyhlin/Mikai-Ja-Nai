#include "state/AppearanceStore.hpp"
#include "state/SettingsKeys.hpp"

#include <QApplication>
#include <QEvent>
#include <QWidget>

#include <QSettings>

namespace mikai::app {
namespace {

theme::Appearance& stored() {
    static theme::Appearance appearance = loadAppearance();
    return appearance;
}

}  // namespace

theme::Appearance loadAppearance() {
    QSettings settings;
    theme::Appearance appearance;
    appearance.siteTheme =
        theme::themeFromId(settings.value(QString::fromUtf8(kCustomizationTheme)).toString());
    appearance.titleLanguage = theme::titleLanguageFromId(
        settings.value(QString::fromUtf8(kCustomizationTitleLanguage)).toString());
    appearance.nameLanguage = theme::nameLanguageFromId(
        settings.value(QString::fromUtf8(kCustomizationNameLanguage)).toString());
    appearance.coverGradient =
        settings.value(QString::fromUtf8(kCustomizationCoverGradient), true).toBool();
    // Explicit content is allowed unless the viewer turns it off: the warning
    // dialog is the exception, not the default reading mode.
    appearance.showAdult =
        settings.value(QString::fromUtf8(kCustomizationAdultContent), true).toBool();
    const QString accent = settings.value(QString::fromUtf8(kCustomizationAccent)).toString();
    if (QColor(accent).isValid()) {
        appearance.accent = accent;
    }
    appearance.radiusStep = qBound(
        0, settings.value(QString::fromUtf8(kCustomizationRadiusStep), appearance.radiusStep).toInt(),
        theme::radiusStepCount() - 1);
    appearance.backgroundGradient =
        settings.value(QString::fromUtf8(kCustomizationBackgroundGradient), true).toBool();
    appearance.gradientIntensity =
        qBound(0, settings.value(QString::fromUtf8(kCustomizationGradientIntensity), 30).toInt(), 100);
    appearance.gradientHeight =
        qBound(10, settings.value(QString::fromUtf8(kCustomizationGradientHeight), 100).toInt(), 100);
    const QString gradient =
        settings.value(QString::fromUtf8(kCustomizationGradientColor)).toString();
    if (QColor(gradient).isValid()) {
        appearance.gradientColor = gradient;
    }
    // The gradient used to be an independent colour, so every profile that ever
    // saved customization carries one. A profile with no mode key but with a
    // stored colour chose that colour deliberately and keeps it; one with
    // neither key never made the choice, so it joins the accent instead of
    // inheriting the old blue default.
    if (settings.contains(QString::fromUtf8(kCustomizationGradientMode))) {
        appearance.gradientMode = theme::gradientModeFromId(
            settings.value(QString::fromUtf8(kCustomizationGradientMode)).toString());
    } else {
        appearance.gradientMode = settings.contains(QString::fromUtf8(kCustomizationGradientColor))
                                      ? theme::GradientMode::Custom
                                      : theme::GradientMode::Auto;
    }
    return appearance;
}

void saveAppearance(const theme::Appearance& appearance) {
    QSettings settings;
    settings.setValue(QString::fromUtf8(kCustomizationTheme), theme::themeId(appearance.siteTheme));
    settings.setValue(QString::fromUtf8(kCustomizationTitleLanguage),
                      theme::languageId(appearance.titleLanguage));
    settings.setValue(QString::fromUtf8(kCustomizationNameLanguage),
                      theme::nameLanguageId(appearance.nameLanguage));
    settings.setValue(QString::fromUtf8(kCustomizationCoverGradient), appearance.coverGradient);
    settings.setValue(QString::fromUtf8(kCustomizationAdultContent), appearance.showAdult);
    settings.setValue(QString::fromUtf8(kCustomizationAccent), theme::hexOf(theme::accentColor(appearance)));
    settings.setValue(QString::fromUtf8(kCustomizationRadiusStep),
                      qBound(0, appearance.radiusStep, theme::radiusStepCount() - 1));
    settings.setValue(QString::fromUtf8(kCustomizationBackgroundGradient),
                      appearance.backgroundGradient);
    settings.setValue(QString::fromUtf8(kCustomizationGradientIntensity),
                      qBound(0, appearance.gradientIntensity, 100));
    settings.setValue(QString::fromUtf8(kCustomizationGradientHeight),
                      qBound(10, appearance.gradientHeight, 100));
    settings.setValue(QString::fromUtf8(kCustomizationGradientMode),
                      theme::gradientModeId(appearance.gradientMode));
    // Only a Custom gradient is written: while Auto is selected the stored
    // colour is the viewer's last custom choice, and saving the accent over it
    // would silently destroy a preference they can return to.
    if (appearance.gradientMode == theme::GradientMode::Custom) {
        settings.setValue(QString::fromUtf8(kCustomizationGradientColor),
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
            // Notify hidden windows too; they still need to refresh translated content.
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

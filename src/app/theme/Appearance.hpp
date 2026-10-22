#pragma once

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>

namespace mikai::app::theme {

/// Site-wide colour scheme. Every stylesheet token is derived from it, so a
/// switch re-lays the whole application rather than one page.
enum class SiteTheme { Dark, Light };

/// Which form of a title the application shows. `Original` is the language the
/// service returns as the original name, never a transliteration.
enum class TitleLanguage { Ukrainian, English, Original };

/// Which language the application's own content vocabulary uses: episode
/// captions, release kinds, statuses and age ratings. Independent of the
/// interface language, so an English UI can still name episodes in Ukrainian.
enum class NameLanguage { Interface, Ukrainian, English, Original };

struct AccentPreset {
    QString id;
    QString color;
};

struct Appearance {
    SiteTheme siteTheme = SiteTheme::Dark;
    TitleLanguage titleLanguage = TitleLanguage::Ukrainian;
    NameLanguage nameLanguage = NameLanguage::Interface;
    bool coverGradient = true;
    bool showAdult = false;
    QString accent = QStringLiteral("#bdacff");
    int radiusStep = 1;
    bool backgroundGradient = true;
    int gradientIntensity = 30;
    int gradientHeight = 100;
    QString gradientColor = QStringLiteral("#67e4e4");

    bool operator==(const Appearance&) const = default;
};

/// The accent swatches the appearance page offers, in display order.
QVector<AccentPreset> accentPresets();

/// The corner-radius steps, smallest first. Index 0 means square corners.
QVector<int> radiusSteps();
QStringList radiusNames();

int radiusSmall(const Appearance& appearance);
int radiusMedium(const Appearance& appearance);
int radiusLarge(const Appearance& appearance);
int radiusPill(const Appearance& appearance);
int radiusStepCount();

QString themeId(SiteTheme theme);
SiteTheme themeFromId(const QString& id);
QString languageId(TitleLanguage language);
TitleLanguage titleLanguageFromId(const QString& id);
QString nameLanguageId(NameLanguage language);
NameLanguage nameLanguageFromId(const QString& id);

/// The readable name of a swatch's colour ("Blue"), empty for a custom one,
/// resolved through the application translator.
QString presetName(const QString& color);

bool isLight(const Appearance& appearance);
/// The accent actually used: the stored one when it parses, the default
/// otherwise, so a corrupted setting cannot leave the application unreadable.
QColor accentColor(const Appearance& appearance);
QColor gradientColor(const Appearance& appearance);

/// Scales every `border-radius: Npx` in `sheet` by `scale`, keeping a sheet
/// written with literal corners in step with the chosen radius step.
QString scaleRadii(const QString& sheet, double scale);

QString hexOf(const QColor& color);
/// "167, 139, 250" — the form `rgba(%1, 0.12)` needs.
QString rgbTriplet(const QColor& color);
/// A hover form of the accent that stays legible on the current bed.
QColor hoverOf(const QColor& color, bool light);
/// The accent mixed into the bed, opaque, for washes a QSS cannot compose.
QColor blend(const QColor& from, const QColor& to, double amount);

}  // namespace mikai::app::theme

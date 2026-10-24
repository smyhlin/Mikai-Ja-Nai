#include "theme/Appearance.hpp"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QStringList>

namespace mikai::app::theme {
namespace {

constexpr char kDefaultAccent[] = "#bdacff";
constexpr char kDefaultGradient[] = "#67e4e4";

}  // namespace

QVector<AccentPreset> accentPresets() {
    return {
        {QStringLiteral("pink"), QStringLiteral("#f27bb0")},
        {QStringLiteral("blue"), QStringLiteral("#6797e4")},
        {QStringLiteral("green"), QStringLiteral("#5fd08a")},
        {QStringLiteral("orange"), QStringLiteral("#f0a35e")},
        {QStringLiteral("purple"), QString::fromLatin1(kDefaultAccent)},
        {QStringLiteral("red"), QStringLiteral("#ef6b78")},
        {QStringLiteral("cyan"), QStringLiteral("#67e4e4")},
    };
}

QVector<int> radiusSteps() { return {0, 4, 6, 8, 12, 16}; }

int radiusStepCount() { return radiusSteps().size(); }

QStringList radiusNames() {
    return {QCoreApplication::translate("mikai::app::theme", "None"),
            QStringLiteral("XS"), QStringLiteral("SM"), QStringLiteral("MD"),
            QStringLiteral("LG"), QStringLiteral("XL")};
}

int radiusSmall(const Appearance& appearance) {
    const QVector<int> steps = radiusSteps();
    const int index = qBound(0, appearance.radiusStep, steps.size() - 1);
    return steps.at(index);
}

int radiusMedium(const Appearance& appearance) { return radiusSmall(appearance) + 4; }

int radiusLarge(const Appearance& appearance) { return radiusSmall(appearance) + 8; }

int radiusPill(const Appearance& appearance) {
    // A pill keeps a minimum curve even at the square step: a 0 px radius turns
    // a chip into a rectangle and loses the affordance it carries.
    return qBound(2, radiusSmall(appearance) * 3 / 4, 8);
}

QString themeId(SiteTheme theme) {
    return theme == SiteTheme::Light ? QStringLiteral("light") : QStringLiteral("dark");
}

SiteTheme themeFromId(const QString& id) {
    return id == QStringLiteral("light") ? SiteTheme::Light : SiteTheme::Dark;
}

QString languageId(TitleLanguage language) {
    switch (language) {
        case TitleLanguage::English:
            return QStringLiteral("en");
        case TitleLanguage::Original:
            return QStringLiteral("original");
        case TitleLanguage::Ukrainian:
            break;
    }
    return QStringLiteral("uk");
}

TitleLanguage titleLanguageFromId(const QString& id) {
    if (id == QStringLiteral("en")) return TitleLanguage::English;
    if (id == QStringLiteral("original")) return TitleLanguage::Original;
    return TitleLanguage::Ukrainian;
}

QString nameLanguageId(NameLanguage language) {
    switch (language) {
        case NameLanguage::Ukrainian:
            return QStringLiteral("uk");
        case NameLanguage::English:
            return QStringLiteral("en");
        case NameLanguage::Original:
            return QStringLiteral("original");
        case NameLanguage::Interface:
            break;
    }
    return QStringLiteral("interface");
}

NameLanguage nameLanguageFromId(const QString& id) {
    if (id == QStringLiteral("uk")) return NameLanguage::Ukrainian;
    if (id == QStringLiteral("en")) return NameLanguage::English;
    if (id == QStringLiteral("original")) return NameLanguage::Original;
    return NameLanguage::Interface;
}

QString presetName(const QString& color) {
    const QColor value(color);
    if (!value.isValid()) return QString();
    if (value == QColor(QStringLiteral("#f27bb0")))
        return QCoreApplication::translate("mikai::app::theme", "Pink");
    if (value == QColor(QStringLiteral("#6797e4")))
        return QCoreApplication::translate("mikai::app::theme", "Blue");
    if (value == QColor(QStringLiteral("#5fd08a")))
        return QCoreApplication::translate("mikai::app::theme", "Green");
    if (value == QColor(QStringLiteral("#f0a35e")))
        return QCoreApplication::translate("mikai::app::theme", "Orange");
    if (value == QColor(QString::fromLatin1(kDefaultAccent)))
        return QCoreApplication::translate("mikai::app::theme", "Purple");
    if (value == QColor(QStringLiteral("#ef6b78")))
        return QCoreApplication::translate("mikai::app::theme", "Red");
    if (value == QColor(QString::fromLatin1(kDefaultGradient)))
        return QCoreApplication::translate("mikai::app::theme", "Turquoise");
    return QString();
}

bool isLight(const Appearance& appearance) { return appearance.siteTheme == SiteTheme::Light; }

QColor accentColor(const Appearance& appearance) {
    const QColor stored(appearance.accent);
    return stored.isValid() ? stored : QColor(QString::fromLatin1(kDefaultAccent));
}

QColor gradientColor(const Appearance& appearance) {
    const QColor stored(appearance.gradientColor);
    return stored.isValid() ? stored : QColor(QString::fromLatin1(kDefaultGradient));
}

QString scaleRadii(const QString& sheet, double scale) {
    if (qFuzzyCompare(scale, 1.0)) return sheet;
    static const QRegularExpression corner(QStringLiteral("border-radius:\\s*(\\d+)px"));
    QString scaled;
    scaled.reserve(sheet.size());
    qsizetype offset = 0;
    QRegularExpressionMatchIterator matches = corner.globalMatch(sheet);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        scaled += sheet.mid(offset, match.capturedStart() - offset);
        const int radius = qMax(0, qRound(match.captured(1).toInt() * scale));
        scaled += QStringLiteral("border-radius: %1px").arg(radius);
        offset = match.capturedEnd();
    }
    scaled += sheet.mid(offset);
    return scaled;
}

QString hexOf(const QColor& color) { return color.name(QColor::HexRgb); }

QString rgbTriplet(const QColor& color) {
    return QStringLiteral("%1, %2, %3")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue());
}

QColor hoverOf(const QColor& color, bool light) {
    // Toward white on a dark bed, toward black on a light one: the hover has to
    // stay distinguishable from the base accent in the direction the bed leaves
    // room for.
    return light ? color.darker(118) : color.lighter(118);
}

QColor blend(const QColor& from, const QColor& to, double amount) {
    const double clamped = qBound(0.0, amount, 1.0);
    return QColor(qRound(from.red() + (to.red() - from.red()) * clamped),
                  qRound(from.green() + (to.green() - from.green()) * clamped),
                  qRound(from.blue() + (to.blue() - from.blue()) * clamped));
}

}  // namespace mikai::app::theme

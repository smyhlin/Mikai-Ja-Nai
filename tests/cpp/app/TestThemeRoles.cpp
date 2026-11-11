#include "TestThemeRoles.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <QtCore/QEventLoop>
#include <QtCore/QMap>
#include <QtCore/QScopeGuard>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QtMath>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtGui/QRegion>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QProxyStyle>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include "dialogs/settings/SettingsUi.hpp"
#include "state/AppearanceStore.hpp"
#include "theme/AntialiasedStyle.hpp"
#include "theme/Appearance.hpp"
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"
#include "theme/Theme.hpp"
#include "widgets/FlowLayout.hpp"
#include "widgets/PageGlow.hpp"
#include "widgets/SwatchButton.hpp"
#include <player/shell/ShellTheme.hpp>

namespace {

using mikai::app::theme::ActionRole;
using mikai::app::theme::Appearance;
using mikai::app::theme::Palette;
using mikai::app::theme::SiteTheme;

constexpr int kColorTolerance = 3;

// ── Independent math ────────────────────────────────────────────────────────
// The gates below are recomputed here rather than delegated to the production
// helper, so a change to theme::contrastRatio cannot silently move both the
// rule and its proof. Qt composites QRgb values in sRGB space and rounds to the
// nearest channel value; this is that operation, written out.
double linearChannel(int value) {
    const double channel = value / 255.0;
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor& color) {
    return 0.2126 * linearChannel(color.red()) + 0.7152 * linearChannel(color.green()) +
           0.0722 * linearChannel(color.blue());
}

double independentContrast(const QColor& a, const QColor& b) {
    const double first = relativeLuminance(a);
    const double second = relativeLuminance(b);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

QColor over(const QColor& foreground, const QColor& bed, double alpha) {
    const double amount = alpha;
    const auto channel = [&](int top, int bottom) {
        return qBound(0, qRound(top * amount + bottom * (1.0 - amount)), 255);
    };
    return QColor(channel(foreground.red(), bed.red()),
                  channel(foreground.green(), bed.green()),
                  channel(foreground.blue(), bed.blue()));
}

QColor washColor(const Palette& palette) {
    const QStringList parts = palette.wash.split(QStringLiteral(","));
    if (parts.size() != 3) return QColor(Qt::white);
    return QColor(parts.at(0).trimmed().toInt(), parts.at(1).trimmed().toInt(),
                  parts.at(2).trimmed().toInt());
}

bool nearChannel(int actual, int expected, int tolerance = kColorTolerance) {
    return std::abs(actual - expected) <= tolerance;
}

bool nearColor(const QColor& actual, const QColor& expected) {
    if (expected.alpha() == 0) return actual.alpha() == 0;
    return nearChannel(actual.red(), expected.red()) &&
           nearChannel(actual.green(), expected.green()) &&
           nearChannel(actual.blue(), expected.blue()) &&
           nearChannel(actual.alpha(), expected.alpha());
}

/// The hue a translucent wash resolves to, ignoring the alpha the wash adds on
/// purpose: a gradient's top row is the accent at a bounded opacity, so the
/// colour is the contract and the alpha is measured on its own.
bool sameRgb(const QColor& actual, const QColor& expected, int tolerance = kColorTolerance) {
    return nearChannel(actual.red(), expected.red(), tolerance) &&
           nearChannel(actual.green(), expected.green(), tolerance) &&
           nearChannel(actual.blue(), expected.blue(), tolerance);
}

QString texture(const QColor& color) {
    return QStringLiteral("#%1%2%3 a%4")
        .arg(color.red(), 2, 16, QLatin1Char('0'))
        .arg(color.green(), 2, 16, QLatin1Char('0'))
        .arg(color.blue(), 2, 16, QLatin1Char('0'))
        .arg(color.alpha());
}

void installAppearance(const Appearance& appearance) {
    mikai::app::setCurrentAppearance(appearance);
    mikai::app::theme::applyPalette(appearance);
    qApp->setStyleSheet(mikai::app::theme::buildStylesheet(appearance));
    mplayer::shell::setAccent(mikai::app::theme::accentColor(appearance));
    mplayer::shell::setCornerRadius(mikai::app::theme::radiusSmall(appearance));
}

enum class RoleState { Normal, Hover, Pressed, Checked, Focus, Disabled };

const char* stateName(RoleState state) {
    switch (state) {
        case RoleState::Normal: return "normal";
        case RoleState::Hover: return "hover";
        case RoleState::Pressed: return "pressed";
        case RoleState::Checked: return "checked";
        case RoleState::Focus: return "focus";
        case RoleState::Disabled: return "disabled";
    }
    return "normal";
}

void setVisualState(QPushButton& button, RoleState state) {
    button.setAttribute(Qt::WA_UnderMouse, false);
    button.setDown(false);
    button.setChecked(false);
    button.setEnabled(true);
    button.clearFocus();
    button.setCheckable(state == RoleState::Checked);
    switch (state) {
        case RoleState::Normal:
            break;
        case RoleState::Hover:
            button.setAttribute(Qt::WA_UnderMouse, true);
            break;
        case RoleState::Pressed:
            button.setDown(true);
            break;
        case RoleState::Checked:
            button.setChecked(true);
            break;
        case RoleState::Focus:
            button.setFocus(Qt::TabFocusReason);
            break;
        case RoleState::Disabled:
            button.setEnabled(false);
            break;
    }
    mikai::app::theme::repolish(&button);
    QApplication::processEvents();
}

struct WidgetPixels {
    QImage image;
    qreal scale = 1.0;

    QColor at(int x, int y) const {
        return image.pixelColor(qRound(x * scale), qRound(y * scale));
    }
};

/// The style option a button's own paint event hands to the sheet, with the
/// state under test stated explicitly. Windows clears WA_UnderMouse whenever
/// the physical pointer is elsewhere, so a rendered hover cannot be produced by
/// flipping the attribute: the state is passed to the widget's own style, which
/// is the same resolution path the paint event uses.
QStyleOptionButton roleOption(const QPushButton& button, RoleState state) {
    QStyleOptionButton option;
    option.initFrom(&button);
    option.rect = button.rect();
    option.text = button.text();
    option.icon = button.icon();
    option.iconSize = button.iconSize();
    option.features = QStyleOptionButton::None;
    option.state &= ~(QStyle::State_HasFocus | QStyle::State_MouseOver | QStyle::State_Sunken |
                      QStyle::State_On | QStyle::State_Raised);
    switch (state) {
        case RoleState::Normal:
            option.state |= QStyle::State_Raised;
            break;
        case RoleState::Hover:
            option.state |= QStyle::State_Raised | QStyle::State_MouseOver;
            break;
        case RoleState::Pressed:
            option.state |= QStyle::State_Sunken;
            break;
        case RoleState::Checked:
            option.state |= QStyle::State_On;
            break;
        case RoleState::Focus:
            option.state |= QStyle::State_Raised | QStyle::State_HasFocus;
            break;
        case RoleState::Disabled:
            option.state &= ~QStyle::State_Enabled;
            break;
    }
    return option;
}

WidgetPixels grabRoleState(QPushButton& button, RoleState state) {
    // Both places a sheet can read a hover from are set here, and nothing is
    // processed afterwards: the style option's state and the widget's own
    // under-mouse attribute (an event turn can clear the latter).
    button.setAttribute(Qt::WA_UnderMouse, state == RoleState::Hover);
    WidgetPixels pixels;
    pixels.scale = button.devicePixelRatioF() > 0.0 ? button.devicePixelRatioF() : 1.0;
    pixels.image = QImage(qCeil(button.width() * pixels.scale),
                          qCeil(button.height() * pixels.scale),
                          QImage::Format_ARGB32_Premultiplied);
    pixels.image.setDevicePixelRatio(pixels.scale);
    pixels.image.fill(Qt::transparent);
    QPainter painter(&pixels.image);
    QStyleOptionButton option = roleOption(button, state);
    button.style()->drawControl(QStyle::CE_PushButton, &option, &painter, &button);
    painter.end();
    return pixels;
}

WidgetPixels grabWidget(QWidget& widget) {
    WidgetPixels pixels;
    // Render onto an explicitly transparent surface: a widget's own grab leaves
    // areas it does not paint unspecified, and the ghost/danger roles are
    // defined by painting *nothing*, so the alpha has to be measurable.
    pixels.scale = widget.devicePixelRatioF() > 0.0 ? widget.devicePixelRatioF() : 1.0;
    pixels.image = QImage(qCeil(widget.width() * pixels.scale),
                          qCeil(widget.height() * pixels.scale),
                          QImage::Format_ARGB32_Premultiplied);
    pixels.image.setDevicePixelRatio(pixels.scale);
    pixels.image.fill(Qt::transparent);
    QPainter painter(&pixels.image);
    widget.render(&painter);
    painter.end();
    return pixels;
}

/// What one rendered role state shows at two places a viewer reads: the inside
/// of the control and its top edge. Both are raw widget pixels, so any alpha
/// they carry is the widget's own translucency, not a value copied from the
/// sheet.
struct RoleSample {
    QColor fill;
    QColor border;
    WidgetPixels pixels;
    int width = 0;
    int height = 0;
};

RoleSample sampleRole(QPushButton& button, RoleState state) {
    RoleSample sample;
    if (button.width() < 8 || button.height() < 8) return sample;
    sample.pixels = grabRoleState(button, state);
    if (sample.pixels.image.isNull()) return sample;
    sample.width = button.width();
    sample.height = button.height();
    sample.fill = sample.pixels.at(qMin(4, sample.width / 4), sample.height / 2);
    sample.border = sample.pixels.at(sample.width / 2, 0);
    return sample;
}

/// The opaque colour a viewer perceives for a translucent pixel on the real bed
/// the control sits on - which is what contrast and hue mean in practice.
QColor perceived(const QColor& pixel, const QColor& bed) {
    if (!pixel.isValid()) return QColor();
    return over(pixel, bed, pixel.alpha() / 255.0);
}

/// The strongest labelled-text contrast inside the control: the core of a glyph
/// stroke against the surface it is painted on. The border ring is excluded, so
/// a borderless role is measured by its text and a filled one by whichever of
/// the two carries more.
double textContrast(const RoleSample& sample, const QColor& bed) {
    if (sample.width <= 4 || sample.height <= 4 || sample.pixels.image.isNull()) return 0.0;
    const QColor surface = perceived(sample.fill, bed);
    double best = 0.0;
    for (int y = 2; y < sample.height - 2; ++y) {
        for (int x = 2; x < sample.width - 2; ++x) {
            // A borderless role paints its label with an alpha of its own, so
            // every pixel is composited onto the surface it is drawn over
            // rather than filtered by its opacity.
            const QColor pixel = sample.pixels.at(x, y);
            const QColor seen = over(pixel, surface, pixel.alpha() / 255.0);
            best = std::max(best, independentContrast(seen, surface));
        }
    }
    return best;
}

/// How much of a state's appearance a viewer can actually see, compared with the
/// resting role: the whole rendered control - fill, edge and label alike - as it
/// would appear on the real bed. A state may legitimately change only its ink
/// (a disabled neutral keeps its fill and boundary and just dims its label), so
/// the label has to be part of the measurement or that feedback is invisible to
/// it. Two identical renders give zero; any real change gives a large per-pixel
/// step at the pixels that changed.
int perceivedDistance(const RoleSample& first, const RoleSample& second, const QColor& bed) {
    if (first.pixels.image.isNull() || second.pixels.image.isNull()) return 0;
    if (first.width != second.width || first.height != second.height) return 255 * 3;
    int best = 0;
    for (int y = 0; y < first.height; ++y) {
        for (int x = 0; x < first.width; ++x) {
            const QColor left = first.pixels.at(x, y);
            const QColor right = second.pixels.at(x, y);
            const QColor a = over(left, bed, left.alpha() / 255.0);
            const QColor b = over(right, bed, right.alpha() / 255.0);
            const int delta = std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) +
                              std::abs(a.blue() - b.blue());
            best = std::max(best, delta);
        }
    }
    return best;
}

int hueDistance(const QColor& first, const QColor& second) {
    if (!first.isValid() || !second.isValid() || first.hue() < 0 || second.hue() < 0) return 0;
    return std::abs(((first.hue() - second.hue()) % 360 + 540) % 360 - 180);
}

QString failure(const char* what, const QColor& actual, const QColor& expected) {
    return QStringLiteral("%1: painted %2, expected %3")
        .arg(QString::fromLatin1(what), texture(actual), texture(expected));
}

}  // namespace

void TestThemeRoles::initTestCase() {
    settingsDirectory_ = std::make_unique<QTemporaryDir>();
    QVERIFY2(settingsDirectory_->isValid(), "cannot create the suite's isolated settings directory");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_->path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory_->path());
}

void TestThemeRoles::init() {
    savedAppearance_ = mikai::app::currentAppearance();
    savedPalette_ = qApp->palette();
    savedStylesheet_ = qApp->styleSheet();
    savedAccent_ = mplayer::shell::accent();
    QSettings settings;
    settings.clear();
    settings.sync();
}

void TestThemeRoles::cleanup() {
    QSettings settings;
    settings.clear();
    settings.sync();
    if (QWidget* focused = QApplication::focusWidget()) focused->clearFocus();
    mikai::app::setCurrentAppearance(savedAppearance_);
    mikai::app::theme::applyPalette(savedAppearance_);
    qApp->setStyleSheet(savedStylesheet_);
    qApp->setPalette(savedPalette_);
    mplayer::shell::setAccent(savedAccent_);
    QApplication::processEvents();
}

void TestThemeRoles::checkboxIndicatorUsesPaletteAndRadiusAtFractionalDpi() {
    mikai::app::theme::AntialiasedStyle style(QStringLiteral("Fusion"));
    QProxyStyle legacy(QStringLiteral("Fusion"));
    for (const SiteTheme site : {SiteTheme::Dark, SiteTheme::Light}) {
        Appearance appearance;
        appearance.siteTheme = site;
        for (const int radiusStep : {0, 1}) {
            appearance.radiusStep = radiusStep;
            installAppearance(appearance);

            for (const qreal scale : {1.25, 1.75}) {
                QStyleOptionButton option;
                option.rect = QRect(2, 2, 13, 13);
                option.palette = qApp->palette();
                option.state = QStyle::State_Enabled | QStyle::State_On;
                QImage image(40, 40, QImage::Format_ARGB32_Premultiplied);
                image.setDevicePixelRatio(scale);
                image.fill(Qt::transparent);
                {
                    QPainter painter(&image);
                    style.drawPrimitive(QStyle::PE_IndicatorCheckBox, &option, &painter);
                }

                int accentPixels = 0;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        if (image.pixelColor(x, y) == option.palette.color(QPalette::Highlight)) {
                            ++accentPixels;
                        }
                    }
                }
                QVERIFY(accentPixels > 0);

                option.state = QStyle::State_Enabled;
                image.fill(Qt::transparent);
                {
                    QPainter painter(&image);
                    style.drawPrimitive(QStyle::PE_IndicatorCheckBox, &option, &painter);
                }
                const qreal physicalScale = scale;
                const int top = qRound(option.rect.top() * physicalScale);
                const int center = qRound((option.rect.left() + option.rect.width() / 2.0) *
                                          physicalScale);
                QCOMPARE(image.pixelColor(center, top), option.palette.color(QPalette::Mid));
                QCOMPARE(image.pixelColor(center, top - 1).alpha(), 0);

                QImage legacyImage(40, 40, QImage::Format_ARGB32_Premultiplied);
                legacyImage.setDevicePixelRatio(scale);
                legacyImage.fill(Qt::transparent);
                {
                    QPainter painter(&legacyImage);
                    legacy.drawPrimitive(QStyle::PE_IndicatorCheckBox, &option, &painter);
                }
                if (radiusStep == 0) {
                    QCOMPARE(image.pixelColor(top, top).alpha(), 255);
                    continue;
                }

                const int cornerX = top + 1;
                const int cornerY = top + 1;
                QVERIFY(image.pixelColor(cornerX, cornerY).alpha() <
                        legacyImage.pixelColor(cornerX, cornerY).alpha());

                bool foundFractionalCornerCoverage = false;
                for (int offset = 1; offset < qCeil(5 * scale); ++offset) {
                    const int alpha = image.pixelColor(top + offset, top + offset).alpha();
                    if (alpha > 0 && alpha < 255) {
                        foundFractionalCornerCoverage = true;
                        break;
                    }
                }
                QVERIFY2(foundFractionalCornerCoverage,
                         "rounded indicator edge must contain real fractional corner coverage");

                option.state = QStyle::State_Enabled | QStyle::State_NoChange;
                image.fill(Qt::transparent);
                {
                    QPainter painter(&image);
                    style.drawPrimitive(QStyle::PE_IndicatorCheckBox, &option, &painter);
                }
                const QColor ink = option.palette.color(QPalette::HighlightedText);
                QVERIFY(image.pixelColor(center, qRound((option.rect.top() + 6.0) * scale)) == ink);

                option.state = QStyle::State_Enabled | QStyle::State_HasFocus;
                image.fill(Qt::transparent);
                {
                    QPainter painter(&image);
                    style.drawPrimitive(QStyle::PE_IndicatorCheckBox, &option, &painter);
                }
                QCOMPARE(image.pixelColor(center, top), option.palette.color(QPalette::Highlight));

                option.state = QStyle::State_None;
                image.fill(Qt::transparent);
                {
                    QPainter painter(&image);
                    style.drawPrimitive(QStyle::PE_IndicatorCheckBox, &option, &painter);
                }
                QCOMPARE(image.pixelColor(center, top),
                         option.palette.color(QPalette::Disabled, QPalette::Mid));
            }
        }
    }
}

void TestThemeRoles::actionRoleStatesOnePropertyAndNoSize() {
    // No ambient sheet here: the assertion is about applyActionRole's own
    // writes. A sheet's min-height is the sheet's business and is measured by
    // the rendered geometry test instead.
    qApp->setStyleSheet(QString());
    const QVector<QPair<ActionRole, QString>> roles = {
        {ActionRole::Primary, QStringLiteral("primary")},
        {ActionRole::Secondary, QStringLiteral("secondary")},
        {ActionRole::Ghost, QStringLiteral("ghost")},
        {ActionRole::Danger, QStringLiteral("danger")}};

    QPushButton button(QStringLiteral("Role"));
    // A role must never resize a control whose caller already fixed it; the
    // sheets own role heights so two metrics cannot fight.
    button.setMinimumHeight(11);
    button.setMaximumHeight(11);
    for (const auto& role : roles) {
        mikai::app::theme::applyActionRole(&button, role.first);
        QCOMPARE(button.property("actionRole").toString(), role.second);
        QCOMPARE(button.minimumHeight(), 11);
        QCOMPARE(button.maximumHeight(), 11);
    }
}

void TestThemeRoles::searchPrimaryFillIsTheRawAccent() {
    // The property every ambient-accent fix depends on: the emitted primary is
    // exactly the chosen accent, with no shipped-accent special case left.
    QVector<QString> accents;
    for (const mikai::app::theme::AccentPreset& preset : mikai::app::theme::accentPresets()) {
        accents.append(preset.color);
    }
    accents.append(QStringLiteral("#123456"));  // dark custom
    accents.append(QStringLiteral("#f8f8f8"));  // near-white custom

    for (const QString& accent : accents) {
        Appearance appearance;
        appearance.accent = accent;
        const mikai::app::theme::SearchHeaderPalette roles =
            mikai::app::theme::searchHeaderPalette(appearance);
        QCOMPARE(roles.primary, mikai::app::theme::hexOf(mikai::app::theme::accentColor(appearance)));
        QCOMPARE(roles.primaryHover,
                 mikai::app::theme::paletteFor(appearance).accentHover);
    }

    // Render the real header rule: the scoped search-page sheet is part of the
    // application stylesheet, and the primary role's fill must be the accent
    // pixel-for-pixel (only the ink may flip for readability).
    QWidget page;
    page.setObjectName(QStringLiteral("searchPage"));
    page.setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QHBoxLayout(&page);
    layout->setContentsMargins(12, 12, 12, 12);
    auto* action = new QPushButton(QStringLiteral("Search"));
    action->setObjectName(QStringLiteral("searchActionButton"));
    mikai::app::theme::applyActionRole(action, ActionRole::Primary);
    layout->addWidget(action);
    page.resize(360, 90);
    page.show();

    for (SiteTheme site : {SiteTheme::Dark, SiteTheme::Light}) {
        for (const QString& accent : accents) {
            Appearance appearance;
            appearance.siteTheme = site;
            appearance.accent = accent;
            installAppearance(appearance);
            QApplication::processEvents();
            const QColor expected = mikai::app::theme::accentColor(appearance);
            QCOMPARE(mikai::app::theme::paletteFor(appearance).accent,
                     mikai::app::theme::hexOf(expected));
            const WidgetPixels pixels = grabWidget(*action);
            QVERIFY(!pixels.image.isNull());
            const QColor painted = pixels.at(4, action->height() / 2);
            QVERIFY2(nearColor(painted, expected),
                     qPrintable(failure("search primary fill", painted, expected)));
        }
    }
}

void TestThemeRoles::secondaryBoundaryPassesThreeToOneInBothThemes() {
    for (SiteTheme site : {SiteTheme::Dark, SiteTheme::Light}) {
        Appearance appearance;
        appearance.siteTheme = site;
        const Palette palette = mikai::app::theme::paletteFor(appearance);
        const QColor border(palette.borderControl);
        const QStringList beds = {palette.background, palette.surfacePage, palette.surfaceRaised};
        for (const QString& bed : beds) {
            const double ratio = independentContrast(border, QColor(bed));
            QVERIFY2(ratio >= 3.0,
                     qPrintable(QStringLiteral("%1 theme: borderControl on %2 is %3:1")
                                    .arg(mikai::app::theme::themeId(site), bed)
                                    .arg(ratio, 0, 'f', 2)));
            // The production helper must report the same number the gate just
            // computed, so a future "fix" cannot hide in the helper.
            QVERIFY(std::abs(ratio - mikai::app::theme::contrastRatio(border, QColor(bed))) <
                    0.005);
        }
        // Disabled body text keeps its distance from the disabled fill instead
        // of dissolving into it.
        const QColor disabledInk =
            over(washColor(palette), QColor(palette.surface), 0.55);
        const double disabledRatio =
            independentContrast(disabledInk, QColor(palette.surface));
        QVERIFY2(disabledRatio >= 4.5,
                 qPrintable(QStringLiteral("%1 theme: disabled ink is %2:1")
                                .arg(mikai::app::theme::themeId(site))
                                .arg(disabledRatio, 0, 'f', 2)));
    }
}

void TestThemeRoles::roleStatesRenderTheirDocumentedContract() {
    const QVector<ActionRole> roles = {ActionRole::Primary, ActionRole::Secondary,
                                       ActionRole::Ghost, ActionRole::Danger};
    const QVector<RoleState> states = {RoleState::Normal, RoleState::Hover,
                                       RoleState::Pressed, RoleState::Checked,
                                       RoleState::Focus, RoleState::Disabled};
    const auto slot = [](RoleState state) { return static_cast<int>(state); };
    constexpr int kHueTolerance = 20;
    constexpr int kFeedbackFloor = 8;

    for (SiteTheme site : {SiteTheme::Dark, SiteTheme::Light}) {
        Appearance appearance;
        appearance.siteTheme = site;
        installAppearance(appearance);
        const Palette palette = mikai::app::theme::paletteFor(appearance);
        const QColor bed(palette.background);
        const QColor accent(palette.accent);
        const QColor danger(palette.danger);
        const QString themeName = mikai::app::theme::themeId(site);

        QWidget host;
        host.setAttribute(Qt::WA_StyledBackground, true);
        auto* layout = new QVBoxLayout(&host);
        layout->setContentsMargins(8, 8, 8, 8);
        QPushButton button(QStringLiteral("Action"));
        layout->addWidget(&button, 0, Qt::AlignLeft);
        host.resize(280, 90);
        host.show();
        QApplication::processEvents();

        for (ActionRole role : roles) {
            mikai::app::theme::applyActionRole(&button, role);
            const QString roleName = mikai::app::theme::actionRoleName(role);
            QMap<int, RoleSample> samples;
            for (RoleState state : states) {
                setVisualState(button, state);
                QVERIFY2(button.isVisible(), "the role host must be visible to render");
                samples.insert(slot(state), sampleRole(button, state));
            }
            setVisualState(button, RoleState::Normal);
            const RoleSample resting = samples.value(slot(RoleState::Normal));
            QVERIFY2(resting.fill.isValid() && resting.border.isValid(),
                     "the resting role must render");

            const auto detail = [&](RoleState state, const QString& what) {
                return QStringLiteral("%1 %2 %3: %4")
                    .arg(themeName, QString::fromLatin1(stateName(state)), roleName, what);
            };

            // ── The fill each role's definition names ───────────────────────
            if (role == ActionRole::Primary) {
                QVERIFY2(sameRgb(resting.fill, accent),
                         qPrintable(detail(RoleState::Normal,
                                           QStringLiteral("the primary fill must be the chosen "
                                                          "accent, painted %1")
                                               .arg(texture(resting.fill)))));
            } else if (role == ActionRole::Secondary) {
                const QColor raised(palette.surfaceControlRaised);
                QVERIFY2(sameRgb(resting.fill, raised),
                         qPrintable(detail(RoleState::Normal,
                                           QStringLiteral("the secondary fill must be the raised "
                                                          "control, painted %1")
                                               .arg(texture(resting.fill)))));
            } else {
                QVERIFY2(resting.fill.alpha() == 0,
                         qPrintable(detail(RoleState::Normal,
                                           QStringLiteral("the %1 role paints no fill, painted %2")
                                               .arg(roleName, texture(resting.fill)))));
            }

            // ── The colour identity the role paints ─────────────────────────
            // A state that carries the accent must carry the CHOSEN accent: the
            // hue a viewer sees, not the alpha one stylesheet happens to write.
            const auto hueError = [&](RoleState state, const QColor& reference,
                                      const QString& what) {
                const RoleSample sample = samples.value(slot(state));
                const int distance = hueDistance(sample.border, reference);
                if (distance <= kHueTolerance) return QString();
                return detail(state,
                              QStringLiteral("%1 must carry the chosen hue, its hue is %2 degrees "
                                             "away (%3)")
                                  .arg(what)
                                  .arg(distance)
                                  .arg(texture(sample.border)));
            };
            if (role == ActionRole::Primary) {
                for (RoleState state : {RoleState::Normal, RoleState::Hover, RoleState::Pressed}) {
                    const QString error = hueError(state, accent, QStringLiteral("the primary edge"));
                    QVERIFY2(error.isEmpty(), qPrintable(error));
                }
            } else if (role == ActionRole::Secondary) {
                for (RoleState state : {RoleState::Hover, RoleState::Checked, RoleState::Focus}) {
                    const QString error = hueError(state, accent, QStringLiteral("the hovered/checked edge"));
                    QVERIFY2(error.isEmpty(), qPrintable(error));
                }
            } else if (role == ActionRole::Ghost) {
                for (RoleState state : {RoleState::Hover, RoleState::Pressed, RoleState::Checked,
                                        RoleState::Focus}) {
                    const QString error = hueError(state, accent, QStringLiteral("the ghost edge"));
                    QVERIFY2(error.isEmpty(), qPrintable(error));
                }
            } else {
                for (RoleState state : {RoleState::Normal, RoleState::Hover, RoleState::Pressed,
                                        RoleState::Checked, RoleState::Focus}) {
                    const QString error = hueError(state, danger, QStringLiteral("the danger edge"));
                    QVERIFY2(error.isEmpty(), qPrintable(error));
                }
            }

            // ── The neutral boundary carries the control ────────────────────
            // Measured where the viewer sees it: the boundary composited onto the
            // opaque page bed, which is what a 3:1 component contrast means.
            const auto boundaryError = [&](RoleState state) {
                const RoleSample sample = samples.value(slot(state));
                const QColor seen = perceived(sample.border, bed);
                const double ratio = independentContrast(seen, bed);
                if (ratio >= 3.0) return QString();
                return detail(state,
                              QStringLiteral("the neutral boundary is %1:1 on the page bed")
                                  .arg(ratio, 0, 'f', 2));
            };
            for (RoleState state : states) {
                if (!samples.contains(slot(state))) continue;
                const bool neutralBoundary =
                    (state == RoleState::Disabled &&
                     (role == ActionRole::Primary || role == ActionRole::Secondary ||
                      role == ActionRole::Danger)) ||
                    (role == ActionRole::Secondary &&
                     (state == RoleState::Normal || state == RoleState::Pressed));
                if (!neutralBoundary) continue;
                const QString error = boundaryError(state);
                QVERIFY2(error.isEmpty(), qPrintable(error));
            }

            // ── A disabled control stays readable whichever part carries it ─
            // Filled roles are carried by their boundary; a borderless role by
            // its text. One gate covers both, so no role needs an exemption.
            {
                const RoleSample disabled = samples.value(slot(RoleState::Disabled));
                const double edge = independentContrast(perceived(disabled.border, bed), bed);
                const double text = textContrast(disabled, bed);
                QVERIFY2(std::max(edge, text) >= 3.0,
                         qPrintable(detail(RoleState::Disabled,
                                           QStringLiteral("a disabled control must stay readable: "
                                                          "boundary %1:1, text %2:1")
                                               .arg(edge, 0, 'f', 2)
                                               .arg(text, 0, 'f', 2))));
            }

            // ── Focus is always drawn, and always solid ─────────────────────
            {
                const RoleSample focused = samples.value(slot(RoleState::Focus));
                QVERIFY2(focused.border.alpha() >= 250,
                         qPrintable(detail(RoleState::Focus,
                                           QStringLiteral("the focus edge must be painted solid, it "
                                                          "is alpha %1")
                                               .arg(focused.border.alpha()))));
            }

            // ── Every state reads differently from rest ─────────────────────
            // Measured on the rendered control as a viewer sees it, label
            // included: a disabled neutral legitimately keeps its fill and its
            // boundary and dims only its ink.
            for (RoleState state : states) {
                if (state == RoleState::Normal) continue;
                const int delta = perceivedDistance(resting, samples.value(slot(state)), bed);
                QVERIFY2(delta >= kFeedbackFloor,
                         qPrintable(detail(state,
                                           QStringLiteral("the rendered control must read "
                                                          "differently from rest, the strongest "
                                                          "changed pixel is %1")
                                               .arg(delta))));
            }
        }
        // Leave the button in a neutral state before the host is destroyed.
        setVisualState(button, RoleState::Normal);
        host.hide();
    }
}

void TestThemeRoles::primaryInkStaysReadableOnEveryAccent() {
    QVector<QString> accents;
    for (const mikai::app::theme::AccentPreset& preset : mikai::app::theme::accentPresets()) {
        accents.append(preset.color);
    }
    accents.append(QStringLiteral("#123456"));
    accents.append(QStringLiteral("#f8f8f8"));

    for (const QString& accent : accents) {
        Appearance appearance;
        appearance.accent = accent;
        const QColor chosen = mikai::app::theme::accentColor(appearance);
        const Palette palette = mikai::app::theme::paletteFor(appearance);
        // The fill is never adjusted to make the label fit.
        QCOMPARE(QColor(palette.accent), chosen);
        const QColor ink(palette.accentInk);
        const QColor expectedInkColor =
            independentContrast(chosen, QColor(Qt::black)) >=
                    independentContrast(chosen, QColor(Qt::white))
                ? QColor(Qt::black)
                : QColor(Qt::white);
        QCOMPARE(ink, expectedInkColor);
        const double ratio = independentContrast(ink, chosen);
        QVERIFY2(ratio >= 4.5,
                 qPrintable(QStringLiteral("accent %1 ink %2 is %3:1")
                                .arg(accent, texture(ink))
                                .arg(ratio, 0, 'f', 2)));

        // The player's own token must agree with the application's derived ink.
        const QColor playerInk = mplayer::shell::inkOn(chosen);
        QCOMPARE(playerInk, expectedInkColor);
        QVERIFY(mplayer::shell::rgbaTriplet(chosen) ==
                QStringLiteral("%1, %2, %3")
                    .arg(chosen.red())
                    .arg(chosen.green())
                    .arg(chosen.blue()));
    }
}

void TestThemeRoles::compareSideBStaysAmberOrRotatesToTheComplement() {
    using mplayer::shell::compareSide;
    using mplayer::shell::compareSideB;
    const QColor amber(245, 158, 11);  // #f59e0b
    const QColor saved = mplayer::shell::accent();
    const auto restore = qScopeGuard([saved] { mplayer::shell::setAccent(saved); });

    // Cool and violet accents keep the semantic amber for side B.
    QCOMPARE(compareSideB(QColor(QStringLiteral("#6797e4"))), amber);
    QCOMPARE(compareSideB(QColor(QStringLiteral("#bdacff"))), amber);
    QCOMPARE(compareSideB(QColor(QStringLiteral("#123456"))), amber);

    // Amber's hue is 37 degrees. When the accent is within +/-30 degrees, side B
    // rotates to the documented 210-degree complement so the two seats cannot
    // read as one colour. The window is inclusive: exactly 30 degrees away is
    // already too close to tell apart, so the boundary rotates and only 31
    // degrees out keeps the semantic amber.
    QCOMPARE(compareSideB(QColor::fromHsv(6, 200, 220)), amber);
    QCOMPARE(compareSideB(QColor::fromHsv(68, 200, 220)), amber);
    const QColor atBoundary = compareSideB(QColor::fromHsv(7, 200, 220));
    QVERIFY(atBoundary != amber);
    QCOMPARE(atBoundary.hue(), 210);
    QCOMPARE(compareSideB(QColor::fromHsv(67, 200, 220)).hue(), 210);
    QVERIFY(atBoundary.saturationF() >= 0.45f && atBoundary.saturationF() <= 0.90f);
    QVERIFY(atBoundary.valueF() >= 0.55f && atBoundary.valueF() <= 0.95f);

    mplayer::shell::setAccent(QColor(QStringLiteral("#6797e4")));
    QCOMPARE(compareSide(0), mplayer::shell::accent());
    QCOMPARE(compareSide(1), amber);
    mplayer::shell::setAccent(QColor::fromHsv(37, 200, 220));
    QCOMPARE(compareSide(0), mplayer::shell::accent());
    QCOMPARE(compareSide(1).hue(), 210);
}

void TestThemeRoles::gradientModeMigratesAndPreservesTheCustomChoice() {
    QSettings settings;
    settings.clear();
    settings.sync();

    // A profile that never chose anything joins the accent.
    {
        const Appearance fresh = mikai::app::loadAppearance();
        QCOMPARE(fresh.gradientMode, mikai::app::theme::GradientMode::Auto);
        QCOMPARE(mikai::app::theme::gradientColor(fresh),
                 mikai::app::theme::accentColor(fresh));
    }

    // Old profiles stored an independent colour and no mode key: that colour
    // was a deliberate choice, so it migrates to Custom instead of being
    // silently replaced by the accent.
    settings.clear();
    settings.setValue(QStringLiteral("customization/gradientColor"), QStringLiteral("#abcdef"));
    settings.sync();
    {
        const Appearance migrated = mikai::app::loadAppearance();
        QCOMPARE(migrated.gradientMode, mikai::app::theme::GradientMode::Custom);
        QCOMPARE(mikai::app::theme::gradientColor(migrated), QColor(QStringLiteral("#abcdef")));
    }

    // An explicit mode wins, and Auto never paints the stored custom colour.
    settings.setValue(QStringLiteral("customization/gradientMode"), QStringLiteral("auto"));
    settings.sync();
    {
        const Appearance automatic = mikai::app::loadAppearance();
        QCOMPARE(automatic.gradientMode, mikai::app::theme::GradientMode::Auto);
        QCOMPARE(mikai::app::theme::gradientColor(automatic),
                 mikai::app::theme::accentColor(automatic));
    }

    // Saving a Custom choice persists both keys.
    Appearance custom = mikai::app::loadAppearance();
    custom.gradientMode = mikai::app::theme::GradientMode::Custom;
    custom.gradientColor = QStringLiteral("#abcdef");
    mikai::app::saveAppearance(custom);
    settings.sync();
    QCOMPARE(settings.value(QStringLiteral("customization/gradientMode")).toString(),
             QStringLiteral("custom"));
    QCOMPARE(settings.value(QStringLiteral("customization/gradientColor")).toString(),
             QStringLiteral("#abcdef"));

    // Returning to Auto saves the mode, changes the accent, and leaves the
    // stored custom colour untouched: the viewer can switch back later.
    custom.gradientMode = mikai::app::theme::GradientMode::Auto;
    custom.accent = QStringLiteral("#123456");
    mikai::app::saveAppearance(custom);
    settings.sync();
    QCOMPARE(settings.value(QStringLiteral("customization/gradientMode")).toString(),
             QStringLiteral("auto"));
    QCOMPARE(settings.value(QStringLiteral("customization/gradientColor")).toString(),
             QStringLiteral("#abcdef"));
    {
        const Appearance reloaded = mikai::app::loadAppearance();
        QCOMPARE(reloaded.gradientMode, mikai::app::theme::GradientMode::Auto);
        QCOMPARE(mikai::app::theme::gradientColor(reloaded), QColor(QStringLiteral("#123456")));
        Appearance back = reloaded;
        back.gradientMode = mikai::app::theme::GradientMode::Custom;
        QCOMPARE(mikai::app::theme::gradientColor(back), QColor(QStringLiteral("#abcdef")));
    }

    // A custom colour also survives a save/toggle cycle through the draft the
    // settings page builds: Custom -> Auto -> Custom with no other edit.
    Appearance cycling = mikai::app::loadAppearance();
    for (int round = 0; round < 2; ++round) {
        cycling.gradientMode = round == 0 ? mikai::app::theme::GradientMode::Auto
                                          : mikai::app::theme::GradientMode::Custom;
        mikai::app::saveAppearance(cycling);
    }
    settings.sync();
    QCOMPARE(settings.value(QStringLiteral("customization/gradientColor")).toString(),
             QStringLiteral("#abcdef"));
    QCOMPARE(mikai::app::theme::gradientColor(mikai::app::loadAppearance()),
             QColor(QStringLiteral("#abcdef")));
}

void TestThemeRoles::pageGlowFollowsTheGradientSourceWithoutAColourEdit() {
    // The defect: Auto paints the accent while the stored custom colour sits
    // untouched, so a source-only change leaves every stored field equal. A
    // consumer that compared the stored fields never repainted and the wash kept
    // the previous source's colour.
    Appearance appearance;
    appearance.accent = QStringLiteral("#bdacff");
    appearance.gradientMode = mikai::app::theme::GradientMode::Auto;
    appearance.gradientColor = QStringLiteral("#f27bb0");
    appearance.backgroundGradient = true;
    appearance.gradientIntensity = 60;
    appearance.gradientHeight = 100;
    installAppearance(appearance);

    QWidget host;
    host.resize(420, 240);
    auto* glow = new mikai::app::PageGlow(&host);
    host.show();
    QApplication::processEvents(QEventLoop::AllEvents, 50);

    struct PaintCounter final : QObject {
        int paints = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Paint) ++paints;
            return false;
        }
    } counter;
    glow->installEventFilter(&counter);
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    counter.paints = 0;

    const auto topPixel = [glow] {
        const WidgetPixels pixels = grabWidget(*glow);
        return pixels.at(glow->width() / 2, 2);
    };

    // Auto resolves to the accent even though a pink custom colour is stored.
    // The wash carries the intensity's bounded alpha, so the colour is compared
    // on its own and the alpha only has to exist.
    {
        const QColor wash = topPixel();
        QVERIFY2(wash.alpha() > 0, "the wash must paint at a non-zero opacity");
        QVERIFY2(sameRgb(wash, mikai::app::theme::accentColor(appearance)),
                 qPrintable(failure("auto wash", wash,
                                    mikai::app::theme::accentColor(appearance))));
    }

    Appearance custom = appearance;
    custom.gradientMode = mikai::app::theme::GradientMode::Custom;
    mikai::app::setCurrentAppearance(custom);
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    QVERIFY2(counter.paints > 0,
             "switching the gradient source must ask the wash to repaint");
    {
        const QColor wash = topPixel();
        QVERIFY(wash.alpha() > 0);
        QVERIFY2(sameRgb(wash, QColor(QStringLiteral("#f27bb0"))),
                 qPrintable(failure("custom wash", wash,
                                    QColor(QStringLiteral("#f27bb0")))));
    }

    counter.paints = 0;
    mikai::app::setCurrentAppearance(appearance);
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    QVERIFY2(counter.paints > 0, "returning to Auto must ask the wash to repaint");
    {
        const QColor wash = topPixel();
        QVERIFY(wash.alpha() > 0);
        QVERIFY2(sameRgb(wash, mikai::app::theme::accentColor(appearance)),
                 qPrintable(failure("auto wash again", wash,
                                    mikai::app::theme::accentColor(appearance))));
    }

    glow->removeEventFilter(&counter);
    host.hide();
}

void TestThemeRoles::swatchKeepsItsFixedSizeInsideAFlowLayout() {
    Appearance appearance;
    appearance.siteTheme = SiteTheme::Dark;
    installAppearance(appearance);

    // The real composition the appearance card uses: an application sheet, the
    // settings sheet on the page, a FlowLayout and a self-painted fixed chip.
    QWidget page;
    page.setObjectName(QStringLiteral("appearancePage"));
    page.setAttribute(Qt::WA_StyledBackground, true);
    page.setStyleSheet(mikai::app::settingsStylesheet(appearance));
    auto* flow = new mikai::app::FlowLayout(mikai::app::theme::kSpacingSmall,
                                           mikai::app::theme::kSpacingSmall);
    page.setLayout(flow);

    auto* swatch = new mikai::app::SwatchButton(QColor(QStringLiteral("#f27bb0")), &page);
    swatch->setObjectName(QStringLiteral("accentSwatch"));
    swatch->setFixedSize(mikai::app::theme::kSettingsSwatchSize,
                         mikai::app::theme::kSettingsSwatchSize);
    flow->addWidget(swatch);

    page.resize(580, 240);
    page.show();
    // Two event turns: the first shows and polishes, the second lays the flow
    // out against the final width. The original defect only appeared after the
    // generic button box model was polished onto the chip.
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    swatch->ensurePolished();

    const QSize expected(mikai::app::theme::kSettingsSwatchSize,
                         mikai::app::theme::kSettingsSwatchSize);
    // The author's cap and the final box are the contract; a stylesheet may
    // raise or clear the *minimum* during polish, which is exactly how the old
    // generic button rule made the body two pixels taller than its cap.
    QCOMPARE(swatch->maximumSize(), expected);
    QCOMPARE(swatch->size(), expected);
    // A stylesheet that raises a minimum above the chip's cap is the defect this
    // pair exists to prevent: the interval must be closed, not merely narrower
    // than the tolerance of a comparison.
    QVERIFY2(swatch->minimumSize().width() <= swatch->maximumSize().width() &&
                 swatch->minimumSize().height() <= swatch->maximumSize().height(),
             qPrintable(QStringLiteral("inverted size interval: min %1x%2, max %3x%4")
                            .arg(swatch->minimumSize().width())
                            .arg(swatch->minimumSize().height())
                            .arg(swatch->maximumSize().width())
                            .arg(swatch->maximumSize().height())));
    QVERIFY(swatch->isVisible());
    QVERIFY2(swatch->visibleRegion().contains(swatch->rect()),
             "the chip must be fully visible, not clipped by an inflated layout item");

    const QLayoutItem* item = flow->itemAt(0);
    QVERIFY(item != nullptr);
    QCOMPARE(item->geometry().size(), expected);

    // The painted chip must stay inside those bounds: the disc is inset for its
    // selection ring, and a generic button fill would make the corner opaque.
    const WidgetPixels chip = grabWidget(*swatch);
    QCOMPARE(qRound(chip.image.width() / chip.scale), expected.width());
    QCOMPARE(qRound(chip.image.height() / chip.scale), expected.height());
    const QColor center = chip.at(expected.width() / 2, expected.height() / 2);
    QVERIFY2(nearColor(center, QColor(QStringLiteral("#f27bb0"))),
             qPrintable(failure("swatch fill", center, QColor(QStringLiteral("#f27bb0")))));
    QCOMPARE(chip.at(0, 0).alpha(), 0);
}

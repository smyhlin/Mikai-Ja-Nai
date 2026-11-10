#pragma once

#include "state/AppearanceStore.hpp"
#include "theme/Appearance.hpp"
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"
#include "theme/Theme.hpp"

#include <array>

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace mikai::app {

/// Enables OpenType tabular figures (`tnum`) on a widget's font so live
/// counters, rate-limit timers, selection counts, and FPS tables never jitter
/// horizontally when digits change.
inline void applyTabularNumerals(QWidget* widget) {
    if (widget == nullptr) return;
    QFont font = widget->font();
    font.setFeature(QFont::Tag("tnum"), 1);
    font.setStyleStrategy(QFont::PreferAntialias);
    widget->setFont(font);
}

/// Applies a pointing-hand cursor and minimum interactive height to actionable
/// desktop controls while keeping focus policies keyboard-accessible.
inline void polishInteractiveControl(QWidget* control, int minHeight = theme::kSettingsControlSize) {
    if (control == nullptr) return;
    if (minHeight > 0) {
        control->setMinimumHeight(minHeight);
    }
    control->setCursor(Qt::PointingHandCursor);
}

/// A caption that must never set its page's width floor. The styled heading and
/// section-heading fonts are wide and a translated caption is wider still: left
/// to its own size hint a caption votes its whole advance into the page, and a
/// page wider than its viewport can only be read by panning it sideways. Under
/// an Ignored horizontal policy the caption yields and wraps instead.
inline void yieldCaptionWidth(QLabel* label) {
    if (label == nullptr) return;
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, label->sizePolicy().verticalPolicy());
}

/// A check box that wraps its own caption. QCheckBox paints its caption on one
/// line and never elides it, so a translated caption votes its whole advance
/// into the page and the page either pans sideways or clips the words. Here the
/// caption is measured and drawn with word wrapping beside the indicator, and
/// the wrapped height is what the layout receives; text() still carries the
/// whole caption, so the accessible name, the mnemonic and every translation
/// keep reading it.
class WrappingCheckBox final : public QCheckBox {
 public:
    explicit WrappingCheckBox(QWidget* parent = nullptr) : QCheckBox(parent) {
        // A layout asks qSmartMinSize first, and a policy without the shrink
        // flag is answered with max(sizeHint, minimumSizeHint) - the whole
        // one-line caption - before minimumSizeHint is ever consulted. Only a
        // shrinkable policy lets the wrapped minimum below stand.
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    }

    QSize minimumSizeHint() const override {
        const QSize base = QCheckBox::sizeHint();
        // The width below which no word fits on a line; the height is left to
        // heightForWidth at the width the layout actually hands out, or a row
        // that wraps twice would still reserve the height of its narrowest
        // possible wrap.
        return QSize(horizontalChrome(base) + longestWordAdvance(), base.height());
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override {
        const QSize base = QCheckBox::sizeHint();
        const int textWidth = qMax(0, width - horizontalChrome(base));
        const int lines = wrappedLines(textWidth).size();
        return qMax(base.height(),
                    base.height() - fontMetrics().height() + lines * fontMetrics().lineSpacing());
    }

 protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QStyleOptionButton option;
        initStyleOption(&option);
        // The style draws the indicator, the surface and the label in the
        // sheet's own ink and mnemonic handling; the caption it receives is the
        // same words on as many lines as the box has room for.
        option.text =
            wrappedLines(qMax(0, horizontalWidth(option.rect.width()))).join(QLatin1Char('\n'));
        QPainter painter(this);
        style()->drawControl(QStyle::CE_CheckBox, &option, &painter, this);
    }

 private:
    /// Everything a single-line caption adds beside its own advance: the
    /// indicator, the label spacing and the sheet's box model, as the style
    /// computes them for this control. The style measures the caption with its
    /// mnemonics shown, so the advance it counted has no `&` in it either.
    int horizontalChrome(const QSize& singleLine) const {
        QString spoken = text();
        spoken.remove(QLatin1Char('&'));
        return qMax(0, singleLine.width() - fontMetrics().horizontalAdvance(spoken));
    }

    int horizontalWidth(int widgetWidth) const {
        return widgetWidth - horizontalChrome(QCheckBox::sizeHint());
    }

    /// The caption's words greedily packed into the width the box has, measured
    /// without the mnemonic markers the style turns into underlines.
    QStringList wrappedLines(int textWidth) const {
        const QString caption = text();
        if (caption.isEmpty()) return {};
        if (textWidth <= 0) return {caption};
        const QStringList words = caption.split(QLatin1Char(' '));
        const auto advance = [this](const QString& line) {
            QString spoken = line;
            spoken.remove(QLatin1Char('&'));
            return fontMetrics().horizontalAdvance(spoken);
        };
        QStringList lines;
        QString line;
        for (const QString& word : words) {
            const QString candidate = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
            if (!line.isEmpty() && advance(candidate) > textWidth) {
                lines.append(line);
                line = word;
            } else {
                line = candidate;
            }
        }
        if (!line.isEmpty()) lines.append(line);
        return lines;
    }

    /// The width below which no word can stand on its own line: a caption wraps
    /// at any width down to this one and never below it.
    int longestWordAdvance() const {
        int widest = 0;
        for (const QStringView part : QStringView(text()).split(QLatin1Char(' '))) {
            QString word = part.toString();
            word.remove(QLatin1Char('&'));
            widest = qMax(widest, fontMetrics().horizontalAdvance(word));
        }
        return widest;
    }
};

/// Styles a top-level page heading with balanced weight and tight tracking.
inline void stylePageHeading(QLabel* label) {
    if (label == nullptr) return;
    label->setObjectName(QStringLiteral("heading"));
    yieldCaptionWidth(label);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, -0.3);
    font.setStyleStrategy(QFont::PreferAntialias);
    label->setFont(font);
}

/// Styles a card or section heading with medium-bold weight and snug tracking.
inline void styleSectionHeading(QLabel* label) {
    if (label == nullptr) return;
    label->setObjectName(QStringLiteral("sectionHeading"));
    yieldCaptionWidth(label);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, -0.15);
    font.setStyleStrategy(QFont::PreferAntialias);
    label->setFont(font);
}

/// Styles a muted description or helper caption for comfortable reading.
inline void styleMutedCaption(QLabel* label) {
    if (label == nullptr) return;
    label->setObjectName(QStringLiteral("muted"));
    yieldCaptionWidth(label);
}

/// Styles a compact pill counter or status badge (e.g., "3 / 5 selected", "Ready").
inline void stylePillBadge(QLabel* label, const QString& tone = QStringLiteral("purple")) {
    if (label == nullptr) return;
    label->setProperty("badgeTone", tone);
    label->setAlignment(Qt::AlignCenter);
    applyTabularNumerals(label);
    theme::repolish(label);
}

/// Styles a semantic status/error/callout banner (`"neutral"`, `"info"`,
/// `"success"`, `"warning"`, `"danger"`).
inline void styleStatusBanner(QLabel* label, const QString& state) {
    if (label == nullptr) return;
    label->setProperty("statusTone", state);
    applyTabularNumerals(label);
    theme::repolish(label);
}

/// Creates a 1px hairline horizontal divider within a card or between sections.
inline QFrame* makeSettingsDivider(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setObjectName(QStringLiteral("settingsDivider"));
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setFixedHeight(1);
    return line;
}

/// Creates a styled surface card with concentric radius and standard internal
/// margins (`kSpacingMedium` = 16px) and vertical rhythm.
inline QFrame* makeSettingsCard(QWidget* parent, QVBoxLayout** layoutOut) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("settingsCard"));
    card->setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(card);
    const int pad = theme::kSpacingMedium;
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme::kSpacingSmall);
    if (layoutOut != nullptr) *layoutOut = layout;
    return card;
}

/// Creates an inset sub-panel inside a card (concentric inner radius `kRadiusMd`)
/// for live telemetry, quota readouts, or grouped controls.
inline QFrame* makeSettingsSubCard(QWidget* parent, QVBoxLayout** layoutOut) {
    auto* subCard = new QFrame(parent);
    subCard->setObjectName(QStringLiteral("settingsSubCard"));
    subCard->setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(subCard);
    const int padH = theme::kSpacingMedium;
    const int padV = theme::kSpacingSmall + theme::kSpacingXs;
    layout->setContentsMargins(padH, padV, padH, padV);
    layout->setSpacing(theme::kSpacingXs);
    if (layoutOut != nullptr) *layoutOut = layout;
    return subCard;
}

/// Creates a unified page header block (`heading` + `muted` subtitle) so every
/// settings tab shares a calm, editorial top hierarchy.
inline QFrame* makeSettingsPageHeader(QWidget* parent, QLabel** headingOut,
                                      QLabel** descriptionOut, const QString& titleText,
                                      const QString& descriptionText,
                                      QWidget* leadingBadge = nullptr) {
    auto* headerFrame = new QFrame(parent);
    headerFrame->setObjectName(QStringLiteral("settingsPageHeader"));
    auto* headerLayout = new QVBoxLayout(headerFrame);
    headerLayout->setContentsMargins(theme::kSpacingSmall, theme::kSpacingSmall,
                                     theme::kSpacingSmall, theme::kSpacingXs);
    headerLayout->setSpacing(theme::kSpacingXs);

    auto* titleRow = new QHBoxLayout();
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(theme::kSpacingSmall);
    if (leadingBadge != nullptr) {
        leadingBadge->setParent(headerFrame);
        titleRow->addWidget(leadingBadge, 0, Qt::AlignVCenter);
    }
    auto* heading = new QLabel(titleText, headerFrame);
    stylePageHeading(heading);
    titleRow->addWidget(heading, 1, Qt::AlignVCenter);
    headerLayout->addLayout(titleRow);

    auto* description = new QLabel(descriptionText, headerFrame);
    styleMutedCaption(description);
    headerLayout->addWidget(description);

    if (headingOut != nullptr) *headingOut = heading;
    if (descriptionOut != nullptr) *descriptionOut = description;
    return headerFrame;
}

/// Builds the Slight Purple QSS stylesheet for a settings page or scroll area.
/// Designed so nested surfaces follow concentric radii (`16px` card -> `12px`
/// sub-card -> `8px` control -> `6px` badge) and slight-purple accents highlight
/// interactive focus, checked states, and active telemetry without visual noise.
inline QString settingsStylesheet(const theme::Appearance& appearance) {
    const theme::Palette p = theme::paletteFor(appearance);
    QString sheet = QStringLiteral(R"QSS(
        /* ── Page & Scroll Surfaces ────────────────────────────────────────── */
        QScrollArea#nativeGeneralScroll,
        QScrollArea#nativeProcessingScroll,
        QScrollArea#benchmarkScroll,
        QScrollArea#presetsScroll,
        QScrollArea#componentsScroll {
            background-color: transparent;
            border: none;
        }
        QWidget#benchmarkPage,
        QWidget#presetsPage,
        QWidget#generalPage,
        QWidget#processingPage,
        QWidget#componentsPage,
        QWidget#customizationGeneralPage,
        QWidget#appearancePage {
            background-color: %surfacePage%;
            color: %textPrimary%;
        }

        /* ── Navigation ───────────────────────────────────────────────────── */
        QWidget#settingsNav { background: transparent; }
        QLabel#settingsNavGroup {
            color: %textMutedSoft%;
            font-size: 12px;
            font-weight: 600;
            padding: 14px 10px 4px 10px;
            background: transparent;
        }
        QPushButton#settingsNavItem {
            background: transparent;
            color: %textLabel%;
            border: 1px solid transparent;
            border-radius: %radiusSmall%px;
            padding: 0 %controlInsetH%px;
            text-align: left;
            font-weight: 500;
        }
        QPushButton#settingsNavItem:hover {
            background-color: %accentDeep%;
            color: %textStrong%;
        }
        QPushButton#settingsNavItem:checked {
            background-color: %surfaceRow%;
            color: %textStrong%;
            font-weight: 600;
            border-color: rgba(%hairlineRgb%, 0.16);
        }
        QPushButton#settingsNavItem:focus { border-color: %accentFocus%; }

        /* ── Appearance controls ──────────────────────────────────────────── */
        QPushButton#radiusChip {
            background-color: %surfaceInput%;
            color: %textLabel%;
            border: 1px solid rgba(%hairlineRgb%, 0.14);
            border-radius: %radiusSmall%px;
            padding: 0 %controlInsetHPrimary%px;
            font-weight: 500;
        }
        QPushButton#radiusChip:hover { background-color: %surfaceControlHover%; color: %textStrong%; }
        QPushButton#radiusChip:checked {
            background-color: %accentSolid%;
            color: %textOnAccent%;
            border-color: %accentSolid%;
            font-weight: 600;
        }
        QLabel#accentValueLabel, QLabel#gradientValueLabel {
            color: %textLabel%;
            font-size: 12px;
            font-weight: 600;
            background: transparent;
        }
        QLabel#appearanceValue {
            color: %textStrong%;
            font-size: 12px;
            font-weight: 600;
            background: transparent;
        }
        QSlider#appearanceSlider { background: transparent; min-height: 22px; }
        QSlider#appearanceSlider::groove:horizontal {
            height: 4px;
            background: %surfaceInset%;
            border-radius: 2px;
        }
        QSlider#appearanceSlider::sub-page:horizontal {
            background: %accentSolid%;
            border-radius: 2px;
        }
        QSlider#appearanceSlider::handle:horizontal {
            background: %accentSolid%;
            border: 2px solid %surfaceRaised%;
            width: 12px;
            height: 12px;
            margin: -5px 0;
            border-radius: 7px;
        }
        QSlider#appearanceSlider::handle:horizontal:hover { background: %accentSolid%; }
        QSlider#appearanceSlider::handle:horizontal:disabled { background: %surfaceInset%; border-color: %surfacePage%; }
        QSlider#appearanceSlider::sub-page:horizontal:disabled { background: %textDisabled%; }

        /* ── Elevated Cards & Concentric Sub-Panels ────────────────────────── */
        QFrame#settingsCard {
            background-color: %surfaceRaised%;
            border: 1px solid rgba(%hairlineRgb%, 0.12);
            border-radius: %radiusMedium%px;
        }
        QFrame#settingsCard:hover {
            border-color: rgba(%hairlineRgb%, 0.20);
        }
        QFrame#settingsSubCard {
            background-color: %surfaceInset%;
            border: 1px solid rgba(%hairlineRgb%, 0.10);
            border-radius: %radiusMedium%px;
        }
        QFrame#settingsPageHeader,
        QFrame#benchmarkHeader {
            background-color: %surfaceRaised%;
            border: 1px solid rgba(%hairlineRgb%, 0.12);
            border-radius: %radiusMedium%px;
        }
        QFrame#settingsDivider {
            background-color: rgba(%hairlineRgb%, 0.10);
            border: none;
            max-height: 1px;
        }

        /* ── Typography Hierarchy ──────────────────────────────────────────── */
        QLabel#heading {
            color: %textStrong%;
            font-size: 17px;
            font-weight: 600;
            background: transparent;
        }
        QLabel#sectionHeading {
            color: %textBright%;
            font-size: 14px;
            font-weight: 600;
            background: transparent;
        }
        QLabel#muted,
        QLabel[textRole="muted"] {
            color: %textMutedSoft%;
            font-size: 12px;
            background: transparent;
        }
        QLabel#SettingsFormLabel {
            color: %textLabel%;
            font-size: 13px;
            font-weight: 500;
            background: transparent;
        }

        /* ── Telemetry, Description & Status Callouts ──────────────────────── */
        QLabel#descriptionPanel {
            background-color: %surfaceInset%;
            color: %textLabel%;
            border: 1px solid rgba(%hairlineRgb%, 0.12);
            border-radius: %radiusMedium%px;
            padding: 10px 12px;
            font-size: 12px;
        }
        QLabel#status {
            background-color: rgba(%accentRgb%, 0.08);
            color: %accentSoftBright%;
            border: 1px solid rgba(%accentRgb%, 0.22);
            border-radius: %radiusMedium%px;
            padding: 8px 12px;
            font-size: 12px;
        }
        QLabel#status[statusTone="success"] {
            background-color: rgba(%successRgb%, 0.10);
            color: %successText%;
            border: 1px solid rgba(%successRgb%, 0.28);
        }
        QLabel#status[statusTone="warning"] {
            background-color: rgba(%warningRgb%, 0.10);
            color: %warningText%;
            border: 1px solid rgba(%warningRgb%, 0.28);
        }
        QLabel#status[statusTone="danger"] {
            background-color: rgba(%dangerRgb%, 0.12);
            color: %dangerText%;
            border: 1px solid rgba(%dangerRgb%, 0.32);
        }
        QLabel#status[statusTone="info"] {
            background-color: rgba(%accentRgb%, 0.12);
            color: %accentSoftBright%;
            border: 1px solid rgba(%accentRgb%, 0.30);
        }

        /* ── Pill Counters & Status Badges ─────────────────────────────────── */
        QLabel#benchmarkClipCount,
        QLabel#benchmarkModelCount,
        QLabel#benchmarkRunStatus,
        QLabel[badgeTone="purple"] {
            background-color: rgba(%accentRgb%, 0.12);
            color: %accentSoft%;
            border: 1px solid rgba(%accentRgb%, 0.24);
            border-radius: %radiusPill%px;
            padding: 3px 9px;
            font-size: 11px;
            font-weight: 600;
        }
        QLabel#benchmarkRunStatus[statusTone="success"] {
            background-color: rgba(%successRgb%, 0.12);
            color: %successText%;
            border-color: rgba(%successRgb%, 0.30);
        }
        QLabel#benchmarkRunStatus[statusTone="warning"],
        QLabel#benchmarkRunStatus[statusTone="danger"] {
            background-color: rgba(%dangerRgb%, 0.12);
            color: %dangerText%;
            border-color: rgba(%dangerRgb%, 0.32);
        }
        QLabel#benchmarkRunStatus[statusTone="running"] {
            background-color: rgba(%accentSolidRgb%, 0.20);
            color: %textBright%;
            border-color: rgba(%accentRgb%, 0.45);
        }

        /* ── Inputs, Search Fields, Combos & KeySequence Editors ───────────── */
        QLineEdit,
        QComboBox,
        QKeySequenceEdit > QLineEdit {
            background-color: %surfaceInput%;
            color: %textPrimary%;
            border: 1px solid rgba(%hairlineRgb%, 0.16);
            border-radius: %radiusSmall%px;
            padding: 5px 11px;
            selection-background-color: %accentDeep%;
            selection-color: %textOnAccent%;
        }
        QLineEdit:hover,
        QComboBox:hover,
        QKeySequenceEdit > QLineEdit:hover {
            border-color: rgba(%accentRgb%, 0.36);
            background-color: %surfaceInputHover%;
        }
        QLineEdit:focus,
        QComboBox:focus,
        QComboBox:on,
        QKeySequenceEdit > QLineEdit:focus {
            border: 1px solid %accentFocus%;
            background-color: %surfaceInputFocus%;
        }
        QLineEdit:disabled,
        QComboBox:disabled,
        QKeySequenceEdit > QLineEdit:disabled {
            color: %textDisabled%;
            background-color: rgba(%surfaceDeepRgb%, 0.55);
            border-color: rgba(%hairlineRgb%, 0.07);
        }
        QComboBox QAbstractItemView {
            background-color: %surfaceRow%;
            color: %textPrimary%;
            border: 1px solid rgba(%accentRgb%, 0.28);
            border-radius: %radiusSmall%px;
            padding: 4px;
            selection-background-color: rgba(%accentSolidRgb%, 0.28);
            selection-color: %textOnAccent%;
            outline: 0;
        }

        /* ── Action roles (see theme::ActionRole) ───────────────────────────
           The settings page states the same hierarchy the application sheet
           does: the bare rule is the secondary role, so the neutral controls
           carry the 3:1 boundary the raised fill alone cannot, and hover lifts
           toward the accent. A primary keeps the accent as its fill and takes a
           contrast-derived ink, which is what keeps a very dark custom accent
           readable. */
        QPushButton {
            background-color: %surfaceControlRaised%;
            color: %textBright%;
            border: 1px solid %borderControl%;
            border-radius: %radiusSmall%px;
            min-height: %controlContentHeight%px;
            max-height: %controlContentHeight%px;
            padding: 0 %controlInsetH%px;
            font-weight: 500;
            text-align: center;
        }
        QPushButton:hover {
            background-color: rgba(%accentRgb%, 0.12);
            border-color: rgba(%accentRgb%, 0.38);
            color: %textOnAccent%;
        }
        QPushButton:pressed {
            background-color: %surfaceControlPressed%;
            border-color: %accentFocus%;
        }
        QPushButton:checked {
            background-color: rgba(%accentRgb%, 0.14);
            border-color: %accentSolid%;
            color: %textOnAccent%;
        }
        QPushButton:focus {
            border-color: %accentFocus%;
        }
        QPushButton:disabled {
            background-color: %surfaceInput%;
            color: rgba(%textPrimaryRgb%, 0.55);
            border-color: %borderControl%;
        }
        QPushButton[actionRole="primary"] {
            background-color: %accentSolid%;
            color: %accentInk%;
            border: 1px solid %accentSolid%;
            border-radius: %radiusSmall%px;
            padding: 0 %controlInsetHPrimary%px;
            font-weight: 600;
        }
        QPushButton[actionRole="primary"]:hover {
            background-color: %accentHover%;
            border-color: %accentHover%;
        }
        QPushButton[actionRole="primary"]:pressed {
            background-color: %accentSolid%;
            border-color: %accentDeepest%;
        }
        QPushButton[actionRole="primary"]:checked {
            background-color: %accentHover%;
            border-color: %accentInk%;
        }
        QPushButton[actionRole="primary"]:focus {
            border-color: %accentInk%;
        }
        QPushButton[actionRole="primary"]:disabled {
            background-color: %surfaceInput%;
            color: rgba(%textPrimaryRgb%, 0.55);
            border-color: %borderControl%;
        }
        QPushButton[actionRole="ghost"] {
            background-color: transparent;
            color: %accentSolid%;
            border: 1px solid rgba(%accentRgb%, 0.22);
            border-radius: %radiusSmall%px;
            min-height: %controlContentHeight%px;
            max-height: %controlContentHeight%px;
            padding: 0 %controlInsetH%px;
            font-size: 12px;
            font-weight: 500;
        }
        QPushButton[actionRole="ghost"]:hover {
            background-color: rgba(%accentRgb%, 0.16);
            border-color: rgba(%accentRgb%, 0.45);
            color: %accentHover%;
        }
        QPushButton[actionRole="ghost"]:pressed {
            background-color: rgba(%accentRgb%, 0.20);
        }
        QPushButton[actionRole="ghost"]:checked {
            background-color: rgba(%accentRgb%, 0.18);
            border-color: %accentSolid%;
            color: %accentHover%;
        }
        QPushButton[actionRole="ghost"]:focus {
            border-color: %accentSolid%;
        }
        QPushButton[actionRole="ghost"]:disabled {
            color: rgba(%textPrimaryRgb%, 0.55);
            border-color: %borderControl%;
        }
        QPushButton[actionRole="danger"] {
            background-color: transparent;
            color: %dangerFill%;
            border: 1px solid %dangerFill%;
            font-weight: 600;
        }
        QPushButton[actionRole="danger"]:hover {
            background-color: rgba(%dangerRgb%, 0.14);
        }
        QPushButton[actionRole="danger"]:pressed {
            background-color: rgba(%dangerRgb%, 0.22);
        }
        QPushButton[actionRole="danger"]:checked {
            background-color: rgba(%dangerRgb%, 0.18);
            border-color: %dangerFill%;
        }
        QPushButton[actionRole="danger"]:focus {
            background-color: rgba(%dangerRgb%, 0.10);
            border-color: %dangerFill%;
        }
        QPushButton[actionRole="danger"]:disabled {
            color: rgba(%textPrimaryRgb%, 0.55);
            border-color: %borderControl%;
        }
        /* A chip paints its own disc at one fixed size. The size is stated in
           all four directions because the generic button rule above sets a
           minimum height that would otherwise replace the caller's fixed size
           and leave the chip at its bare hint. */
        QPushButton#accentSwatch, QPushButton#gradientSwatch,
        QPushButton#customAccentSwatch, QPushButton#customGradientSwatch {
            background: transparent;
            border: none;
            padding: 0;
            min-width: %settingsSwatchSize%px;
            max-width: %settingsSwatchSize%px;
            min-height: %settingsSwatchSize%px;
            max-height: %settingsSwatchSize%px;
            border-radius: 0;
        }

        /* ── Checkboxes & Selectable Resolution / Feed Tiles ───────────────── */
        QCheckBox {
            color: %textCheckLabel%;
            spacing: 8px;
            padding: 5px 8px;
            border-radius: 8px;
        }
        QCheckBox:hover {
            background-color: rgba(%accentRgb%, 0.06);
        }
        QCheckBox#benchmarkClipCheck,
        QCheckBox[tileCheck="true"] {
            background-color: %surfaceTag%;
            border: 1px solid rgba(%hairlineRgb%, 0.12);
            border-radius: %radiusSmall%px;
            padding: 6px 10px;
            font-weight: 500;
        }
        QCheckBox#benchmarkClipCheck:hover,
        QCheckBox[tileCheck="true"]:hover {
            background-color: %surfaceTileHover%;
            border-color: rgba(%accentRgb%, 0.32);
        }
        QCheckBox#benchmarkClipCheck:checked,
        QCheckBox[tileCheck="true"]:checked {
            background-color: rgba(%accentSolidRgb%, 0.14);
            border-color: rgba(%accentRgb%, 0.42);
            color: %textStrong%;
        }
        QCheckBox::indicator {
            width: 16px;
            height: 16px;
            border-radius: 4px;
            border: 1px solid rgba(%hairlineRgb%, 0.30);
            background-color: %surfaceInput%;
        }
        QCheckBox::indicator:hover {
            border-color: %accentFocus%;
        }
        QCheckBox::indicator:checked {
            background-color: %accentSolid%;
            border-color: %accentFocus%;
        }

        /* ── Preset Shortcut Rows ──────────────────────────────────────────── */
        QWidget#presetShortcutRow {
            background-color: %surfaceShortcutRow%;
            border: 1px solid rgba(%hairlineRgb%, 0.09);
            border-radius: %radiusMedium%px;
        }
        QWidget#presetShortcutRow:hover {
            background-color: %surfaceControl%;
            border-color: rgba(%accentRgb%, 0.26);
        }
        QWidget#presetShortcutRow[hasBinding="true"] {
            background-color: rgba(%accentSolidRgb%, 0.10);
            border: 1px solid rgba(%accentRgb%, 0.34);
        }
        QWidget#presetShortcutRow[hasBinding="true"]:hover {
            background-color: rgba(%accentSolidRgb%, 0.15);
            border-color: rgba(%accentRgb%, 0.48);
        }

        /* ── Benchmark Model Tree & Results Table ──────────────────────────── */
        QTreeWidget#benchmarkModelTree {
            background-color: %surfaceInput%;
            color: %textPrimary%;
            border: 1px solid rgba(%hairlineRgb%, 0.14);
            border-radius: %radiusMedium%px;
            padding: 6px;
            outline: 0;
        }
        QTreeWidget#benchmarkModelTree::item {
            padding: 5px 6px;
            border-radius: 6px;
        }
        QTreeWidget#benchmarkModelTree::item:hover {
            background-color: rgba(%accentRgb%, 0.10);
        }
        QTableWidget#benchmarkResults {
            background-color: %surfaceInput%;
            alternate-background-color: %tableAlternateRow%;
            color: %textPrimary%;
            border: 1px solid rgba(%hairlineRgb%, 0.14);
            border-radius: %radiusMedium%px;
            selection-background-color: rgba(%accentSolidRgb%, 0.24);
            selection-color: %textOnAccent%;
            outline: 0;
        }
        QTableWidget#benchmarkResults QHeaderView::section {
            background-color: %surfaceTag%;
            color: %accentSoft%;
            font-weight: 600;
            font-size: 12px;
            padding: 6px 10px;
            border: none;
            border-bottom: 1px solid rgba(%hairlineRgb%, 0.14);
        }
        QLabel#benchmarkEmptyState {
            background-color: %surfaceInput%;
            color: %textMutedSoft%;
            border: 1px dashed rgba(%hairlineRgb%, 0.18);
            border-radius: %radiusMedium%px;
            padding: 20px;
        }
        QLabel#benchmarkResultDetail {
            background-color: rgba(%dangerRgb%, 0.10);
            color: %dangerText%;
            border: 1px solid rgba(%dangerRgb%, 0.28);
            border-radius: %radiusSmall%px;
            padding: 8px 12px;
        }

        /* ── Progress Bars (Slight Purple Telemetry) ───────────────────────── */
        QProgressBar#benchmarkProgress,
        QProgressBar#benchmarkEngineProgress {
            background-color: %surfaceInput%;
            border: 1px solid rgba(%hairlineRgb%, 0.16);
            border-radius: 5px;
            min-height: 8px;
            max-height: 8px;
        }
        QProgressBar#benchmarkProgress::chunk,
        QProgressBar#benchmarkEngineProgress::chunk {
            background-color: %accentSolid%;
            border-radius: 4px;
        }
        QProgressBar#benchmarkProgress[runState="success"]::chunk {
            background-color: %successFill%;
        }
        QProgressBar#benchmarkProgress[runState="failure"]::chunk {
            background-color: %dangerFill%;
        }

        /* ── Benchmark Sticky Footer Bar ───────────────────────────────────── */
        QWidget#benchmarkBottomBar {
            background-color: %surfaceRaised%;
            border-top: 1px solid rgba(%hairlineRgb%, 0.14);
        }

        /* ── Minimal Sleek Scrollbars ──────────────────────────────────────── */
        QScrollBar:vertical {
            background: transparent;
            width: 8px;
            margin: 2px 1px 2px 1px;
        }
        QScrollBar::handle:vertical {
            background: rgba(%accentRgb%, 0.24);
            min-height: 28px;
            border-radius: 3px;
        }
        QScrollBar::handle:vertical:hover {
            background: rgba(%accentRgb%, 0.45);
        }
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {
            background: none;
            height: 0px;
        }
    )QSS";

    // X-D24: markers are named, not numbered. The old 52-positional
    // .arg() chain filled the lowest remaining %N, so deleting a rule
    // shifted every later marker by three and once painted a table-row
    // colour as a control border and a success fill as a primary label
    // ink. A (marker, value) pair can only ever fill its own marker.
    const std::array<std::pair<QString, QString>, 52> replacements{{
        {QStringLiteral("%surfacePage%"), p.surfacePage},
        {QStringLiteral("%textPrimary%"), p.textPrimary},
        {QStringLiteral("%surfaceRaised%"), p.surfaceRaised},
        {QStringLiteral("%hairlineRgb%"), p.hairlineRgb},
        {QStringLiteral("%surfaceInset%"), p.surfaceInset},
        {QStringLiteral("%textStrong%"), p.textStrong},
        {QStringLiteral("%textBright%"), p.textBright},
        {QStringLiteral("%textMutedSoft%"), p.textMutedSoft},
        {QStringLiteral("%textLabel%"), p.textLabel},
        {QStringLiteral("%accentRgb%"), p.accentRgb},
        {QStringLiteral("%accentSoftBright%"), p.accentSoftBright},
        {QStringLiteral("%successRgb%"), p.successRgb},
        {QStringLiteral("%successText%"), p.successText},
        {QStringLiteral("%warningRgb%"), p.warningRgb},
        {QStringLiteral("%warningText%"), p.warningText},
        {QStringLiteral("%dangerRgb%"), p.dangerRgb},
        {QStringLiteral("%dangerText%"), p.dangerText},
        {QStringLiteral("%accentSoft%"), p.accentSoft},
        {QStringLiteral("%accentSolidRgb%"), p.accentSolidRgb},
        {QStringLiteral("%surfaceInput%"), p.surfaceInput},
        {QStringLiteral("%accentDeep%"), p.accentDeep},
        {QStringLiteral("%textOnAccent%"), p.textOnAccent},
        {QStringLiteral("%surfaceInputHover%"), p.surfaceInputHover},
        {QStringLiteral("%accentFocus%"), p.accentFocus},
        {QStringLiteral("%surfaceInputFocus%"), p.surfaceInputFocus},
        {QStringLiteral("%textDisabled%"), p.textDisabled},
        {QStringLiteral("%surfaceDeepRgb%"), p.surfaceDeepRgb},
        {QStringLiteral("%surfaceRow%"), p.surfaceRow},
        {QStringLiteral("%surfaceControl%"), p.surfaceControl},
        {QStringLiteral("%surfaceControlHover%"), p.surfaceControlHover},
        {QStringLiteral("%surfaceControlPressed%"), p.surfaceControlPressed},
        {QStringLiteral("%accentSolid%"), p.accentSolid},
        {QStringLiteral("%accentDeepest%"), p.accentDeepest},
        {QStringLiteral("%textPrimaryRgb%"), p.textPrimaryRgb},
        {QStringLiteral("%textCheckLabel%"), p.textCheckLabel},
        {QStringLiteral("%surfaceTag%"), p.surfaceTag},
        {QStringLiteral("%surfaceTileHover%"), p.surfaceTileHover},
        {QStringLiteral("%surfaceShortcutRow%"), p.surfaceShortcutRow},
        {QStringLiteral("%tableAlternateRow%"), p.tableAlternateRow},
        {QStringLiteral("%successFill%"), p.successFill},
        {QStringLiteral("%dangerFill%"), p.dangerFill},
        {QStringLiteral("%surfaceControlRaised%"), p.surfaceControlRaised},
        {QStringLiteral("%borderControl%"), p.borderControl},
        {QStringLiteral("%accentInk%"), p.accentInk},
        {QStringLiteral("%accentHover%"), p.accentHover},
        {QStringLiteral("%settingsSwatchSize%"), QString::number(theme::kSettingsSwatchSize)},
        {QStringLiteral("%radiusSmall%"), QString::number(theme::radiusSmall(appearance))},
        {QStringLiteral("%controlContentHeight%"), QString::number(theme::kControlContentHeight)},
        {QStringLiteral("%controlInsetH%"), QString::number(theme::kControlInsetH)},
        {QStringLiteral("%controlInsetHPrimary%"), QString::number(theme::kControlInsetHPrimary)},
        {QStringLiteral("%radiusMedium%"), QString::number(theme::radiusMedium(appearance))},
        {QStringLiteral("%radiusPill%"), QString::number(theme::radiusPill(appearance))},
    }};
    for (const auto& [marker, value] : replacements) sheet.replace(marker, value);
    // The page's control and card corners are the appearance's own radius
    // steps, not a second scale computed here.
    return sheet;
}

}  // namespace mikai::app

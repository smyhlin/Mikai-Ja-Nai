#pragma once

#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

#include <QAbstractButton>
#include <QComboBox>
#include <QCursor>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QString>
#include <QStyle>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace mikai::app {

/// Re-polishes a widget after a dynamic property change so QSS selectors update
/// immediately without rebuilding the widget hierarchy.
inline void repolishWidget(QWidget* widget) {
    if (widget == nullptr || widget->style() == nullptr) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

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
    repolishWidget(label);
}

/// Styles a semantic status/error/callout banner (`"neutral"`, `"info"`,
/// `"success"`, `"warning"`, `"danger"`).
inline void styleStatusBanner(QLabel* label, const QString& state) {
    if (label == nullptr) return;
    label->setProperty("statusTone", state);
    applyTabularNumerals(label);
    repolishWidget(label);
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
inline QString settingsPurpleStyleSheet() {
    static const QString sheet = QStringLiteral(R"QSS(
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
        QWidget#componentsPage {
            background-color: %1;
            color: %2;
        }

        /* ── Elevated Cards & Concentric Sub-Panels ────────────────────────── */
        QFrame#settingsCard {
            background-color: %3;
            border: 1px solid rgba(%4, 0.12);
            border-radius: 16px;
        }
        QFrame#settingsCard:hover {
            border-color: rgba(%4, 0.20);
        }
        QFrame#settingsSubCard {
            background-color: %5;
            border: 1px solid rgba(%4, 0.10);
            border-radius: 12px;
        }
        QFrame#settingsPageHeader,
        QFrame#benchmarkHeader {
            background-color: %3;
            border: 1px solid rgba(%4, 0.12);
            border-radius: 16px;
        }
        QFrame#settingsDivider {
            background-color: rgba(%4, 0.10);
            border: none;
            max-height: 1px;
        }

        /* ── Typography Hierarchy ──────────────────────────────────────────── */
        QLabel#heading {
            color: %6;
            font-size: 17px;
            font-weight: 600;
            background: transparent;
        }
        QLabel#sectionHeading {
            color: %7;
            font-size: 14px;
            font-weight: 600;
            background: transparent;
        }
        QLabel#muted,
        QLabel[textRole="muted"] {
            color: %8;
            font-size: 12px;
            background: transparent;
        }
        QLabel#SettingsFormLabel {
            color: %9;
            font-size: 13px;
            font-weight: 500;
            background: transparent;
        }

        /* ── Telemetry, Description & Status Callouts ──────────────────────── */
        QLabel#descriptionPanel {
            background-color: %5;
            color: %9;
            border: 1px solid rgba(%4, 0.12);
            border-radius: 10px;
            padding: 10px 12px;
            font-size: 12px;
        }
        QLabel#status {
            background-color: rgba(%10, 0.08);
            color: %11;
            border: 1px solid rgba(%10, 0.22);
            border-radius: 8px;
            padding: 8px 12px;
            font-size: 12px;
        }
        QLabel#status[statusTone="success"] {
            background-color: rgba(%12, 0.10);
            color: %13;
            border: 1px solid rgba(%12, 0.28);
        }
        QLabel#status[statusTone="warning"] {
            background-color: rgba(%14, 0.10);
            color: %15;
            border: 1px solid rgba(%14, 0.28);
        }
        QLabel#status[statusTone="danger"] {
            background-color: rgba(%16, 0.12);
            color: %17;
            border: 1px solid rgba(%16, 0.32);
        }
        QLabel#status[statusTone="info"] {
            background-color: rgba(%10, 0.12);
            color: %11;
            border: 1px solid rgba(%10, 0.30);
        }

        /* ── Pill Counters & Status Badges ─────────────────────────────────── */
        QLabel#benchmarkClipCount,
        QLabel#benchmarkModelCount,
        QLabel#benchmarkRunStatus,
        QLabel[badgeTone="purple"] {
            background-color: rgba(%10, 0.12);
            color: %18;
            border: 1px solid rgba(%10, 0.24);
            border-radius: 6px;
            padding: 3px 9px;
            font-size: 11px;
            font-weight: 600;
        }
        QLabel#benchmarkRunStatus[statusTone="success"] {
            background-color: rgba(%12, 0.12);
            color: %13;
            border-color: rgba(%12, 0.30);
        }
        QLabel#benchmarkRunStatus[statusTone="warning"],
        QLabel#benchmarkRunStatus[statusTone="danger"] {
            background-color: rgba(%16, 0.12);
            color: %17;
            border-color: rgba(%16, 0.32);
        }
        QLabel#benchmarkRunStatus[statusTone="running"] {
            background-color: rgba(%19, 0.20);
            color: %7;
            border-color: rgba(%10, 0.45);
        }

        /* ── Inputs, Search Fields, Combos & KeySequence Editors ───────────── */
        QLineEdit,
        QComboBox,
        QKeySequenceEdit > QLineEdit {
            background-color: %20;
            color: %2;
            border: 1px solid rgba(%4, 0.16);
            border-radius: 8px;
            padding: 5px 11px;
            selection-background-color: %21;
            selection-color: %22;
        }
        QLineEdit:hover,
        QComboBox:hover,
        QKeySequenceEdit > QLineEdit:hover {
            border-color: rgba(%10, 0.36);
            background-color: %23;
        }
        QLineEdit:focus,
        QComboBox:focus,
        QComboBox:on,
        QKeySequenceEdit > QLineEdit:focus {
            border: 1px solid %24;
            background-color: %25;
        }
        QLineEdit:disabled,
        QComboBox:disabled,
        QKeySequenceEdit > QLineEdit:disabled {
            color: %26;
            background-color: rgba(%27, 0.55);
            border-color: rgba(%4, 0.07);
        }
        QComboBox::drop-down {
            subcontrol-origin: padding;
            subcontrol-position: top right;
            width: 26px;
            border-left: 1px solid rgba(%4, 0.10);
            border-top-right-radius: 8px;
            border-bottom-right-radius: 8px;
            background: transparent;
        }
        QComboBox QAbstractItemView {
            background-color: %28;
            color: %2;
            border: 1px solid rgba(%10, 0.28);
            border-radius: 8px;
            padding: 4px;
            selection-background-color: rgba(%19, 0.28);
            selection-color: %22;
            outline: 0;
        }

        /* ── Buttons (Primary Slight-Purple CTA, Secondary & Ghost Actions) ── */
        QPushButton {
            background-color: %29;
            color: %7;
            border: 1px solid rgba(%4, 0.18);
            border-radius: 8px;
            padding: 6px 14px;
            font-weight: 500;
        }
        QPushButton:hover {
            background-color: %30;
            border-color: rgba(%10, 0.38);
            color: %22;
        }
        QPushButton:pressed {
            background-color: %31;
            border-color: %24;
        }
        QPushButton:disabled {
            background-color: rgba(%32, 0.45);
            color: %26;
            border-color: rgba(%4, 0.07);
        }
        QPushButton#benchmarkRunButton,
        QPushButton[primaryAction="true"] {
            background-color: %21;
            color: %22;
            border: 1px solid %33;
            border-radius: 8px;
            padding: 7px 18px;
            font-weight: 600;
        }
        QPushButton#benchmarkRunButton:hover,
        QPushButton[primaryAction="true"]:hover {
            background-color: %33;
            border-color: %24;
        }
        QPushButton#benchmarkRunButton:pressed,
        QPushButton[primaryAction="true"]:pressed {
            background-color: %34;
            border-color: %21;
        }
        QPushButton#benchmarkRunButton:disabled,
        QPushButton[primaryAction="true"]:disabled {
            background-color: rgba(%35, 0.28);
            color: rgba(%36, 0.42);
            border-color: rgba(%10, 0.14);
        }
        QPushButton#textAction {
            background-color: transparent;
            color: %18;
            border: 1px solid rgba(%10, 0.16);
            border-radius: 7px;
            padding: 4px 10px;
            font-size: 12px;
            font-weight: 500;
        }
        QPushButton#textAction:hover {
            background-color: rgba(%10, 0.12);
            border-color: rgba(%10, 0.35);
            color: %7;
        }
        QPushButton#textAction:pressed {
            background-color: rgba(%10, 0.20);
        }
        QPushButton#clearAllPresetsButton:hover {
            background-color: rgba(%16, 0.14);
            border-color: rgba(%16, 0.38);
            color: %37;
        }

        /* ── Checkboxes & Selectable Resolution / Feed Tiles ───────────────── */
        QCheckBox {
            color: %38;
            spacing: 8px;
            padding: 5px 8px;
            border-radius: 8px;
        }
        QCheckBox:hover {
            background-color: rgba(%10, 0.06);
        }
        QCheckBox#benchmarkClipCheck,
        QCheckBox[tileCheck="true"] {
            background-color: %39;
            border: 1px solid rgba(%4, 0.12);
            border-radius: 8px;
            padding: 6px 10px;
            font-weight: 500;
        }
        QCheckBox#benchmarkClipCheck:hover,
        QCheckBox[tileCheck="true"]:hover {
            background-color: %40;
            border-color: rgba(%10, 0.32);
        }
        QCheckBox#benchmarkClipCheck:checked,
        QCheckBox[tileCheck="true"]:checked {
            background-color: rgba(%19, 0.14);
            border-color: rgba(%10, 0.42);
            color: %6;
        }
        QCheckBox::indicator {
            width: 16px;
            height: 16px;
            border-radius: 4px;
            border: 1px solid rgba(%4, 0.30);
            background-color: %20;
        }
        QCheckBox::indicator:hover {
            border-color: %24;
        }
        QCheckBox::indicator:checked {
            background-color: %33;
            border-color: %24;
        }

        /* ── Preset Shortcut Rows ──────────────────────────────────────────── */
        QWidget#presetShortcutRow {
            background-color: %41;
            border: 1px solid rgba(%4, 0.09);
            border-radius: 10px;
        }
        QWidget#presetShortcutRow:hover {
            background-color: %29;
            border-color: rgba(%10, 0.26);
        }
        QWidget#presetShortcutRow[hasBinding="true"] {
            background-color: rgba(%19, 0.10);
            border: 1px solid rgba(%10, 0.34);
        }
        QWidget#presetShortcutRow[hasBinding="true"]:hover {
            background-color: rgba(%19, 0.15);
            border-color: rgba(%10, 0.48);
        }

        /* ── Benchmark Model Tree & Results Table ──────────────────────────── */
        QTreeWidget#benchmarkModelTree {
            background-color: %20;
            color: %2;
            border: 1px solid rgba(%4, 0.14);
            border-radius: 10px;
            padding: 6px;
            outline: 0;
        }
        QTreeWidget#benchmarkModelTree::item {
            padding: 5px 6px;
            border-radius: 6px;
        }
        QTreeWidget#benchmarkModelTree::item:hover {
            background-color: rgba(%10, 0.10);
        }
        QTableWidget#benchmarkResults {
            background-color: %20;
            alternate-background-color: %42;
            color: %2;
            border: 1px solid rgba(%4, 0.14);
            border-radius: 10px;
            selection-background-color: rgba(%19, 0.24);
            selection-color: %22;
            outline: 0;
        }
        QTableWidget#benchmarkResults QHeaderView::section {
            background-color: %39;
            color: %18;
            font-weight: 600;
            font-size: 12px;
            padding: 6px 10px;
            border: none;
            border-bottom: 1px solid rgba(%4, 0.14);
        }
        QLabel#benchmarkEmptyState {
            background-color: %20;
            color: %8;
            border: 1px dashed rgba(%4, 0.18);
            border-radius: 10px;
            padding: 20px;
        }
        QLabel#benchmarkResultDetail {
            background-color: rgba(%16, 0.10);
            color: %17;
            border: 1px solid rgba(%16, 0.28);
            border-radius: 8px;
            padding: 8px 12px;
        }

        /* ── Progress Bars (Slight Purple Telemetry) ───────────────────────── */
        QProgressBar#benchmarkProgress,
        QProgressBar#benchmarkEngineProgress {
            background-color: %20;
            border: 1px solid rgba(%4, 0.16);
            border-radius: 5px;
            min-height: 8px;
            max-height: 8px;
        }
        QProgressBar#benchmarkProgress::chunk,
        QProgressBar#benchmarkEngineProgress::chunk {
            background-color: %33;
            border-radius: 4px;
        }
        QProgressBar#benchmarkProgress[runState="success"]::chunk {
            background-color: %43;
        }
        QProgressBar#benchmarkProgress[runState="failure"]::chunk {
            background-color: %44;
        }

        /* ── Benchmark Sticky Footer Bar ───────────────────────────────────── */
        QWidget#benchmarkBottomBar {
            background-color: %3;
            border-top: 1px solid rgba(%4, 0.14);
        }

        /* ── Minimal Sleek Scrollbars ──────────────────────────────────────── */
        QScrollBar:vertical {
            background: transparent;
            width: 8px;
            margin: 2px 1px 2px 1px;
        }
        QScrollBar::handle:vertical {
            background: rgba(%10, 0.24);
            min-height: 28px;
            border-radius: 3px;
        }
        QScrollBar::handle:vertical:hover {
            background: rgba(%10, 0.45);
        }
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {
            background: none;
            height: 0px;
        }
    )QSS")
        .arg(QLatin1String(theme::kSurfacePage))
        .arg(QLatin1String(theme::kTextPrimary))
        .arg(QLatin1String(theme::kSurfaceRaised))
        .arg(QLatin1String(theme::kHairlineRgb))
        .arg(QLatin1String(theme::kSurfaceInset))
        .arg(QLatin1String(theme::kTextStrong))
        .arg(QLatin1String(theme::kTextBright))
        .arg(QLatin1String(theme::kTextMutedSoft))
        .arg(QLatin1String(theme::kTextLabel))
        .arg(QLatin1String(theme::kAccentRgb))
        .arg(QLatin1String(theme::kAccentSoftBright))
        .arg(QLatin1String(theme::kSuccessRgb))
        .arg(QLatin1String(theme::kSuccessText))
        .arg(QLatin1String(theme::kWarningRgb))
        .arg(QLatin1String(theme::kWarningText))
        .arg(QLatin1String(theme::kDangerRgb))
        .arg(QLatin1String(theme::kDangerText))
        .arg(QLatin1String(theme::kAccentSoft))
        .arg(QLatin1String(theme::kAccentSolidRgb))
        .arg(QLatin1String(theme::kSurfaceInput))
        .arg(QLatin1String(theme::kAccentDeep))
        .arg(QLatin1String(theme::kTextOnAccent))
        .arg(QLatin1String(theme::kSurfaceInputHover))
        .arg(QLatin1String(theme::kAccentFocus))
        .arg(QLatin1String(theme::kSurfaceInputFocus))
        .arg(QLatin1String(theme::kTextDisabled))
        .arg(QLatin1String(theme::kSurfaceDeepRgb))
        .arg(QLatin1String(theme::kSurfaceRow))
        .arg(QLatin1String(theme::kSurfaceControl))
        .arg(QLatin1String(theme::kSurfaceControlHover))
        .arg(QLatin1String(theme::kSurfaceControlPressed))
        .arg(QLatin1String(theme::kSurfaceControlRgb))
        .arg(QLatin1String(theme::kAccentSolid))
        .arg(QLatin1String(theme::kAccentDeepest))
        .arg(QLatin1String(theme::kAccentDeepRgb))
        .arg(QLatin1String(theme::kTextPrimaryRgb))
        .arg(QLatin1String(theme::kDangerTextSoft))
        .arg(QLatin1String(theme::kTextCheckLabel))
        .arg(QLatin1String(theme::kSurfaceTag))
        .arg(QLatin1String(theme::kSurfaceTileHover))
        .arg(QLatin1String(theme::kSurfaceShortcutRow))
        .arg(QLatin1String(theme::kTableAlternateRow))
        .arg(QLatin1String(theme::kSuccessFill))
        .arg(QLatin1String(theme::kDangerFill));
    return sheet;
}

/// Applies the Slight Purple Refined UI stylesheet to a settings tab container
/// if it has not already been applied.
inline void applySettingsTheme(QWidget* root) {
    if (root == nullptr) return;
    root->setStyleSheet(settingsPurpleStyleSheet());
}

}  // namespace mikai::app

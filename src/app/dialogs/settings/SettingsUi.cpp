#include "dialogs/settings/SettingsUi.hpp"

#include "theme/Appearance.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"

namespace mikai::app {

/// Builds the Slight Purple QSS stylesheet for a settings page or scroll area.
/// The stylesheet text is unchanged from the former header-inline definition;
/// only its home moved, so every includer of SettingsUi.hpp stops recompiling it.
QString settingsStylesheet(const theme::Appearance& appearance) {
    const theme::Palette p = theme::paletteFor(appearance);
    const QString sheet = QStringLiteral(R"QSS(
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
            background-color: %1;
            color: %2;
        }

        /* ── Navigation ───────────────────────────────────────────────────── */
        QWidget#settingsNav { background: transparent; }
        QLabel#settingsNavGroup {
            color: %8;
            font-size: 12px;
            font-weight: 600;
            padding: 14px 10px 4px 10px;
            background: transparent;
        }
        QPushButton#settingsNavItem {
            background: transparent;
            color: %9;
            border: 1px solid transparent;
            border-radius: %50px;
            padding: 0 %52px;
            text-align: left;
            font-weight: 500;
        }
        QPushButton#settingsNavItem:hover {
            background-color: %21;
            color: %6;
        }
        QPushButton#settingsNavItem:checked {
            background-color: %28;
            color: %6;
            font-weight: 600;
            border-color: rgba(%4, 0.16);
        }
        QPushButton#settingsNavItem:focus { border-color: %24; }

        /* ── Appearance controls ──────────────────────────────────────────── */
        QPushButton#radiusChip {
            background-color: %20;
            color: %9;
            border: 1px solid rgba(%4, 0.14);
            border-radius: %50px;
            padding: 0 %53px;
            font-weight: 500;
        }
        QPushButton#radiusChip:hover { background-color: %30; color: %6; }
        QPushButton#radiusChip:checked {
            background-color: %33;
            color: %22;
            border-color: %33;
            font-weight: 600;
        }
        QLabel#accentValueLabel, QLabel#gradientValueLabel {
            color: %9;
            font-size: 12px;
            font-weight: 600;
            background: transparent;
        }
        QLabel#appearanceValue {
            color: %6;
            font-size: 12px;
            font-weight: 600;
            background: transparent;
        }
        QSlider#appearanceSlider { background: transparent; min-height: 22px; }
        QSlider#appearanceSlider::groove:horizontal {
            height: 4px;
            background: %5;
            border-radius: 2px;
        }
        QSlider#appearanceSlider::sub-page:horizontal {
            background: %33;
            border-radius: 2px;
        }
        QSlider#appearanceSlider::handle:horizontal {
            background: %33;
            border: 2px solid %3;
            width: 12px;
            height: 12px;
            margin: -5px 0;
            border-radius: 7px;
        }
        QSlider#appearanceSlider::handle:horizontal:hover { background: %33; }
        QSlider#appearanceSlider::handle:horizontal:disabled { background: %5; border-color: %1; }
        QSlider#appearanceSlider::sub-page:horizontal:disabled { background: %26; }

        /* ── Elevated Cards & Concentric Sub-Panels ────────────────────────── */
        QFrame#settingsCard {
            background-color: %3;
            border: 1px solid rgba(%4, 0.12);
            border-radius: %54px;
        }
        QFrame#settingsCard:hover {
            border-color: rgba(%4, 0.20);
        }
        QFrame#settingsSubCard {
            background-color: %5;
            border: 1px solid rgba(%4, 0.10);
            border-radius: %54px;
        }
        QFrame#settingsPageHeader,
        QFrame#benchmarkHeader {
            background-color: %3;
            border: 1px solid rgba(%4, 0.12);
            border-radius: %54px;
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
            border-radius: %54px;
            padding: 10px 12px;
            font-size: 12px;
        }
        QLabel#status {
            background-color: rgba(%10, 0.08);
            color: %11;
            border: 1px solid rgba(%10, 0.22);
            border-radius: %54px;
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
            border-radius: %55px;
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
            border-radius: %50px;
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
        QComboBox QAbstractItemView {
            background-color: %28;
            color: %2;
            border: 1px solid rgba(%10, 0.28);
            border-radius: %50px;
            padding: 4px;
            selection-background-color: rgba(%19, 0.28);
            selection-color: %22;
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
            background-color: %45;
            color: %7;
            border: 1px solid %46;
            border-radius: %50px;
            min-height: %51px;
            max-height: %51px;
            padding: 0 %52px;
            font-weight: 500;
            text-align: center;
        }
        QPushButton:hover {
            background-color: rgba(%10, 0.12);
            border-color: rgba(%10, 0.38);
            color: %22;
        }
        QPushButton:pressed {
            background-color: %31;
            border-color: %24;
        }
        QPushButton:checked {
            background-color: rgba(%10, 0.14);
            border-color: %33;
            color: %22;
        }
        QPushButton:focus {
            border-color: %24;
        }
        QPushButton:disabled {
            background-color: %20;
            color: rgba(%36, 0.55);
            border-color: %46;
        }
        QPushButton[actionRole="primary"] {
            background-color: %33;
            color: %47;
            border: 1px solid %33;
            border-radius: %50px;
            padding: 0 %53px;
            font-weight: 600;
        }
        QPushButton[actionRole="primary"]:hover {
            background-color: %48;
            border-color: %48;
        }
        QPushButton[actionRole="primary"]:pressed {
            background-color: %33;
            border-color: %34;
        }
        QPushButton[actionRole="primary"]:checked {
            background-color: %48;
            border-color: %47;
        }
        QPushButton[actionRole="primary"]:focus {
            border-color: %47;
        }
        QPushButton[actionRole="primary"]:disabled {
            background-color: %20;
            color: rgba(%36, 0.55);
            border-color: %46;
        }
        QPushButton[actionRole="ghost"] {
            background-color: transparent;
            color: %33;
            border: 1px solid rgba(%10, 0.22);
            border-radius: %50px;
            min-height: %51px;
            max-height: %51px;
            padding: 0 %52px;
            font-size: 12px;
            font-weight: 500;
        }
        QPushButton[actionRole="ghost"]:hover {
            background-color: rgba(%10, 0.16);
            border-color: rgba(%10, 0.45);
            color: %48;
        }
        QPushButton[actionRole="ghost"]:pressed {
            background-color: rgba(%10, 0.20);
        }
        QPushButton[actionRole="ghost"]:checked {
            background-color: rgba(%10, 0.18);
            border-color: %33;
            color: %48;
        }
        QPushButton[actionRole="ghost"]:focus {
            border-color: %33;
        }
        QPushButton[actionRole="ghost"]:disabled {
            color: rgba(%36, 0.55);
            border-color: %46;
        }
        QPushButton[actionRole="danger"] {
            background-color: transparent;
            color: %44;
            border: 1px solid %44;
            font-weight: 600;
        }
        QPushButton[actionRole="danger"]:hover {
            background-color: rgba(%16, 0.14);
        }
        QPushButton[actionRole="danger"]:pressed {
            background-color: rgba(%16, 0.22);
        }
        QPushButton[actionRole="danger"]:checked {
            background-color: rgba(%16, 0.18);
            border-color: %44;
        }
        QPushButton[actionRole="danger"]:focus {
            background-color: rgba(%16, 0.10);
            border-color: %44;
        }
        QPushButton[actionRole="danger"]:disabled {
            color: rgba(%36, 0.55);
            border-color: %46;
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
            min-width: %49px;
            max-width: %49px;
            min-height: %49px;
            max-height: %49px;
            border-radius: 0;
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
            border-radius: %50px;
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
            border-radius: %54px;
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
            border-radius: %54px;
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
            border-radius: %54px;
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
            border-radius: %54px;
            padding: 20px;
        }
        QLabel#benchmarkResultDetail {
            background-color: rgba(%16, 0.10);
            color: %17;
            border: 1px solid rgba(%16, 0.28);
            border-radius: %50px;
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
        .arg(p.surfacePage)
        .arg(p.textPrimary)
        .arg(p.surfaceRaised)
        .arg(p.hairlineRgb)
        .arg(p.surfaceInset)
        .arg(p.textStrong)
        .arg(p.textBright)
        .arg(p.textMutedSoft)
        .arg(p.textLabel)
        .arg(p.accentRgb)
        .arg(p.accentSoftBright)
        .arg(p.successRgb)
        .arg(p.successText)
        .arg(p.warningRgb)
        .arg(p.warningText)
        .arg(p.dangerRgb)
        .arg(p.dangerText)
        .arg(p.accentSoft)
        .arg(p.accentSolidRgb)
        .arg(p.surfaceInput)
        .arg(p.accentDeep)
        .arg(p.textOnAccent)
        .arg(p.surfaceInputHover)
        .arg(p.accentFocus)
        .arg(p.surfaceInputFocus)
        .arg(p.textDisabled)
        .arg(p.surfaceDeepRgb)
        .arg(p.surfaceRow)
        .arg(p.surfaceControl)
        .arg(p.surfaceControlHover)
        .arg(p.surfaceControlPressed)
        /* Three rules were deleted with the old button styles. Their arguments
           went with them here: leaving the values in place shifted every later
           marker by three, so the role rules painted a table-row colour as a
           control border and a success fill as a primary label ink. */
        .arg(p.accentSolid)
        .arg(p.accentDeepest)
        .arg(p.textPrimaryRgb)
        .arg(p.textCheckLabel)
        .arg(p.surfaceTag)
        .arg(p.surfaceTileHover)
        .arg(p.surfaceShortcutRow)
        .arg(p.tableAlternateRow)
        .arg(p.successFill)
        .arg(p.dangerFill)
        .arg(p.surfaceControlRaised)
        .arg(p.borderControl)
        .arg(p.accentInk)
        .arg(p.accentHover)
        .arg(theme::kSettingsSwatchSize)
        .arg(theme::radiusSmall(appearance))   // %50
        .arg(theme::kControlContentHeight)     // %51
        .arg(theme::kControlInsetH)            // %52
        .arg(theme::kControlInsetHPrimary)     // %53
        .arg(theme::radiusMedium(appearance))  // %54
        .arg(theme::radiusPill(appearance));   // %55
    // The page's control and card corners are the appearance's own radius
    // steps, not a second scale computed here.
    return sheet;
}

}  // namespace mikai::app

#include "dialogs/settings/SettingsUi.hpp"

#include "theme/Appearance.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"

#include <utility>

namespace mikai::app {

/// Builds the Slight Purple QSS stylesheet for a settings page or scroll area.
/// The stylesheet text is unchanged from the former header-inline definition;
/// only its home moved, so every includer of SettingsUi.hpp stops recompiling it.
/// Placeholders are named markers (%{surfacePage}, %{radiusSmall}, ...) filled
/// from the (marker, value) list below with QString::replace, so adding,
/// removing, or reordering a value can no longer silently shift later markers.
QString settingsStylesheet(const theme::Appearance& appearance) {
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
            background-color: %{surfacePage};
            color: %{textPrimary};
        }

        /* ── Navigation ───────────────────────────────────────────────────── */
        QWidget#settingsNav { background: transparent; }
        QLabel#settingsNavGroup {
            color: %{textMutedSoft};
            font-size: 12px;
            font-weight: 600;
            padding: 14px 10px 4px 10px;
            background: transparent;
        }
        QPushButton#settingsNavItem {
            background: transparent;
            color: %{textLabel};
            border: 1px solid transparent;
            border-radius: %{radiusSmall}px;
            padding: 0 %{controlInsetH}px;
            text-align: left;
            font-weight: 500;
        }
        QPushButton#settingsNavItem:hover {
            background-color: %{accentDeep};
            color: %{textStrong};
        }
        QPushButton#settingsNavItem:checked {
            background-color: %{surfaceRow};
            color: %{textStrong};
            font-weight: 600;
            border-color: rgba(%{hairlineRgb}, 0.16);
        }
        QPushButton#settingsNavItem:focus { border-color: %{accentFocus}; }

        /* ── Appearance controls ──────────────────────────────────────────── */
        QPushButton#radiusChip {
            background-color: %{surfaceInput};
            color: %{textLabel};
            border: 1px solid rgba(%{hairlineRgb}, 0.14);
            border-radius: %{radiusSmall}px;
            padding: 0 %{controlInsetHPrimary}px;
            font-weight: 500;
        }
        QPushButton#radiusChip:hover { background-color: %{surfaceControlHover}; color: %{textStrong}; }
        QPushButton#radiusChip:checked {
            background-color: %{accentSolid};
            color: %{textOnAccent};
            border-color: %{accentSolid};
            font-weight: 600;
        }
        QLabel#accentValueLabel, QLabel#gradientValueLabel {
            color: %{textLabel};
            font-size: 12px;
            font-weight: 600;
            background: transparent;
        }
        QLabel#appearanceValue {
            color: %{textStrong};
            font-size: 12px;
            font-weight: 600;
            background: transparent;
        }
        QSlider#appearanceSlider { background: transparent; min-height: 22px; }
        QSlider#appearanceSlider::groove:horizontal {
            height: 4px;
            background: %{surfaceInset};
            border-radius: 2px;
        }
        QSlider#appearanceSlider::sub-page:horizontal {
            background: %{accentSolid};
            border-radius: 2px;
        }
        QSlider#appearanceSlider::handle:horizontal {
            background: %{accentSolid};
            border: 2px solid %{surfaceRaised};
            width: 12px;
            height: 12px;
            margin: -5px 0;
            border-radius: 7px;
        }
        QSlider#appearanceSlider::handle:horizontal:hover { background: %{accentSolid}; }
        QSlider#appearanceSlider::handle:horizontal:disabled { background: %{surfaceInset}; border-color: %{surfacePage}; }
        QSlider#appearanceSlider::sub-page:horizontal:disabled { background: %{textDisabled}; }

        /* ── Elevated Cards & Concentric Sub-Panels ────────────────────────── */
        QFrame#settingsCard {
            background-color: %{surfaceRaised};
            border: 1px solid rgba(%{hairlineRgb}, 0.12);
            border-radius: %{radiusMedium}px;
        }
        QFrame#settingsCard:hover {
            border-color: rgba(%{hairlineRgb}, 0.20);
        }
        QFrame#settingsSubCard {
            background-color: %{surfaceInset};
            border: 1px solid rgba(%{hairlineRgb}, 0.10);
            border-radius: %{radiusMedium}px;
        }
        QFrame#settingsPageHeader,
        QFrame#benchmarkHeader {
            background-color: %{surfaceRaised};
            border: 1px solid rgba(%{hairlineRgb}, 0.12);
            border-radius: %{radiusMedium}px;
        }
        QFrame#settingsDivider {
            background-color: rgba(%{hairlineRgb}, 0.10);
            border: none;
            max-height: 1px;
        }

        /* ── Typography Hierarchy ──────────────────────────────────────────── */
        QLabel#heading {
            color: %{textStrong};
            font-size: 17px;
            font-weight: 600;
            background: transparent;
        }
        QLabel#sectionHeading {
            color: %{textBright};
            font-size: 14px;
            font-weight: 600;
            background: transparent;
        }
        QLabel#muted,
        QLabel[textRole="muted"] {
            color: %{textMutedSoft};
            font-size: 12px;
            background: transparent;
        }
        QLabel#SettingsFormLabel {
            color: %{textLabel};
            font-size: 13px;
            font-weight: 500;
            background: transparent;
        }

        /* ── Telemetry, Description & Status Callouts ──────────────────────── */
        QLabel#descriptionPanel {
            background-color: %{surfaceInset};
            color: %{textLabel};
            border: 1px solid rgba(%{hairlineRgb}, 0.12);
            border-radius: %{radiusMedium}px;
            padding: 10px 12px;
            font-size: 12px;
        }
        QLabel#status {
            background-color: rgba(%{accentRgb}, 0.08);
            color: %{accentSoftBright};
            border: 1px solid rgba(%{accentRgb}, 0.22);
            border-radius: %{radiusMedium}px;
            padding: 8px 12px;
            font-size: 12px;
        }
        QLabel#status[statusTone="success"] {
            background-color: rgba(%{successRgb}, 0.10);
            color: %{successText};
            border: 1px solid rgba(%{successRgb}, 0.28);
        }
        QLabel#status[statusTone="warning"] {
            background-color: rgba(%{warningRgb}, 0.10);
            color: %{warningText};
            border: 1px solid rgba(%{warningRgb}, 0.28);
        }
        QLabel#status[statusTone="danger"] {
            background-color: rgba(%{dangerRgb}, 0.12);
            color: %{dangerText};
            border: 1px solid rgba(%{dangerRgb}, 0.32);
        }
        QLabel#status[statusTone="info"] {
            background-color: rgba(%{accentRgb}, 0.12);
            color: %{accentSoftBright};
            border: 1px solid rgba(%{accentRgb}, 0.30);
        }

        /* ── Pill Counters & Status Badges ─────────────────────────────────── */
        QLabel#benchmarkClipCount,
        QLabel#benchmarkModelCount,
        QLabel#benchmarkRunStatus,
        QLabel[badgeTone="purple"] {
            background-color: rgba(%{accentRgb}, 0.12);
            color: %{accentSoft};
            border: 1px solid rgba(%{accentRgb}, 0.24);
            border-radius: %{radiusPill}px;
            padding: 3px 9px;
            font-size: 11px;
            font-weight: 600;
        }
        QLabel#benchmarkRunStatus[statusTone="success"] {
            background-color: rgba(%{successRgb}, 0.12);
            color: %{successText};
            border-color: rgba(%{successRgb}, 0.30);
        }
        QLabel#benchmarkRunStatus[statusTone="warning"],
        QLabel#benchmarkRunStatus[statusTone="danger"] {
            background-color: rgba(%{dangerRgb}, 0.12);
            color: %{dangerText};
            border-color: rgba(%{dangerRgb}, 0.32);
        }
        QLabel#benchmarkRunStatus[statusTone="running"] {
            background-color: rgba(%{accentSolidRgb}, 0.20);
            color: %{textBright};
            border-color: rgba(%{accentRgb}, 0.45);
        }

        /* ── Inputs, Search Fields, Combos & KeySequence Editors ───────────── */
        QLineEdit,
        QComboBox,
        QKeySequenceEdit > QLineEdit {
            background-color: %{surfaceInput};
            color: %{textPrimary};
            border: 1px solid rgba(%{hairlineRgb}, 0.16);
            border-radius: %{radiusSmall}px;
            padding: 5px 11px;
            selection-background-color: %{accentDeep};
            selection-color: %{textOnAccent};
        }
        QLineEdit:hover,
        QComboBox:hover,
        QKeySequenceEdit > QLineEdit:hover {
            border-color: rgba(%{accentRgb}, 0.36);
            background-color: %{surfaceInputHover};
        }
        QLineEdit:focus,
        QComboBox:focus,
        QComboBox:on,
        QKeySequenceEdit > QLineEdit:focus {
            border: 1px solid %{accentFocus};
            background-color: %{surfaceInputFocus};
        }
        QLineEdit:disabled,
        QComboBox:disabled,
        QKeySequenceEdit > QLineEdit:disabled {
            color: %{textDisabled};
            background-color: rgba(%{surfaceDeepRgb}, 0.55);
            border-color: rgba(%{hairlineRgb}, 0.07);
        }
        QComboBox QAbstractItemView {
            background-color: %{surfaceRow};
            color: %{textPrimary};
            border: 1px solid rgba(%{accentRgb}, 0.28);
            border-radius: %{radiusSmall}px;
            padding: 4px;
            selection-background-color: rgba(%{accentSolidRgb}, 0.28);
            selection-color: %{textOnAccent};
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
            background-color: %{surfaceControlRaised};
            color: %{textBright};
            border: 1px solid %{borderControl};
            border-radius: %{radiusSmall}px;
            min-height: %{controlContentHeight}px;
            max-height: %{controlContentHeight}px;
            padding: 0 %{controlInsetH}px;
            font-weight: 500;
            text-align: center;
        }
        QPushButton:hover {
            background-color: rgba(%{accentRgb}, 0.12);
            border-color: rgba(%{accentRgb}, 0.38);
            color: %{textOnAccent};
        }
        QPushButton:pressed {
            background-color: %{surfaceControlPressed};
            border-color: %{accentFocus};
        }
        QPushButton:checked {
            background-color: rgba(%{accentRgb}, 0.14);
            border-color: %{accentSolid};
            color: %{textOnAccent};
        }
        QPushButton:focus {
            border-color: %{accentFocus};
        }
        QPushButton:disabled {
            background-color: %{surfaceInput};
            color: rgba(%{textPrimaryRgb}, 0.55);
            border-color: %{borderControl};
        }
        QPushButton[actionRole="primary"] {
            background-color: %{accentSolid};
            color: %{accentInk};
            border: 1px solid %{accentSolid};
            border-radius: %{radiusSmall}px;
            padding: 0 %{controlInsetHPrimary}px;
            font-weight: 600;
        }
        QPushButton[actionRole="primary"]:hover {
            background-color: %{accentHover};
            border-color: %{accentHover};
        }
        QPushButton[actionRole="primary"]:pressed {
            background-color: %{accentSolid};
            border-color: %{accentDeepest};
        }
        QPushButton[actionRole="primary"]:checked {
            background-color: %{accentHover};
            border-color: %{accentInk};
        }
        QPushButton[actionRole="primary"]:focus {
            border-color: %{accentInk};
        }
        QPushButton[actionRole="primary"]:disabled {
            background-color: %{surfaceInput};
            color: rgba(%{textPrimaryRgb}, 0.55);
            border-color: %{borderControl};
        }
        QPushButton[actionRole="ghost"] {
            background-color: transparent;
            color: %{accentSolid};
            border: 1px solid rgba(%{accentRgb}, 0.22);
            border-radius: %{radiusSmall}px;
            min-height: %{controlContentHeight}px;
            max-height: %{controlContentHeight}px;
            padding: 0 %{controlInsetH}px;
            font-size: 12px;
            font-weight: 500;
        }
        QPushButton[actionRole="ghost"]:hover {
            background-color: rgba(%{accentRgb}, 0.16);
            border-color: rgba(%{accentRgb}, 0.45);
            color: %{accentHover};
        }
        QPushButton[actionRole="ghost"]:pressed {
            background-color: rgba(%{accentRgb}, 0.20);
        }
        QPushButton[actionRole="ghost"]:checked {
            background-color: rgba(%{accentRgb}, 0.18);
            border-color: %{accentSolid};
            color: %{accentHover};
        }
        QPushButton[actionRole="ghost"]:focus {
            border-color: %{accentSolid};
        }
        QPushButton[actionRole="ghost"]:disabled {
            color: rgba(%{textPrimaryRgb}, 0.55);
            border-color: %{borderControl};
        }
        QPushButton[actionRole="danger"] {
            background-color: transparent;
            color: %{dangerFill};
            border: 1px solid %{dangerFill};
            font-weight: 600;
        }
        QPushButton[actionRole="danger"]:hover {
            background-color: rgba(%{dangerRgb}, 0.14);
        }
        QPushButton[actionRole="danger"]:pressed {
            background-color: rgba(%{dangerRgb}, 0.22);
        }
        QPushButton[actionRole="danger"]:checked {
            background-color: rgba(%{dangerRgb}, 0.18);
            border-color: %{dangerFill};
        }
        QPushButton[actionRole="danger"]:focus {
            background-color: rgba(%{dangerRgb}, 0.10);
            border-color: %{dangerFill};
        }
        QPushButton[actionRole="danger"]:disabled {
            color: rgba(%{textPrimaryRgb}, 0.55);
            border-color: %{borderControl};
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
            min-width: %{swatchSize}px;
            max-width: %{swatchSize}px;
            min-height: %{swatchSize}px;
            max-height: %{swatchSize}px;
            border-radius: 0;
        }

        /* ── Checkboxes & Selectable Resolution / Feed Tiles ───────────────── */
        QCheckBox {
            color: %{textCheckLabel};
            spacing: 8px;
            padding: 5px 8px;
            border-radius: 8px;
        }
        QCheckBox:hover {
            background-color: rgba(%{accentRgb}, 0.06);
        }
        QCheckBox#benchmarkClipCheck,
        QCheckBox[tileCheck="true"] {
            background-color: %{surfaceTag};
            border: 1px solid rgba(%{hairlineRgb}, 0.12);
            border-radius: %{radiusSmall}px;
            padding: 6px 10px;
            font-weight: 500;
        }
        QCheckBox#benchmarkClipCheck:hover,
        QCheckBox[tileCheck="true"]:hover {
            background-color: %{surfaceTileHover};
            border-color: rgba(%{accentRgb}, 0.32);
        }
        QCheckBox#benchmarkClipCheck:checked,
        QCheckBox[tileCheck="true"]:checked {
            background-color: rgba(%{accentSolidRgb}, 0.14);
            border-color: rgba(%{accentRgb}, 0.42);
            color: %{textStrong};
        }
        QCheckBox::indicator {
            width: 16px;
            height: 16px;
            border-radius: 4px;
            border: 1px solid rgba(%{hairlineRgb}, 0.30);
            background-color: %{surfaceInput};
        }
        QCheckBox::indicator:hover {
            border-color: %{accentFocus};
        }
        QCheckBox::indicator:checked {
            background-color: %{accentSolid};
            border-color: %{accentFocus};
        }

        /* ── Preset Shortcut Rows ──────────────────────────────────────────── */
        QWidget#presetShortcutRow {
            background-color: %{surfaceShortcutRow};
            border: 1px solid rgba(%{hairlineRgb}, 0.09);
            border-radius: %{radiusMedium}px;
        }
        QWidget#presetShortcutRow:hover {
            background-color: %{surfaceControl};
            border-color: rgba(%{accentRgb}, 0.26);
        }
        QWidget#presetShortcutRow[hasBinding="true"] {
            background-color: rgba(%{accentSolidRgb}, 0.10);
            border: 1px solid rgba(%{accentRgb}, 0.34);
        }
        QWidget#presetShortcutRow[hasBinding="true"]:hover {
            background-color: rgba(%{accentSolidRgb}, 0.15);
            border-color: rgba(%{accentRgb}, 0.48);
        }

        /* ── Benchmark Model Tree & Results Table ──────────────────────────── */
        QTreeWidget#benchmarkModelTree {
            background-color: %{surfaceInput};
            color: %{textPrimary};
            border: 1px solid rgba(%{hairlineRgb}, 0.14);
            border-radius: %{radiusMedium}px;
            padding: 6px;
            outline: 0;
        }
        QTreeWidget#benchmarkModelTree::item {
            padding: 5px 6px;
            border-radius: 6px;
        }
        QTreeWidget#benchmarkModelTree::item:hover {
            background-color: rgba(%{accentRgb}, 0.10);
        }
        QTableWidget#benchmarkResults {
            background-color: %{surfaceInput};
            alternate-background-color: %{tableAlternateRow};
            color: %{textPrimary};
            border: 1px solid rgba(%{hairlineRgb}, 0.14);
            border-radius: %{radiusMedium}px;
            selection-background-color: rgba(%{accentSolidRgb}, 0.24);
            selection-color: %{textOnAccent};
            outline: 0;
        }
        QTableWidget#benchmarkResults QHeaderView::section {
            background-color: %{surfaceTag};
            color: %{accentSoft};
            font-weight: 600;
            font-size: 12px;
            padding: 6px 10px;
            border: none;
            border-bottom: 1px solid rgba(%{hairlineRgb}, 0.14);
        }
        QLabel#benchmarkEmptyState {
            background-color: %{surfaceInput};
            color: %{textMutedSoft};
            border: 1px dashed rgba(%{hairlineRgb}, 0.18);
            border-radius: %{radiusMedium}px;
            padding: 20px;
        }
        QLabel#benchmarkResultDetail {
            background-color: rgba(%{dangerRgb}, 0.10);
            color: %{dangerText};
            border: 1px solid rgba(%{dangerRgb}, 0.28);
            border-radius: %{radiusSmall}px;
            padding: 8px 12px;
        }

        /* ── Progress Bars (Slight Purple Telemetry) ───────────────────────── */
        QProgressBar#benchmarkProgress,
        QProgressBar#benchmarkEngineProgress {
            background-color: %{surfaceInput};
            border: 1px solid rgba(%{hairlineRgb}, 0.16);
            border-radius: 5px;
            min-height: 8px;
            max-height: 8px;
        }
        QProgressBar#benchmarkProgress::chunk,
        QProgressBar#benchmarkEngineProgress::chunk {
            background-color: %{accentSolid};
            border-radius: 4px;
        }
        QProgressBar#benchmarkProgress[runState="success"]::chunk {
            background-color: %{successFill};
        }
        QProgressBar#benchmarkProgress[runState="failure"]::chunk {
            background-color: %{dangerFill};
        }

        /* ── Benchmark Sticky Footer Bar ───────────────────────────────────── */
        QWidget#benchmarkBottomBar {
            background-color: %{surfaceRaised};
            border-top: 1px solid rgba(%{hairlineRgb}, 0.14);
        }

        /* ── Minimal Sleek Scrollbars ──────────────────────────────────────── */
        QScrollBar:vertical {
            background: transparent;
            width: 8px;
            margin: 2px 1px 2px 1px;
        }
        QScrollBar::handle:vertical {
            background: rgba(%{accentRgb}, 0.24);
            min-height: 28px;
            border-radius: 3px;
        }
        QScrollBar::handle:vertical:hover {
            background: rgba(%{accentRgb}, 0.45);
        }
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {
            background: none;
            height: 0px;
        }
    )QSS";
    // Named markers, filled from the (marker, value) list below: adding,
    // removing, or reordering a value can no longer silently shift every
    // later marker, the failure the deleted button rules caused with the
    // old positional .arg() chain. Placeholders %32, %35, and %37 are
    // unused for exactly that reason: their rules were deleted with the
    // old button styles, and their arguments went with them.
    const std::pair<QString, QString> markers[] = {
        {QStringLiteral("%{surfacePage}"), p.surfacePage},
        {QStringLiteral("%{textPrimary}"), p.textPrimary},
        {QStringLiteral("%{surfaceRaised}"), p.surfaceRaised},
        {QStringLiteral("%{hairlineRgb}"), p.hairlineRgb},
        {QStringLiteral("%{surfaceInset}"), p.surfaceInset},
        {QStringLiteral("%{textStrong}"), p.textStrong},
        {QStringLiteral("%{textBright}"), p.textBright},
        {QStringLiteral("%{textMutedSoft}"), p.textMutedSoft},
        {QStringLiteral("%{textLabel}"), p.textLabel},
        {QStringLiteral("%{accentRgb}"), p.accentRgb},
        {QStringLiteral("%{accentSoftBright}"), p.accentSoftBright},
        {QStringLiteral("%{successRgb}"), p.successRgb},
        {QStringLiteral("%{successText}"), p.successText},
        {QStringLiteral("%{warningRgb}"), p.warningRgb},
        {QStringLiteral("%{warningText}"), p.warningText},
        {QStringLiteral("%{dangerRgb}"), p.dangerRgb},
        {QStringLiteral("%{dangerText}"), p.dangerText},
        {QStringLiteral("%{accentSoft}"), p.accentSoft},
        {QStringLiteral("%{accentSolidRgb}"), p.accentSolidRgb},
        {QStringLiteral("%{surfaceInput}"), p.surfaceInput},
        {QStringLiteral("%{accentDeep}"), p.accentDeep},
        {QStringLiteral("%{textOnAccent}"), p.textOnAccent},
        {QStringLiteral("%{surfaceInputHover}"), p.surfaceInputHover},
        {QStringLiteral("%{accentFocus}"), p.accentFocus},
        {QStringLiteral("%{surfaceInputFocus}"), p.surfaceInputFocus},
        {QStringLiteral("%{textDisabled}"), p.textDisabled},
        {QStringLiteral("%{surfaceDeepRgb}"), p.surfaceDeepRgb},
        {QStringLiteral("%{surfaceRow}"), p.surfaceRow},
        {QStringLiteral("%{surfaceControl}"), p.surfaceControl},
        {QStringLiteral("%{surfaceControlHover}"), p.surfaceControlHover},
        {QStringLiteral("%{surfaceControlPressed}"), p.surfaceControlPressed},
        {QStringLiteral("%{accentSolid}"), p.accentSolid},
        {QStringLiteral("%{accentDeepest}"), p.accentDeepest},
        {QStringLiteral("%{textPrimaryRgb}"), p.textPrimaryRgb},
        {QStringLiteral("%{textCheckLabel}"), p.textCheckLabel},
        {QStringLiteral("%{surfaceTag}"), p.surfaceTag},
        {QStringLiteral("%{surfaceTileHover}"), p.surfaceTileHover},
        {QStringLiteral("%{surfaceShortcutRow}"), p.surfaceShortcutRow},
        {QStringLiteral("%{tableAlternateRow}"), p.tableAlternateRow},
        {QStringLiteral("%{successFill}"), p.successFill},
        {QStringLiteral("%{dangerFill}"), p.dangerFill},
        {QStringLiteral("%{surfaceControlRaised}"), p.surfaceControlRaised},
        {QStringLiteral("%{borderControl}"), p.borderControl},
        {QStringLiteral("%{accentInk}"), p.accentInk},
        {QStringLiteral("%{accentHover}"), p.accentHover},
        {QStringLiteral("%{swatchSize}"), QString::number(theme::kSettingsSwatchSize)},
        {QStringLiteral("%{radiusSmall}"), QString::number(theme::radiusSmall(appearance))},
        {QStringLiteral("%{controlContentHeight}"), QString::number(theme::kControlContentHeight)},
        {QStringLiteral("%{controlInsetH}"), QString::number(theme::kControlInsetH)},
        {QStringLiteral("%{controlInsetHPrimary}"), QString::number(theme::kControlInsetHPrimary)},
        {QStringLiteral("%{radiusMedium}"), QString::number(theme::radiusMedium(appearance))},
        {QStringLiteral("%{radiusPill}"), QString::number(theme::radiusPill(appearance))},
    };
    for (const auto& [marker, value] : markers) {
        sheet.replace(marker, value);
    }
    // The page's control and card corners are the appearance's own radius
    // steps, not a second scale computed here.
    return sheet;
}

}  // namespace mikai::app

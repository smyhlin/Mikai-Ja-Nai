#include "theme/Theme.hpp"

#include "theme/Appearance.hpp"
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QPalette>
#include <QSizePolicy>
#include <QStyle>
#include <QVector>

#include <utility>

/// The arrow asset lives in the app's resource bundle; a static library keeps
/// that object only when something references its initialiser. Q_INIT_RESOURCE
/// declares its symbol in the enclosing namespace, so this must stay at global
/// scope - not in an anonymous one.
void mikaiInitAssetResources() {
    static const bool loaded = [] {
        Q_INIT_RESOURCE(mikai_assets);
        return true;
    }();
    Q_UNUSED(loaded);
}

namespace mikai::app::theme {
namespace {

// T-302: %24 carries the chosen accent's RGB triplet for the translucent
// accent washes below, mirroring the %9 mechanism titlePage() uses; the washes
// must follow the user's accent like the solid accent %11 does instead of
// staying on the default purple.
QString base() {
    return QStringLiteral(R"QSS(
QWidget {
    background: transparent;
    color: %2;
    font-family: "Segoe UI", system-ui, sans-serif;
    font-size: 14px;
}
QMainWindow, QDialog, QWidget#mainWindow {
    background: %1;
}
QLabel, QCheckBox, QRadioButton {
    background: transparent;
}
QLabel#heading { font-size: 20px; font-weight: 700; background: transparent; }
QLabel#sectionHeading, QLabel#pageHeading { font-size: 15px; font-weight: 600; padding-top: %15px; background: transparent; }
QLabel#muted, QLabel#status, QLabel#benchmarkClipCount, QLabel#benchmarkModelCount {
    color: %4; background: transparent;
}
QLabel#failureCode { color: %4; background: transparent; border: 1px solid %10; border-radius: %6px;
    padding: 1px %3px; }
QLabel#descriptionPanel { background: transparent; color: %2; border: none;
    padding: 2px 0; }
QFrame#settingsCard, QWidget#settingsCard {
    background: %5;
    border: 1px solid %10;
    border-radius: %17px;
}
QFrame#settingsCard QLabel, QWidget#settingsCard QLabel { background: transparent; }
QFrame#settingsCard QCheckBox, QWidget#settingsCard QCheckBox { background: transparent; }
QFrame#settingsCard QLabel#descriptionPanel, QWidget#settingsCard QLabel#descriptionPanel {
    background: transparent;
    border: none;
    padding: 2px 0;
}
QFrame#settingsCard QLineEdit, QFrame#settingsCard QComboBox, QFrame#settingsCard QSpinBox,
QWidget#settingsCard QLineEdit, QWidget#settingsCard QComboBox, QWidget#settingsCard QSpinBox {
    background: %8;
    border: 1px solid %10;
}
QFrame#settingsCard QLineEdit:focus, QFrame#settingsCard QComboBox:focus, QFrame#settingsCard QSpinBox:focus,
QWidget#settingsCard QLineEdit:focus, QWidget#settingsCard QComboBox:focus, QWidget#settingsCard QSpinBox:focus {
    background: %9;
    border-color: %11;
}
QFrame#settingsCard QPushButton, QWidget#settingsCard QPushButton {
    background: %25;
    border: 1px solid %26;
}
QFrame#settingsCard QPushButton:hover, QWidget#settingsCard QPushButton:hover {
    background: rgba(%24, 0.12);
    border-color: rgba(%24, 0.55);
}
QFrame#settingsCard QPushButton:disabled, QWidget#settingsCard QPushButton:disabled {
    color: rgba(%23, 0.55);
    background: %5;
    border-color: %26;
}
QFrame#settingsCard QPushButton[actionRole="primary"],
QWidget#settingsCard QPushButton[actionRole="primary"] {
    background: %11;
    color: %27;
    border: 1px solid %11;
}
QFrame#settingsCard QPushButton[actionRole="primary"]:hover,
QWidget#settingsCard QPushButton[actionRole="primary"]:hover {
    background: %12;
    border-color: %12;
}
QWidget#presetShortcutRow {
    background: %5;
    border: 1px solid %10;
    border-radius: %6px;
}
QWidget#presetShortcutRow:hover {
    background: %8;
    border-color: rgba(%24, 0.35);
}
QWidget#presetShortcutRow QLabel { background: transparent; }
QWidget#settingsFooter {
    background: %1;
    border-top: 1px solid %10;
}
QGroupBox#nativeComponentPackCard {
    background: %5;
    color: %2;
    border: 1px solid %10;
    border-radius: %17px;
    margin-top: 0;
    padding: 0;
}
QGroupBox#nativeComponentPackCard:hover {
    background: %8;
    border-color: rgba(%24, 0.40);
}
QGroupBox#nativeComponentPackCard[recommended="true"] {
    border-color: rgba(%24, 0.55);
}
QGroupBox#nativeComponentPackCard[selected="true"] {
    border-color: %11;
    background: rgba(%24, 0.08);
}
QGroupBox#nativeComponentPackCard::title { subcontrol-origin: margin;
    left: %7px; padding: 0 %15px; color: %2; font-weight: 600; }
QGroupBox#nativeComponentPackCard QLabel { background: transparent; }
QGroupBox#nativeComponentPackCard QCheckBox { background: transparent; }
QLabel#componentRecommendationBanner {
    background: rgba(%24, 0.10);
    color: %2;
    border: 1px solid rgba(%24, 0.38);
    border-radius: %6px;
    padding: %3px %7px;
}
QLabel[textRole="muted"], QGroupBox#nativeComponentPackCard QLabel[textRole="muted"] {
    color: %4;
    background: transparent;
}
QLabel[textRole="recommendation"], QGroupBox#nativeComponentPackCard QLabel[textRole="recommendation"] {
    color: %11;
    background: rgba(%24, 0.14);
    border: 1px solid rgba(%24, 0.35);
    border-radius: %21px;
    min-height: %30px;
    max-height: %30px;
    padding: 2px %3px;
    font-size: 12px;
    font-weight: 600;
}
QLabel[badgeRole="installed"], QGroupBox#nativeComponentPackCard QLabel[badgeRole="installed"] {
    color: %11;
    background: rgba(%24, 0.12);
    border: 1px solid rgba(%24, 0.30);
    border-radius: %21px;
    min-height: %30px;
    max-height: %30px;
    padding: 2px %3px;
    font-size: 12px;
    font-weight: 600;
}
QLabel[badgeRole="optional"], QGroupBox#nativeComponentPackCard QLabel[badgeRole="optional"] {
    color: %4;
    background: rgba(%23, 0.05);
    border: 1px solid %10;
    border-radius: %21px;
    min-height: %30px;
    max-height: %30px;
    padding: 2px %3px;
    font-size: 12px;
}
QLabel#nativeComponentStatus[statusSeverity="failure"] { color: %22; font-weight: 600; }
QLabel#nativeComponentStatus[statusSeverity="success"] { color: %11; font-weight: 600; }
QLabel#nativeComponentStatus[statusSeverity="cancelled"] { color: %4; }
QGroupBox#nativeComponentPackCard QProgressBar {
    background: %1; border: 1px solid %10;
    border-radius: %6px; min-height: 8px; max-height: 8px;
}
QGroupBox#nativeComponentPackCard QProgressBar::chunk {
    background: %11; border-radius: %6px;
}
QLineEdit, QComboBox, QSpinBox {
    background: %5; border: 1px solid %10;
    border-radius: %6px; padding: 6px %7px;
    min-height: 24px; font-size: 14px;
    selection-background-color: %11; selection-color: %1;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: %11; background: %8; }
/* Keep the drop-down native; styling it suppresses the style's arrow. */
QComboBox QAbstractItemView { background: %5; border: 1px solid %10;
    selection-background-color: %8; }
QCheckBox { spacing: %3px; }
/* ── Action roles ────────────────────────────────────────────────────────────
   The four roles declared in Theme.hpp are the only button hierarchy. The bare
   rule is the secondary role: Qt builds standard buttons itself (a
   QDialogButtonBox, a QMessageBox), and a neutral action is what an unlabelled
   standard button is. Its boundary carries the control - the raised fill is
   only 1.28:1 against the bed, while kBorderControl is 3.32:1 on the
   application bed, the settings page bed and a settings card alike.
   Hover moves toward the accent, which is the "підсвітити" the neutral chrome
   was missing; a disabled control keeps that same visible boundary instead of
   dissolving into the page. */
QPushButton {
    background: %25; border: 1px solid %26; border-radius: %6px;
    padding: %3px %7px; min-height: 22px;
}
QPushButton:hover { background: rgba(%24, 0.12); border-color: rgba(%24, 0.55); }
QPushButton:pressed { background: %9; }
QPushButton:checked { background: rgba(%24, 0.14); border-color: %11; color: %2; }
QPushButton:focus { border-color: %11; }
QPushButton:disabled { color: rgba(%23, 0.55); background: %5; border-color: %26; }
QPushButton[actionRole="primary"] {
    background: %11; color: %27; border: 1px solid %11; border-radius: %6px;
    font-weight: 600; font-size: 14px; padding: 6px 16px;
}
QPushButton[actionRole="primary"]:hover { background: %12; border-color: %12; }
QPushButton[actionRole="primary"]:pressed { background: %11; border-color: %12; }
QPushButton[actionRole="primary"]:checked { background: %12; border-color: %27; }
QPushButton[actionRole="primary"]:focus { border-color: %27; }
QPushButton[actionRole="primary"]:disabled {
    background: %5; color: rgba(%23, 0.55); border-color: %26;
}
QPushButton[actionRole="ghost"] {
    background: transparent;
    color: %11;
    border: 1px solid rgba(%24, 0.22);
    border-radius: %21px;
    padding: 2px 10px;
    min-height: 24px;
    font-size: 13px;
    font-weight: 600;
}
QPushButton[actionRole="ghost"]:hover {
    color: %12;
    background: rgba(%24, 0.16);
    border-color: rgba(%24, 0.45);
}
QPushButton[actionRole="ghost"]:pressed {
    background: rgba(%24, 0.24);
    border-color: rgba(%24, 0.60);
}
QPushButton[actionRole="ghost"]:checked {
    background: rgba(%24, 0.18);
    border-color: %11;
    color: %12;
}
QPushButton[actionRole="ghost"]:focus { border-color: %11; }
QPushButton[actionRole="ghost"]:disabled {
    color: rgba(%23, 0.55);
    background: transparent;
    border-color: rgba(%23, 0.18);
}
QPushButton[actionRole="danger"] {
    background: transparent;
    color: %22;
    border: 1px solid %22;
    font-weight: 600;
}
QPushButton[actionRole="danger"]:hover { background: rgba(%28, 0.14); }
QPushButton[actionRole="danger"]:pressed { background: rgba(%28, 0.22); }
QPushButton[actionRole="danger"]:checked {
    background: rgba(%28, 0.18);
    border-color: %22;
}
/* The triplet marker is for a wash; a bare border takes the colour itself.
   Focus keeps the danger identity and adds a wash, or a keyboard user would see
   no focus at all on a destructive action. */
QPushButton[actionRole="danger"]:focus {
    background: rgba(%28, 0.10);
    border-color: %22;
}
QPushButton[actionRole="danger"]:disabled {
    color: rgba(%23, 0.55);
    background: transparent;
    border-color: %26;
}
/* A control that states an action role sits on the one action height and the
   symmetric inset pair, whatever its role's fill is. The compact ID-scoped
   widgets below keep the sizes their own rules fix. */
QPushButton[actionRole] {
    min-height: %31px;
    max-height: %31px;
    padding: 0 %32px;
    text-align: center;
}
QPushButton[actionRole="primary"] { padding: 0 %33px; }
/* A colour chip paints its own disc and the caller fixes its size; the generic
   button box model (padding, border, minimum height) would inflate the size hint
   past that fixed size, so it is cleared for the chips only. */
QPushButton#accentSwatch, QPushButton#gradientSwatch,
QPushButton#customAccentSwatch, QPushButton#customGradientSwatch {
    background: transparent; border: none; padding: 0;
    min-width: %29px; max-width: %29px;
    min-height: %29px; max-height: %29px; border-radius: 0;
}
QPushButton#settingsChip { padding: 0; min-width: %13px;
    max-width: %13px; min-height: %13px; max-height: %13px; border: 1px solid transparent; }
QPushButton#animeCard { background: %5; border: 1px solid %10;
    border-radius: %6px; padding: 0; }
QPushButton#animeCard:hover { background: %8; border-color: rgba(%24, 0.45); }
QPushButton#animeCard:focus { border-color: %11; }
QPushButton#carouselControl { background: %25; color: %2; border: 1px solid %26;
    border-radius: 8px; padding: 0; min-width: 30px; max-width: 30px;
    min-height: 30px; max-height: 30px; }
QPushButton#carouselControl:hover { background: %8; border-color: rgba(%24, 0.45); color: %11; }
QPushButton#carouselControl:pressed { background: %9; }
QPushButton#carouselControl:disabled { color: rgba(%23, 0.22); background: rgba(%23, 0.03); border-color: rgba(%23, 0.06); }
QWidget#homeFeedRow { background: %5; border: 1px solid %10; border-radius: %6px; }
QWidget#homeFeedRow:hover { background: %8; border-color: rgba(%24, 0.30); }
QPushButton#homeFeedTitle { text-align: left; background: transparent; border: none;
    padding: %15px; font-weight: 600; }
QLabel#homeFeedMeta { background: transparent; color: %4; font-size: 12px; }
QLabel#homeFeedPoster { background: %1; border: none; border-radius: %6px; }
QScrollArea#animeCarousel { background: transparent; border: none; }
QScrollArea#discoveryScroll { background: transparent; border: none; }
QWidget#animeCarouselTrack { background: transparent; }
QLabel#carouselPoster { background: %1; color: %4; border-radius: %6px; font-size: 12px; }
QLabel#carouselKind { background: transparent; color: %11; font-size: 11px; font-weight: 600; }
QLabel#carouselTitle { background: transparent; color: %2; font-weight: 600; font-size: 13px; }
QLabel#carouselStatus { background: rgba(%23, 0.06); color: %4;
    font-size: 11px; font-weight: 600; border-radius: 4px; padding: 2px 6px; }
QLabel#carouselStatusLive { background: rgba(%24, 0.16); color: %11;
    font-size: 11px; font-weight: 700; border-radius: 4px; padding: 2px 6px; }
QLabel#carouselEpisodes { background: transparent; color: %4; font-size: 12px; }
QMenu { background: %5; color: %2; border: 1px solid %10;
    border-radius: %6px; padding: %15px; }
QMenu::item { min-height: %16px; padding: 0 %7px; border-radius: %6px; }
QMenu::item:selected { background: %9; color: %2; }
QMenu::item:disabled { color: %4; }
QMenu::separator { height: 1px; background: %10; margin: %15px; }
QMenu::section { color: %4; font-size: 12px; font-weight: 600;
    padding: %3px %7px %15px %7px; border: none; }
QPushButton#pagerControl { background: transparent; color: %2;
    border: 1px solid transparent; border-radius: %17px; padding: 0;
    min-width: %14px; min-height: %14px; }
QPushButton#pagerControl:hover { background: %8; }
QPushButton#pagerControl:disabled { color: %4; background: transparent; }
QPushButton#pagerChip { background: transparent; color: %4;
    border: 1px solid transparent; border-radius: %17px; padding: 7px %18px; }
QPushButton#pagerChip:hover { color: %2; background: %8; }
QPushButton#pagerChipActive { background: %9; color: %2; font-weight: 600; }
QTabWidget::pane { border: none; border-top: 1px solid %10; background: %1; }
QTabWidget#settingsTabs::pane { border: none; border-top: 1px solid %10; background: %1; }
QTabBar::tab { background: transparent; color: %4; border: none;
    border-bottom: 2px solid transparent; padding: 7px %18px; margin-right: 2px;
    font-weight: 600; }
QTabBar::tab:selected { color: %2; border-bottom: 2px solid %11;
    background: rgba(%24, 0.08); border-top-left-radius: %21px;
    border-top-right-radius: %21px; }
QTabBar::tab:hover:!selected { color: %2; background: rgba(%23, 0.03);
    border-top-left-radius: %21px; border-top-right-radius: %21px; }
QTableWidget {
    background: %1;
    alternate-background-color: %5;
    color: %2;
    border: 1px solid %10;
    border-radius: %6px;
    gridline-color: %10;
    selection-background-color: rgba(%24, 0.20);
    selection-color: %2;
    outline: none;
}
QHeaderView::section {
    background: %5;
    color: %4;
    border: none;
    border-bottom: 1px solid %10;
    border-right: 1px solid %10;
    padding: 5px %3px;
    font-size: 12px;
    font-weight: 600;
}
QListWidget { background: %1; border: 1px solid %10; border-radius: %6px; padding: 4px; outline: none; }
QListWidget::item { min-height: 28px; padding: 2px %3px; border-radius: %6px;
    border-left: 2px solid transparent; }
QListWidget#searchResults::item { min-height: %19px; padding: %3px %7px; }
QListWidget::item:hover { background: %8; }
QListWidget::item:selected { background: %9; color: %2; border-left: 2px solid %11; }
QDialog#catalogueFiltersDialog, QDialog#teamPickerDialog { background: %1; }
QDialog#catalogueFiltersDialog QListWidget::indicator,
QDialog#teamPickerDialog QListWidget::indicator {
    width: 16px; height: 16px; border: 1px solid %10; border-radius: 4px; background: %1;
}
QDialog#catalogueFiltersDialog QListWidget::indicator:checked,
QDialog#teamPickerDialog QListWidget::indicator:checked {
    background: %11; border-color: %11;
}
QGroupBox#catalogueFilterSection {
    background: %5; border: 1px solid %10; border-radius: %6px;
    padding: 10px %7px %7px %7px; font-weight: 600;
}
QGroupBox#catalogueFilterSection::title {
    subcontrol-origin: padding; subcontrol-position: top left; left: %7px;
    padding: 0 0 %3px 0; color: %2; background: transparent; font-size: 15px;
}
QDialog#catalogueFiltersDialog QSpinBox::up-button,
QDialog#catalogueFiltersDialog QSpinBox::down-button { width: 0; border: none; }
QTreeWidget#benchmarkModelTree {
    background: %1;
    color: %2;
    border: 1px solid %10;
    border-radius: %6px;
    padding: %3px;
    outline: none;
}
QTreeWidget#benchmarkModelTree::item {
    min-height: 24px;
    padding: %3px %7px;
    border-radius: %6px;
}
QTreeWidget#benchmarkModelTree::item:hover { background: %8; }
QTreeWidget#benchmarkModelTree::item:selected { background: %9; color: %2; }
QTreeWidget#benchmarkModelTree::indicator {
    width: 14px; height: 14px;
    border: 1px solid %10;
    background: %1;
    border-radius: 3px;
}
QTreeWidget#benchmarkModelTree::indicator:unchecked { background: %1; }
QTreeWidget#benchmarkModelTree::indicator:checked { background: %4; border-color: %2; }
QTreeWidget#benchmarkModelTree::indicator:indeterminate { background: %8; border-color: %10; }
QFrame#benchmarkHeader { border: none; border-bottom: 1px solid %10; }
QTableWidget#benchmarkResults {
    background: %1;
    alternate-background-color: %5;
    color: %2;
    border: 1px solid %10;
    border-radius: %6px;
    gridline-color: transparent;
    outline: none;
    selection-background-color: %9;
    selection-color: %2;
}
QTableWidget#benchmarkResults::item {
    padding: %3px %7px;
    border-bottom: 1px solid %10;
}
QTableWidget#benchmarkResults QHeaderView::section {
    background: %5;
    color: %4;
    border: none;
    border-bottom: 1px solid %10;
    padding: 5px %7px;
    font-size: 12px;
    font-weight: 600;
}
QLabel#benchmarkRunSummary, QLabel#benchmarkRunStatus {
    background: transparent;
    color: %4;
    border: none;
    padding: 0;
    font-size: 12px;
    font-weight: 600;
}
QProgressBar#benchmarkProgress, QProgressBar#benchmarkEngineProgress {
    background: %1;
    border: 1px solid %10;
    border-radius: %6px;
    min-height: 4px;
    max-height: 4px;
    text-align: center;
}
QProgressBar#benchmarkProgress[runState="failure"], QProgressBar#benchmarkEngineProgress[runState="failure"] { border-color: %22; }
QProgressBar#benchmarkProgress::chunk, QProgressBar#benchmarkEngineProgress::chunk {
    background: %11; border-radius: %6px;
}
QProgressBar#benchmarkProgress[runState="failure"]::chunk { background: %22; }
QLabel#benchmarkEmptyState { color: %4; background: transparent; padding: %18px; }
QLabel#benchmarkResultDetail { color: %4; background: transparent; }
QScrollArea { border: none; background: transparent; }
QScrollBar:vertical { background: %1; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: %10; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: %11; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { background: %1; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: %10; border-radius: 5px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: %11; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QToolTip { background: %8; color: %2; border: 1px solid %10; padding: %15px %3px; }
QWidget#componentAcquisitionBar { background: %5; border-top: 1px solid %10; }
QWidget#componentAcquisitionBar QLabel { background: transparent; }
QDialog#coverViewer { background: %20; }
QWidget#coverViewerView { background: transparent; border: none; }
)QSS");
}

// The media detail page is the one editorial surface in the application.
// Every rule is scoped to QWidget#titlePage and derives its roles from the
// selected application appearance. Spacing and column widths are owned by
// TitlePage's layout, while this sheet owns colour, type scale,
// radii and control states.

QString titlePage(const Palette& p, const Appearance& a) {
    const QColor accentText = contrastText(p.accentValue);
    const QColor hoverText = contrastText(QColor(p.titleAccentHover));
    // Decorative chips, cards, posters and the details panel retain their
    // appearance-scaled radii; buttons use the application radius directly.
    const auto scaled = [&a](int value) { return scaledRadius(value, a); };
    return QStringLiteral(R"QSS(
QWidget#titlePage {
    background: %1;
    color: %6;
}
QWidget#titlePage QLabel { background: transparent; }
QWidget#titlePage QScrollArea#titleOverview,
QWidget#titlePage QScrollArea { background: transparent; border: none; }
QWidget#titlePage QWidget#titleBody,
QWidget#titlePage QWidget#titleHero,
QWidget#titlePage QWidget#titleHeroPoster,
QWidget#titlePage QWidget#titleDisclosureActions,
QWidget#titlePage QWidget#titleRelated,
QWidget#titlePage QWidget#titleEpisodes { background: transparent; border: none; }

QWidget#titlePage QWidget#titleHeader {
    background: %1;
    border-bottom: 1px solid %8;
}
QWidget#titlePage QWidget#titleHeader QPushButton {
    background: %43;
    color: %6;
    border: 1px solid %44;
    border-radius: %42px;
    min-height: %12px;
    max-height: %12px;
    padding: 0 %37px;
    font-size: %26px;
    font-weight: 500;
    text-align: center;
}
QWidget#titlePage QWidget#titleHeader QPushButton:hover:enabled { border-color: %4; }
QWidget#titlePage QWidget#titleHeader QPushButton:pressed { background: %3; }
QWidget#titlePage QWidget#titleHeader QPushButton:focus { border-color: %4; }
QWidget#titlePage QWidget#titleHeader QPushButton:disabled { color: %7; }
/* The same specificity as the header's pill rule, plus the chip's id: the
   gear states its whole 40 px square, so no inherited padding can widen it. */
QWidget#titlePage QWidget#titleHeader QPushButton#settingsChip {
    background: %43;
    border: 1px solid %44;
    border-radius: %42px;
    min-width: %12px;
    max-width: %12px;
    min-height: %12px;
    max-height: %12px;
    padding: 0;
}
QWidget#titlePage QWidget#titleHeader QPushButton#settingsChip:hover { border-color: %4; }
QWidget#titlePage QWidget#titleHeader QPushButton#settingsChip:pressed { background: %3; }
QWidget#titlePage QWidget#titleHeader QPushButton#settingsChip:focus { border-color: %4; }
QWidget#titlePage QLabel#status,
QWidget#titlePage QLabel#muted { color: %7; font-size: %29px; }

QWidget#titlePage QLabel#titleHeading {
    color: %6;
    font-size: %21px;
    font-weight: 600;
}
QWidget#titlePage QLabel#titleHeading:focus,
QWidget#titlePage QLabel#titleSubtitle:focus {
    border-bottom: 2px solid %4;
}
QWidget#titlePage[compact="true"] QLabel#titleHeading { font-size: %22px; }
QWidget#titlePage[narrow="true"] QLabel#titleHeading { font-size: %41px; }
QWidget#titlePage QLabel#titleSubtitle {
    color: %7;
    font-size: %23px;
    font-weight: 400;
}
QWidget#titlePage QLabel#titleFacts,
QWidget#titlePage QLabel#titleScores,
QWidget#titlePage QLabel#titleLinks,
QWidget#titlePage QLabel#titleHeroWatchContext { font-size: %26px; }
QWidget#titlePage QLabel#titleStudio {
    color: %7;
    font-size: %26px;
    font-weight: 600;
}
QWidget#titlePage QLabel#titleFacts { color: %7; font-size: %26px; }
QWidget#titlePage QLabel#titleScores {
    color: %6;
    font-size: %26px;
    font-weight: 500;
    background: transparent;
    border: none;
    padding: 0;
}
QWidget#titlePage QLabel#titleGenreChip,
QWidget#titlePage QLabel#titleTagChip {
    color: %7;
    background: transparent;
    border: 1px solid %8;
    border-radius: %42px;
    padding: %49px %48px;
    font-size: %26px;
    font-weight: 400;
}
QWidget#titlePage QLabel#titleDescription { color: %6; font-size: %24px; }
QWidget#titlePage QPushButton#titleSynopsisToggle,
QWidget#titlePage QPushButton#titleDetailsToggle,
QWidget#titlePage QPushButton#titleDiscoveryToggle {
    min-height: %47px;
    max-height: %47px;
}
QWidget#titlePage QLabel#titleLinks { color: %6; font-size: %26px; }
QWidget#titlePage QLabel#titleGenreChip:hover,
QWidget#titlePage QLabel#titleTagChip:hover {
    color: %6;
    background: %3;
    border-color: rgba(%9, 0.55);
}

/* Ghost actions: no fill, accent text. The page keeps its own pill geometry so
   the disclosure row still lines up with the hero's type scale, and the shared
   role carries the colour contract. */
QWidget#titlePage QPushButton[actionRole="ghost"] {
    background: transparent;
    color: %4;
    border: 1px solid %8;
    border-radius: %42px;
    min-height: %47px;
    max-height: %47px;
    padding: 0 %37px;
    font-size: %26px;
    font-weight: 500;
    text-align: center;
}
QWidget#titlePage QPushButton[actionRole="ghost"]:hover:enabled {
    background: rgba(%9, 0.16); border-color: rgba(%9, 0.45); color: %5;
}
QWidget#titlePage QPushButton[actionRole="ghost"]:pressed { background: rgba(%9, 0.22); }
QWidget#titlePage QPushButton[actionRole="ghost"]:checked {
    background: rgba(%9, 0.18); border-color: %4; color: %4;
}
QWidget#titlePage QPushButton[actionRole="ghost"]:checked:hover { border-color: %5; }
QWidget#titlePage QPushButton[actionRole="ghost"]:focus { border-color: %4; }
QWidget#titlePage QPushButton[actionRole="ghost"]:disabled {
    color: %7; background: transparent; border-color: %8;
}
QWidget#titlePage QPushButton#titleDetailsToggle,
QWidget#titlePage QPushButton#titleSynopsisToggle { border-color: transparent; }
QWidget#titlePage QPushButton#titleDetailsToggle:hover:enabled,
QWidget#titlePage QPushButton#titleSynopsisToggle:hover:enabled {
    background: %3;
    color: %6;
    border-color: %8;
}
QWidget#titlePage QPushButton#titleDetailsToggle:checked,
QWidget#titlePage QPushButton#titleSynopsisToggle:checked {
    background: %3;
    border-color: %4;
}
QWidget#titlePage QPushButton#titleDetailsToggle:focus,
QWidget#titlePage QPushButton#titleSynopsisToggle:focus { border-color: %4; }

QWidget#titlePage QWidget#titlePlaybackControls {
    background: %2;
    border: 1px solid %8;
    border-radius: %14px;
}
QWidget#titlePage QWidget#titleWatchPanel {
    background: %2;
    border: 1px solid %8;
    border-radius: %14px;
}
QWidget#titlePage QLabel#episodeAvailabilityLabel,
QWidget#titlePage QLabel#nextAiringLabel {
    background: transparent; border: none; color: %7; padding: 0; font-size: %26px;
}
QWidget#titlePage QLabel#titleEpisodeStatus {
    background: transparent; border: none; color: %7; padding: 0; font-size: %26px;
}
QWidget#titlePage QLabel#titleEpisodeStatus[state="error"] {
    color: %6; font-weight: 600;
}
QWidget#titlePage QLabel#titleWatchEpisode {
    color: %6;
    font-size: %30px;
    font-weight: 500;
}
QWidget#titlePage QLabel#titleWatchMeta { color: %7; font-size: %26px; }
QWidget#titlePage QLabel#titleWatchStatus { color: %7; font-size: %29px; }
QWidget#titlePage QProgressBar#titleWatchProgress {
    background: %8; border: none; border-radius: 2px;
    min-height: 4px; max-height: 4px;
}
QWidget#titlePage QProgressBar#titleWatchProgress::chunk {
    background: %4; border-radius: 2px;
}
QWidget#titlePage QPushButton#titleWatchButton,
QWidget#titlePage QPushButton#titleHeroWatchButton {
    background: %4;
    color: %39;
    border: 1px solid %4;
    border-radius: %42px;
    min-height: %45px;
    max-height: %45px;
    padding: 0 %46px;
    font-size: %24px;
    font-weight: 600;
    text-align: center;
}
QWidget#titlePage QPushButton#titleWatchButton:hover:enabled,
QWidget#titlePage QPushButton#titleHeroWatchButton:hover:enabled {
    background: %5; border-color: %5; color: %40;
}
QWidget#titlePage QPushButton#titleWatchButton:pressed,
QWidget#titlePage QPushButton#titleHeroWatchButton:pressed { background: %4; border-color: %4; }
QWidget#titlePage QPushButton#titleWatchButton:focus,
QWidget#titlePage QPushButton#titleHeroWatchButton:focus { border-color: %6; }
QWidget#titlePage QPushButton#titleWatchButton:disabled,
QWidget#titlePage QPushButton#titleHeroWatchButton:disabled {
    background: %2;
    border-color: %8;
    color: %7;
}

QWidget#titlePage QLabel#releaseKind,
QWidget#titlePage QLabel#providerSelectorHeading {
    color: %7;
    font-size: %26px;
    font-weight: 500;
}
QWidget#titlePage QWidget#releaseChipsHost { background: transparent; }
QWidget#titlePage QWidget#releaseLane {
    background: transparent;
    border: none;
}
QWidget#titlePage QWidget#releaseLane[divided="true"] { border-bottom: 1px solid %8; }
QWidget#titlePage QWidget#releaseProviderDivider { background: %8; border: none; }
QWidget#titlePage QWidget#releaseProviderRow {
    background: transparent;
    border: none;
    border-top: 1px solid %8;
}
QWidget#titlePage QPushButton#releaseChip {
    background: %43;
    color: %7;
    border: 1px solid %44;
    border-radius: %42px;
    padding: 0 %20px;
    font-size: %26px;
    font-weight: 500;
}
QWidget#titlePage QPushButton#releaseChip:hover:enabled {
    color: %6;
    border-color: rgba(%9, 0.55);
}
QWidget#titlePage QPushButton#releaseChip:checked {
    background: %3;
    color: %6;
    border-color: %4;
}
QWidget#titlePage QPushButton#releaseChip:checked:hover { border-color: %5; }
QWidget#titlePage QPushButton#releaseChip:pressed { background: %3; }
QWidget#titlePage QPushButton#releaseChip:focus { border-color: %4; }
QWidget#titlePage QPushButton#releaseChip:disabled { color: rgba(%10, 0.45); }

QWidget#titlePage QPushButton#episodeTypeFilter {
    background: transparent;
    color: %7;
    border: 1px solid transparent;
    border-radius: %42px;
    min-height: 24px;
    max-height: 24px;
    padding: 0 8px;
    font-size: %28px;
}
QWidget#titlePage QPushButton#episodeTypeFilter:hover {
    color: %6;
    border-color: %8;
}
QWidget#titlePage QPushButton#episodeTypeFilter:checked {
    background: %3;
    color: %6;
    border-color: %4;
}

QWidget#titlePage QTabWidget#episodeTabs::pane {
    border: none;
    background: transparent;
    padding: 0;
    margin-top: 0;
}
QWidget#titlePage QTabWidget#episodeTabs QTabBar::tab,
QWidget#titlePage QTabBar#providerBar::tab {
    background: %2;
    color: %7;
    border: 1px solid transparent;
    border-radius: %42px;
    padding: %32px %33px;
    margin-right: %34px;
    font-size: %26px;
    font-weight: 500;
}
QWidget#titlePage QTabWidget#episodeTabs QTabBar::tab:selected,
QWidget#titlePage QTabBar#providerBar::tab:selected {
    background: %3;
    color: %6;
    border: 1px solid %4;
}
QWidget#titlePage QTabWidget#episodeTabs QTabBar::tab:hover:!selected,
QWidget#titlePage QTabBar#providerBar::tab:hover:!selected {
    color: %6;
    border-color: rgba(%9, 0.45);
}
QWidget#titlePage QTabWidget#episodeTabs QTabBar::tab:disabled,
QWidget#titlePage QTabBar#providerBar::tab:disabled { color: rgba(%10, 0.45); }

QWidget#titlePage QLabel#titleSectionHeading,
QWidget#titlePage QLabel#episodeSectionHeading {
    color: %6;
    font-size: %25px;
    font-weight: 700;
}
QWidget#titlePage QWidget#titleSectionHeader,
QWidget#titlePage QWidget#titleEpisodeHeader {
    background: transparent;
    border-bottom: 1px solid %8;
}
QWidget#titlePage QLabel#titleEpisodeCount { color: %7; font-size: %26px; }

QWidget#titlePage QScrollArea#animeCarousel { background: transparent; border: none; }
QWidget#titlePage QWidget#animeCarouselTrack { background: transparent; }
QWidget#titlePage QPushButton#carouselControl {
    background: %43;
    color: %6;
    border: 1px solid %44;
    border-radius: %42px;
    min-width: %18px;
    max-width: %18px;
    min-height: %18px;
    max-height: %18px;
    padding: 0;
}
QWidget#titlePage QPushButton#carouselControl:hover:enabled { border-color: %4; color: %6; }
QWidget#titlePage QPushButton#carouselControl:pressed { background: %3; }
QWidget#titlePage QPushButton#carouselControl:focus { border-color: %4; }
QWidget#titlePage QPushButton#carouselControl:disabled {
    background: %2;
    border-color: %8;
    color: rgba(%10, 0.35);
}
QWidget#titlePage QPushButton#animeCard {
    background: %2;
    color: %6;
    border: 1px solid %8;
    border-radius: %42px;
    padding: 0;
}
QWidget#titlePage QPushButton#animeCard:hover { background: %2; border-color: %4; }
QWidget#titlePage QPushButton#animeCard:focus { border-color: %4; }
QWidget#titlePage QPushButton#animeCard:pressed { background: %3; }
QWidget#titlePage QLabel#carouselPoster {
    background: %1;
    color: %7;
    border: none;
    border-radius: %15px;
    font-size: %28px;
}
QWidget#titlePage QLabel#carouselKind {
    color: %7;
    font-size: %27px;
    font-weight: 600;
}
QWidget#titlePage QLabel#carouselTitle {
    color: %6;
    font-size: %26px;
    font-weight: 500;
}
QWidget#titlePage QLabel#carouselStatus {
    background: %3;
    color: %7;
    border-radius: %13px;
    padding: 2px 6px;
    font-size: %27px;
    font-weight: 600;
}
QWidget#titlePage QLabel#carouselStatusLive {
    background: rgba(%9, 0.16);
    color: %5;
    border-radius: %13px;
    padding: 2px 6px;
    font-size: %27px;
    font-weight: 700;
}
QWidget#titlePage QLabel#carouselEpisodes { color: %7; font-size: %28px; }

QWidget#titlePage QPushButton#episodeButton {
    background: %43;
    color: %6;
    border: 1px solid %44;
    border-radius: %42px;
    min-height: %17px;
    max-height: %17px;
    padding: 4px 0 0;
    font-size: %26px;
    font-weight: 500;
}
QWidget#titlePage QLabel#episodeTypeBadge { background: transparent; border: none; }
QWidget#titlePage QPushButton#episodeButton:hover:enabled { border-color: rgba(%9, 0.55); }
QWidget#titlePage QPushButton#episodeButton:checked { background: %3; border-color: rgba(%9, 0.45); color: %6; }
QWidget#titlePage QPushButton#episodeButton:checked:hover { border-color: %5; }
QWidget#titlePage QPushButton#episodeButton[current="true"] {
    background: %3;
    border-color: %4;
    color: %6;
}
QWidget#titlePage QPushButton#episodeButton[current="true"]:hover { border-color: %5; }
QWidget#titlePage QPushButton#episodeButton:pressed { background: %3; }
QWidget#titlePage QPushButton#episodeButton:focus { border-color: %4; }
QWidget#titlePage QPushButton#episodeButton:disabled {
    background: %2;
    border-color: %44;
    color: %7;
}
QWidget#titlePage QPushButton#episodeButton:checked:disabled {
    background: %2;
    border-color: %44;
    color: %7;
}
QWidget#titlePage QWidget#episodeWatchedMark {
    background: %4;
    border: none;
    border-radius: 1px;
}
QWidget#titlePage QWidget#titleEpisodeLegend { background: transparent; }
QWidget#titlePage QLabel#episodeLegendLabel { color: %7; font-size: %27px; }
QWidget#titlePage QWidget#episodeLegendCurrent {
    background: transparent;
    border: 1px solid %4;
    border-radius: %38px;
}
QWidget#titlePage QWidget#episodeLegendWatched {
    background: %4;
    border: none;
    border-radius: 1px;
}
QWidget#titlePage QWidget#episodeLegendUnavailable {
    background: %2;
    border: none;
    border-radius: %38px;
}
QWidget#titlePage QPushButton#titleWatchWatchedToggle {
    background: %43;
    color: %7;
    border: 1px solid %44;
    border-radius: %42px;
    min-height: %47px;
    max-height: %47px;
    padding: 0 %37px;
    font-size: %26px;
    font-weight: 500;
    text-align: center;
}
QWidget#titlePage QPushButton#titleWatchWatchedToggle:hover:enabled {
    color: %6;
    border-color: rgba(%9, 0.55);
}
QWidget#titlePage QPushButton#titleWatchWatchedToggle:checked {
    background: %3;
    color: %6;
    border-color: %4;
}
QWidget#titlePage QPushButton#titleWatchWatchedToggle:checked:hover { border-color: %5; }
QWidget#titlePage QPushButton#titleWatchWatchedToggle:pressed { background: %3; }
QWidget#titlePage QPushButton#titleWatchWatchedToggle:focus { border-color: %4; }
QWidget#titlePage QPushButton#titleWatchWatchedToggle:disabled {
    color: rgba(%10, 0.45);
    border-color: %8;
}

QWidget#titlePage QLabel#titleCover {
    background: %2;
    color: %7;
    border: 1px solid %8;
    border-radius: %14px;
    font-size: %26px;
}

QWidget#titlePage QWidget#titleDetailsOverlay { background: transparent; }
QWidget#titlePage QWidget#titleDetailsScrim {
    background: rgba(%11, 0.75);
    border: none;
    border-radius: 0;
    padding: 0;
}
QWidget#titlePage QWidget#titleDetailsPanel {
    background: %2;
    border: none;
    border-left: 1px solid %8;
}
QWidget#titlePage QScrollArea#titleDetailsScroll { background: transparent; border: none; }
QWidget#titlePage QWidget#titleDetails {
    background: transparent;
    color: %6;
    border: none;
}
QWidget#titlePage QWidget#titleDetails QLabel { background: transparent; }
QWidget#titlePage QWidget#titleDetailsHeader {
    background: %2;
    border-bottom: 1px solid %8;
}
QWidget#titlePage QLabel#titleDetailsHeading {
    color: %6;
    font-size: %31px;
    font-weight: 700;
}
QWidget#titlePage QLabel#titleDetailsTitle {
    color: %6;
    font-size: %30px;
    font-weight: 600;
}
QWidget#titlePage QLabel#titleDetailsTitle:hover,
QWidget#titlePage QLabel#titleDetailsTitle:focus {
    color: %4;
}
QWidget#titlePage QWidget#titleDetailsFacts { background: transparent; }
QWidget#titlePage QPushButton#titleDetailsAction {
    color: %6;
    background: transparent;
    border: 1px solid transparent;
    border-radius: %42px;
    padding: %49px %48px;
    text-align: left;
}
QWidget#titlePage QPushButton#titleDetailsChip {
    color: %6;
    background: %3;
    border: 1px solid %8;
    border-radius: %42px;
    padding: %49px %48px;
}
QWidget#titlePage QPushButton#titleDetailsAction:hover,
QWidget#titlePage QPushButton#titleDetailsAction:focus,
QWidget#titlePage QPushButton#titleDetailsChip:hover,
QWidget#titlePage QPushButton#titleDetailsChip:focus {
    color: %4;
    border-color: %4;
}
QWidget#titlePage QLabel#titleDetailsThumb {
    background: %1;
    border: 1px solid %8;
    border-radius: %35px;
}
QWidget#titlePage QLabel#titleDetailsThumb:hover,
QWidget#titlePage QLabel#titleDetailsThumb:focus {
    border-color: %4;
}
QWidget#titlePage QPushButton#titleDetailsClose {
    background: %43;
    color: %6;
    border: 1px solid %44;
    border-radius: %42px;
    min-width: %36px;
    max-width: %36px;
    min-height: %36px;
    max-height: %36px;
    padding: 0;
}
QWidget#titlePage QPushButton#titleDetailsClose:hover { border-color: %4; }
QWidget#titlePage QPushButton#titleDetailsClose:pressed { background: %3; }
QWidget#titlePage QPushButton#titleDetailsClose:focus { border-color: %4; }
)QSS")
        .arg(p.titleBackground)
        .arg(p.titleSurface)
        .arg(p.titleSurfaceHover)
        .arg(p.titleAccent)
        .arg(p.titleAccentHover)
        .arg(p.titleText)
        .arg(p.titleMuted)
        .arg(p.titleBorder)
        .arg(p.titleAccentRgb)
        .arg(p.titleMutedRgb)
        .arg(p.titleBackgroundRgb)
        .arg(kControlContentHeight)
        .arg(scaled(kTitlePillRadiusRound))
        .arg(scaled(kTitleCardRadius))
        .arg(scaled(kTitlePosterRadius))
        .arg(kTitleEpisodeButtonHeight)
        .arg(kTitleCircleControlSize)
        .arg(kTitleChipPaddingH)
        .arg(kTitleHeroTitlePixelSize)
        .arg(kTitleHeroTitlePixelSizeCompact)
        .arg(kTitleSubtitlePixelSize)
        .arg(kTitleBodyPixelSize)
        .arg(kTitleSectionHeadingPixelSize)
        .arg(kTitleMetaPixelSize)
        .arg(kTitleSmallPixelSize)
        .arg(kTitleTinyPixelSize)
        .arg(kTitleCaptionPixelSize)
        .arg(kTitleWatchEpisodePixelSize)
        .arg(kTitleDetailsHeadingPixelSize)
        .arg(kTitleTabPaddingV)
        .arg(kTitleTabPaddingH)
        .arg(kTitleTabSpacing)
        .arg(scaled(kTitleDetailsThumbRadius))
        .arg(kTitleDetailsCloseSize)
        .arg(kTitleButtonPaddingH)
        .arg(scaled(kTitleLegendSwatchRadius))
        .arg(accentText.name())
        .arg(hoverText.name())
        .arg(kTitleHeroTitlePixelSizeNarrow)
        .arg(radiusSmall(a))
        .arg(p.surfaceControlRaised)
        .arg(p.borderControl)
        .arg(kWatchCtaContentHeight)   // %45
        .arg(kWatchCtaInsetH)          // %46
        .arg(kControlContentHeight)    // %47
        .arg(kSpacingSmall)            // %48
        .arg(kSpacingXs);              // %49
}

QString searchPageSheet(const Appearance& appearance, const SearchHeaderPalette& roles) {
    QString sheet = QStringLiteral(R"QSS(
QWidget#searchPage QWidget#searchSidebar {
    background: @pillSurface;
    border: none;
    border-right: 1px solid @pillBorder;
}
QWidget#searchPage QLabel#sidebarBrandName {
    background: transparent;
    color: @text;
    font-size: %16px;
    font-weight: 700;
}
QWidget#searchPage QWidget#searchSidebar QToolButton {
    background: transparent;
    color: @text;
    border: 1px solid transparent;
    border-left: %17px solid transparent;
    border-radius: %1px;
    min-height: %2px;
    max-height: %2px;
    padding: 0 %18px;
    font-weight: 500;
    text-align: left;
}
QWidget#searchPage QWidget#searchSidebar QToolButton:hover {
    background: @secondaryHover;
    border-color: @border;
}
QWidget#searchPage QWidget#searchSidebar QToolButton:pressed {
    background: @input;
    border-color: @primary;
}
QWidget#searchPage QWidget#searchSidebar QToolButton:focus {
    border-color: @primary;
}
QWidget#searchPage QWidget#searchSidebar QToolButton:disabled {
    color: @pillText;
    background: transparent;
    border-color: transparent;
}
QWidget#searchPage QToolButton#homeButton:checked {
    background: @secondary;
    color: @text;
    border-left-color: @primary;
}
QWidget#searchPage QToolButton#homeButton:checked:hover,
QWidget#searchPage QToolButton#homeButton:checked:focus {
    border-left-color: @primary;
}
QWidget#searchPage QWidget#searchSidebar QToolButton#openMediaButton,
QWidget#searchPage QWidget#searchSidebar QToolButton#settingsChip {
    background: @secondary;
    color: @text;
    border: 1px solid @border;
    border-radius: %1px;
    min-height: %2px;
    max-height: %2px;
    padding: 0 %6px;
    font-weight: 500;
    text-align: center;
}
QWidget#searchPage QWidget#searchSidebar QToolButton#openMediaButton:hover,
QWidget#searchPage QWidget#searchSidebar QToolButton#settingsChip:hover {
    background: @secondaryHover;
    border-color: @primary;
}
QWidget#searchPage QWidget#searchSidebar QToolButton#openMediaButton:pressed,
QWidget#searchPage QWidget#searchSidebar QToolButton#settingsChip:pressed {
    background: @input;
}
QWidget#searchPage QWidget#searchSidebar QToolButton#openMediaButton:focus,
QWidget#searchPage QWidget#searchSidebar QToolButton#settingsChip:focus {
    border-color: @primary;
}
QWidget#searchPage QWidget#searchSidebar QToolButton#openMediaButton:disabled,
QWidget#searchPage QWidget#searchSidebar QToolButton#settingsChip:disabled {
    color: @pillText;
    background: @secondary;
    border-color: @border;
}
QWidget#searchPage QWidget#searchSidebar QToolButton#openMediaButton[compact="true"],
QWidget#searchPage QWidget#searchSidebar QToolButton#settingsChip[compact="true"] {
    padding: 0;
}
QWidget#searchPage QWidget#searchSidebar QToolButton#openMediaButton[compact="true"]::menu-indicator {
    image: none;
    width: 0px;
    height: 0px;
}
QWidget#searchPage QWidget#searchField {
    background: @input;
    border: 1px solid @inputBorder;
    border-radius: %1px;
    min-height: %2px;
    max-height: %2px;
}
QWidget#searchPage QWidget#searchField[focused="true"] { border-color: @primary; }
/* The row's three controls share one height: the field and the quota pill are
   free widgets pinned to the content height above, so the primary action has to
   give up the two pixels a full control height adds, or the row reads as three
   boxes of different sizes. */
QWidget#searchPage QPushButton#searchActionButton {
    min-height: %2px;
    max-height: %2px;
}
QWidget#searchPage QLineEdit#searchQuery {
    background: transparent;
    border: none;
    padding: 0;
    min-height: 0;
    font-size: 14px;
    color: @text;
    selection-background-color: @primary;
    selection-color: @onPrimary;
}
QWidget#searchPage QLabel#searchKeycap {
    background: transparent;
    border: none;
    padding: 0;
    font-size: 11px;
    font-weight: 600;
}
/* The header's controls take the shared action roles and only re-state the
   geometry the compact bar needs; their colours come from the same tokens the
   rest of the application uses, so a neutral control keeps its 3:1 boundary
   here too. */
QWidget#searchPage QPushButton[actionRole="secondary"] {
    background: @secondary;
    color: @text;
    border: 1px solid @border;
    border-radius: %1px;
    min-height: %2px;
    max-height: %2px;
    padding: 0 %6px;
    font-weight: 500;
    text-align: center;
}
QWidget#searchPage QPushButton[actionRole="secondary"]:hover {
    background: @secondaryHover;
    border-color: @primary;
}
QWidget#searchPage QPushButton[actionRole="secondary"]:pressed { background: @input; }
QWidget#searchPage QPushButton[actionRole="secondary"]:focus { border-color: @primary; }
QWidget#searchPage QPushButton[actionRole="secondary"]:disabled {
    color: @pillText;
    background: @secondary;
    border-color: @border;
}
/* The team browser's way back is a secondary action, not a bare chip: it takes
   the same height, boundary and radius as the header's other actions. */
QWidget#searchPage QPushButton#teamBackButton {
    background: @secondary;
    color: @text;
    border: 1px solid @border;
    border-radius: %1px;
    min-height: %2px;
    max-height: %2px;
    padding: 0 %6px;
    font-weight: 500;
    text-align: center;
}
QWidget#searchPage QPushButton#teamBackButton:hover {
    background: @secondaryHover;
    border-color: @primary;
}
QWidget#searchPage QPushButton#teamBackButton:pressed { background: @input; }
QWidget#searchPage QPushButton#teamBackButton:focus { border-color: @primary; }
QWidget#searchPage QPushButton[actionRole="primary"] {
    background: @primary;
    color: @onPrimary;
    border: 1px solid @primary;
    border-radius: %1px;
    min-height: %2px;
    max-height: %2px;
    padding: 0 %8px;
    font-weight: 600;
    text-align: center;
}
QWidget#searchPage QPushButton[actionRole="primary"]:hover,
QWidget#searchPage QPushButton[actionRole="primary"]:pressed { background: @primaryHover; }
QWidget#searchPage QPushButton[actionRole="primary"]:focus { border-color: @onPrimary; }
QWidget#searchPage QPushButton[actionRole="primary"]:disabled {
    background: @secondary;
    color: @onPrimary;
    border-color: @border;
}
QWidget#searchPage QWidget#ratePill {
    background: @pillSurface;
    border: 1px solid @pillBorder;
    border-radius: %1px;
    min-height: %2px;
    max-height: %2px;
}
/* The pill is also the way into Access: it answers a hover like the actions
   beside it. */
QWidget#searchPage QWidget#ratePill:hover { border-color: @primary; }
QWidget#searchPage QWidget#ratePill QLabel#muted {
    background: transparent;
    border: none;
    color: @pillText;
    font-size: 12px;
    font-weight: 500;
}
QWidget#searchPage QLabel#rateDot {
    background: @dotUnknown;
    border: none;
    border-radius: %9px;
    min-width: %10px;
    max-width: %10px;
    min-height: %10px;
    max-height: %10px;
}
QWidget#searchPage QScrollArea#discoveryScroll QWidget#pageIntro QLabel#pageHeading {
    padding-top: 0;
}
QWidget#searchPage QScrollArea#discoveryScroll QScrollBar:vertical {
    background: transparent;
    width: %12px;
    margin: 0;
}
QWidget#searchPage QScrollArea#discoveryScroll QScrollBar::handle:vertical {
    background: @borderStrong;
    border: none;
    border-radius: %13px;
    min-height: %14px;
    margin: 0 %15px;
}
QWidget#searchPage QScrollArea#discoveryScroll QScrollBar::handle:vertical:hover {
    background: @primary;
}
QWidget#searchPage QScrollArea#discoveryScroll QScrollBar::handle:vertical:pressed {
    background: @primaryHover;
}
QWidget#searchPage QScrollArea#discoveryScroll QScrollBar::add-line:vertical,
QWidget#searchPage QScrollArea#discoveryScroll QScrollBar::sub-line:vertical {
    background: transparent;
    border: none;
    height: 0;
}
QWidget#searchPage QScrollArea#discoveryScroll QScrollBar::add-page:vertical,
QWidget#searchPage QScrollArea#discoveryScroll QScrollBar::sub-page:vertical {
    background: transparent;
}
QWidget#searchPage QLabel#rateDot[tone="healthy"] { background: @dotHealthy; }
QWidget#searchPage QLabel#rateDot[tone="low"] { background: @dotLow; }
QWidget#searchPage QLabel#rateDot[tone="exhausted"] { background: @dotExhausted; }
QWidget#searchPage QLabel#rateDot[tone="unknown"] { background: @dotUnknown; }
)QSS")
        .arg(radiusSmall(appearance))      // %1
        .arg(kControlContentHeight)        // %2
        .arg(kControlInsetH)               // %6
        .arg(kControlInsetHPrimary)        // %8
        .arg(kSearchDotRadius)             // %9
        .arg(kSearchDotSize)               // %10
        .arg(kHomeScrollBarExtent)
        .arg((kHomeScrollBarExtent - 2 * kHomeScrollHandleInset) / 2)
        .arg(kHomeScrollHandleMinHeight)
        .arg(kHomeScrollHandleInset)
        .arg(kSearchSidebarBrandFontSize)
        .arg(kSearchSidebarIndicatorWidth)
        .arg(kSpacingSmall);
    // Longest name first where one token is a prefix of another (@border and
    // @borderStrong, @secondary and @secondaryHover, @primary and
    // @primaryHover), or the shorter replacement would eat the longer name.
    const QVector<std::pair<QString, QString>> rolesTable = {
        {QStringLiteral("@inputBorder"), roles.inputBorder},
        {QStringLiteral("@pillBorder"), roles.pillBorder},
        {QStringLiteral("@borderStrong"), roles.borderStrong},
        {QStringLiteral("@secondaryHover"), roles.secondaryHover},
        {QStringLiteral("@primaryHover"), roles.primaryHover},
        {QStringLiteral("@dotExhausted"), roles.dotExhausted},
        {QStringLiteral("@dotHealthy"), roles.dotHealthy},
        {QStringLiteral("@dotUnknown"), roles.dotUnknown},
        {QStringLiteral("@pillSurface"), roles.pillSurface},
        {QStringLiteral("@onPrimary"), roles.onPrimary},
        {QStringLiteral("@dotLow"), roles.dotLow},
        {QStringLiteral("@pillText"), roles.pillText},
        {QStringLiteral("@secondary"), roles.secondary},
        {QStringLiteral("@primary"), roles.primary},
        {QStringLiteral("@border"), roles.border},
        {QStringLiteral("@input"), roles.input},
        {QStringLiteral("@text"), roles.text},
    };
    for (const auto& entry : rolesTable) {
        sheet.replace(entry.first, entry.second);
    }
    return sheet;
}

}  // namespace

SearchHeaderPalette searchHeaderPalette(const Appearance& appearance) {
    const Palette p = paletteFor(appearance);
    SearchHeaderPalette roles;
    if (p.light) {
        roles.input = p.surfaceInput;
        roles.inputBorder = p.border;
        roles.border = p.borderControl;
        roles.borderStrong = p.borderControl;
        roles.secondary = p.surfaceControlRaised;
        roles.secondaryHover = p.surfaceHover;
        roles.text = p.text;
        roles.pillSurface = p.surfaceControl;
        roles.pillBorder = p.border;
        roles.pillText = p.muted;
        roles.dotHealthy = p.successFill;
        roles.dotLow = p.warningFill;
        roles.dotExhausted = p.dangerFill;
        roles.dotUnknown = p.textDisabled;
        roles.keycapSurface = p.surfaceHover;
        roles.keycapBorder = p.border;
    } else {
        roles.input = QString::fromLatin1(kSearchInput);
        roles.inputBorder = QString::fromLatin1(kSearchInputHairline);
        roles.border = p.borderControl;
        roles.borderStrong = p.borderControl;
        roles.secondary = p.surfaceControlRaised;
        roles.secondaryHover = QString::fromLatin1(kSearchSecondaryHover);
        roles.text = p.text;
        roles.pillSurface = QString::fromLatin1(kSearchPill);
        roles.pillBorder = QString::fromLatin1(kSearchPillHairline);
        roles.pillText = QString::fromLatin1(kSearchPillText);
        roles.dotHealthy = QString::fromLatin1(kSearchDotHealthy);
        roles.dotLow = p.warningFill;
        roles.dotExhausted = p.dangerFill;
        roles.dotUnknown = QString::fromLatin1(kSearchDotUnknown);
        roles.keycapSurface = QString::fromLatin1(kSearchKeycap);
        roles.keycapBorder = QString::fromLatin1(kSearchKeycapHairline);
    }
    // The search action is the chosen accent on every appearance, the shipped
    // one included. A special case here is what made a purple accent render a
    // pink button: the header must not carry a colour of its own.
    roles.primary = p.accent;
    roles.primaryHover = p.accentHover;
    roles.onPrimary = p.accentInk;
    return roles;
}

QString buildStylesheet(const Appearance& appearance) {
    mikaiInitAssetResources();
    const Palette p = paletteFor(appearance);
    const QString application = base()
            .arg(p.background, 1)
            .arg(p.text, 2)
            .arg(kSpacingSmall, 3)
            .arg(p.muted, 4)
            .arg(p.surface, 5)
            .arg(radiusSmall(appearance), 6)
            .arg(kSpacingMedium, 7)
            .arg(p.surfaceHover, 8)
            .arg(p.surfaceActive, 9)
            .arg(p.border, 10)
            .arg(p.accent, 11)
            .arg(p.accentHover, 12)
            .arg(kSettingsControlSize, 13)
            .arg(kSecondaryControlSize, 14)
            .arg(kSpacingXs, 15)
            .arg(kMenuItemMinHeight, 16)
            .arg(radiusMedium(appearance), 17)
            .arg(kSpacingLarge, 18)
            .arg(kSearchRowMinHeight, 19)
            .arg(p.videoBackground, 20)
            .arg(radiusPill(appearance), 21)
            .arg(p.danger, 22)
            .arg(p.wash, 23)
            .arg(p.accentRgb, 24)
            .arg(p.surfaceControlRaised, 25)
            .arg(p.borderControl, 26)
            .arg(p.accentInk, 27)
            .arg(p.dangerRgb, 28)
            .arg(kSettingsSwatchSize, 29)
            // The trailing numbers are place markers, not field widths: arg()
            // substitutes in call order.
            .arg(kBadgeHeight - 2 * 2 - 2)  // %30
            .arg(kControlContentHeight)     // %31
            .arg(kControlInsetH)            // %32
            .arg(kControlInsetHPrimary);    // %33
    return application + titlePage(p, appearance) +
           searchPageSheet(appearance, searchHeaderPalette(appearance));
}

QString buildStylesheet() { return buildStylesheet(Appearance{}); }

QString actionRoleName(ActionRole role) {
    switch (role) {
        case ActionRole::Primary:
            return QStringLiteral("primary");
        case ActionRole::Secondary:
            return QStringLiteral("secondary");
        case ActionRole::Ghost:
            return QStringLiteral("ghost");
        case ActionRole::Danger:
            return QStringLiteral("danger");
    }
    return QStringLiteral("secondary");
}

void applyActionRole(QAbstractButton* button, ActionRole role) {
    if (button == nullptr) return;
    const QString name = actionRoleName(role);
    if (button->property("actionRole").toString() != name) {
        button->setProperty("actionRole", name);
    }
    // The role states a colour contract, not a size. A widget-level minimum here
    // would outlive the sheet and inflate a control the caller fixed (a compact
    // row glyph, a self-painted chip); the sheets carry the height, so one
    // metric owns it instead of two.
    if (button->style() != nullptr) button->style()->polish(button);
}

int equalizedWidth(std::span<QAbstractButton* const> buttons, int minimum) {
    int width = qMax(0, minimum);
    qreal ratio = 1.0;
    for (QAbstractButton* button : buttons) {
        if (button == nullptr) continue;
        // A stylesheet font and box model reach the widget only at polish, so
        // measuring before it reads the ambient font and understates the row.
        button->ensurePolished();
        ratio = button->devicePixelRatioF();
        // The hint is the width the style paints this control's label and icon
        // at: it carries the role's inset pair, the icon box and the frame, and
        // it rounds each of them for the device. Rebuilding that sum from the
        // font advance alone lands up to a pixel under it, and the control that
        // then paints widest in the row breaks the equality the row asked for.
        width = qMax(width, button->sizeHint().width());
    }
    // Even in device pixels: the group shares one window, so the narrowest
    // logical width whose device width is even is the answer at 1.25 and 1.5
    // as well as at 1.0.
    while (qRound(width * ratio) % 2 != 0) ++width;
    return width;
}

void equalizeWidths(std::span<QAbstractButton* const> buttons, int minimum) {
    const int width = equalizedWidth(buttons, minimum);
    for (QAbstractButton* button : buttons) {
        if (button == nullptr) continue;
        button->setMinimumWidth(width);
        QSizePolicy policy = button->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::MinimumExpanding);
        policy.setVerticalPolicy(QSizePolicy::Fixed);
        button->setSizePolicy(policy);
    }
}

void applyPalette(const Appearance& appearance) {
    const Palette p = paletteFor(appearance);
    // Qt draws its own sub-controls - combo arrows, menu indicators, checkbox
    // ticks, scrollbar arrows - from the palette, not from the sheet. Without
    // this the arrows stay the platform default black on a near-black field.
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(p.background));
    pal.setColor(QPalette::WindowText, QColor(p.text));
    pal.setColor(QPalette::Base, QColor(p.surfaceInput));
    pal.setColor(QPalette::AlternateBase, QColor(p.tableAlternateRow));
    pal.setColor(QPalette::Text, QColor(p.text));
    // Native-drawn buttons and their indicators follow the same raised neutral
    // the sheet paints a secondary control with, or a combo popup's own buttons
    // show the old dark slab next to a styled one.
    pal.setColor(QPalette::Button, QColor(p.surfaceControlRaised));
    pal.setColor(QPalette::ButtonText, QColor(p.text));
    pal.setColor(QPalette::BrightText, QColor(p.accentSoftBright));
    pal.setColor(QPalette::Highlight, QColor(p.accentValue));
    // The selected-text ink is contrast-derived from the same accent the
    // highlight is painted with; a fixed near-white dissolved into a light
    // accent, and into a white custom one.
    pal.setColor(QPalette::HighlightedText, QColor(p.accentInk));
    pal.setColor(QPalette::ToolTipBase, QColor(p.surfaceHover));
    pal.setColor(QPalette::ToolTipText, QColor(p.text));
    pal.setColor(QPalette::PlaceholderText, QColor(p.muted));
    pal.setColor(QPalette::Link, QColor(p.accent));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(p.textDisabled));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(p.textDisabled));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(p.textDisabled));
    pal.setColor(QPalette::Light, QColor(p.borderControl));
    pal.setColor(QPalette::Midlight, QColor(p.borderControl));
    pal.setColor(QPalette::Mid, QColor(p.borderControl));
    pal.setColor(QPalette::Dark, QColor(p.borderControl));
    QApplication::setPalette(pal);
}

}  // namespace mikai::app::theme

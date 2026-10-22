#include "theme/Theme.hpp"

#include "theme/Appearance.hpp"
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"

#include <QApplication>
#include <QPalette>

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
QLabel#sectionHeading { font-size: 15px; font-weight: 600; padding-top: %15px; background: transparent; }
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
    background: %8;
    border: 1px solid %10;
}
QFrame#settingsCard QPushButton:hover, QWidget#settingsCard QPushButton:hover {
    background: %9;
    border-color: rgba(189, 172, 255, 0.45);
}
QFrame#settingsCard QPushButton:disabled, QWidget#settingsCard QPushButton:disabled {
    color: %4;
    background: rgba(%23, 0.03);
    border-color: rgba(%23, 0.06);
}
QFrame#settingsCard QPushButton#primary, QWidget#settingsCard QPushButton#primary {
    background: %11;
    color: %1;
    border: none;
}
QFrame#settingsCard QPushButton#primary:hover, QWidget#settingsCard QPushButton#primary:hover {
    background: %12;
}
QWidget#presetShortcutRow {
    background: %5;
    border: 1px solid %10;
    border-radius: %6px;
}
QWidget#presetShortcutRow:hover {
    background: %8;
    border-color: rgba(189, 172, 255, 0.35);
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
    border-color: rgba(189, 172, 255, 0.40);
}
QGroupBox#nativeComponentPackCard[recommended="true"] {
    border-color: rgba(189, 172, 255, 0.55);
}
QGroupBox#nativeComponentPackCard[selected="true"] {
    border-color: %11;
    background: rgba(189, 172, 255, 0.08);
}
QGroupBox#nativeComponentPackCard::title { subcontrol-origin: margin;
    left: %7px; padding: 0 %15px; color: %2; font-weight: 600; }
QGroupBox#nativeComponentPackCard QLabel { background: transparent; }
QGroupBox#nativeComponentPackCard QCheckBox { background: transparent; }
QLabel#componentRecommendationBanner {
    background: rgba(189, 172, 255, 0.10);
    color: %2;
    border: 1px solid rgba(189, 172, 255, 0.38);
    border-radius: %6px;
    padding: %3px %7px;
}
QLabel[textRole="muted"], QGroupBox#nativeComponentPackCard QLabel[textRole="muted"] {
    color: %4;
    background: transparent;
}
QLabel[textRole="recommendation"], QGroupBox#nativeComponentPackCard QLabel[textRole="recommendation"] {
    color: %11;
    background: rgba(189, 172, 255, 0.14);
    border: 1px solid rgba(189, 172, 255, 0.35);
    border-radius: %21px;
    padding: 2px %3px;
    font-size: 12px;
    font-weight: 600;
}
QLabel[badgeRole="installed"], QGroupBox#nativeComponentPackCard QLabel[badgeRole="installed"] {
    color: %11;
    background: rgba(189, 172, 255, 0.12);
    border: 1px solid rgba(189, 172, 255, 0.30);
    border-radius: %21px;
    padding: 2px %3px;
    font-size: 12px;
    font-weight: 600;
}
QLabel[badgeRole="optional"], QGroupBox#nativeComponentPackCard QLabel[badgeRole="optional"] {
    color: %4;
    background: rgba(%23, 0.05);
    border: 1px solid %10;
    border-radius: %21px;
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
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right;
    width: 26px; border: none; background: transparent; }
QComboBox::down-arrow { image: url(:/icons/chevron-down.svg); width: 10px; height: 10px; }
QComboBox QAbstractItemView { background: %5; border: 1px solid %10;
    selection-background-color: %8; }
QCheckBox { spacing: %3px; }
QPushButton {
    background: %5; border: 1px solid %10; border-radius: %6px;
    padding: %3px %7px; min-height: 22px;
}
QPushButton:hover { background: %8; border-color: rgba(189, 172, 255, 0.35); }
QPushButton:pressed { background: %9; }
QPushButton:checked { background: rgba(189, 172, 255, 0.14); border-color: %11; color: %2; }
QPushButton:focus { border-color: %11; }
QPushButton:disabled { color: %4; background: %5; border-color: transparent; }
QPushButton#primary { background: %11; color: %1; border: none; font-weight: 600;
    font-size: 14px; padding: 6px 16px; border-radius: %6px; }
QPushButton#primary:hover { background: %12; }
QPushButton#primary:disabled { background: %8; color: %4; }
QPushButton#textAction {
    background: rgba(189, 172, 255, 0.08);
    color: %11;
    border: 1px solid rgba(189, 172, 255, 0.22);
    border-radius: %21px;
    padding: 2px 10px;
    min-height: 24px;
    font-size: 13px;
    font-weight: 600;
}
QPushButton#textAction:hover {
    color: %12;
    background: rgba(189, 172, 255, 0.16);
    border-color: rgba(189, 172, 255, 0.45);
}
QPushButton#textAction:checked {
    background: rgba(189, 172, 255, 0.18);
    border-color: %11;
    color: %12;
}
QPushButton#settingsChip { padding: 0; min-width: %13px;
    max-width: %13px; min-height: %13px; max-height: %13px; border: 1px solid transparent; }
QPushButton#animeCard { background: %5; border: 1px solid %10;
    border-radius: %6px; padding: 0; }
QPushButton#animeCard:hover { background: %8; border-color: rgba(189, 172, 255, 0.45); }
QPushButton#animeCard:focus { border-color: %11; }
QPushButton#carouselControl { background: %5; color: %2; border: 1px solid %10;
    border-radius: 8px; padding: 0; min-width: 30px; max-width: 30px;
    min-height: 30px; max-height: 30px; }
QPushButton#carouselControl:hover { background: %8; border-color: rgba(189, 172, 255, 0.45); color: %11; }
QPushButton#carouselControl:pressed { background: %9; }
QPushButton#carouselControl:disabled { color: rgba(%23, 0.22); background: rgba(%23, 0.03); border-color: rgba(%23, 0.06); }
QWidget#homeFeedRow { background: %5; border: 1px solid %10; border-radius: %6px; }
QWidget#homeFeedRow:hover { background: %8; border-color: rgba(189, 172, 255, 0.30); }
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
QLabel#carouselStatusLive { background: rgba(189, 172, 255, 0.16); color: %11;
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
    background: rgba(189, 172, 255, 0.08); border-top-left-radius: %21px;
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
    selection-background-color: rgba(189, 172, 255, 0.20);
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
QPushButton#catalogueApplyButton, QPushButton#teamFilterButton,
QPushButton[accentAction="true"] {
    background: %11; color: %1; border: none; font-weight: 600;
}
QPushButton#catalogueApplyButton:hover, QPushButton#teamFilterButton:hover,
QPushButton[accentAction="true"]:hover { background: %12; }
QPushButton#catalogueApplyButton:disabled, QPushButton#teamFilterButton:disabled,
QPushButton[accentAction="true"]:disabled { background: %8; color: %4; }
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
QPushButton#benchmarkRunButton {
    background: %2;
    color: %1;
    border: 1px solid %2;
    font-weight: 600;
}
QPushButton#benchmarkRunButton:hover { background: %4; color: %1; border-color: %4; }
QPushButton#benchmarkRunButton:disabled { background: %9; color: %4; border-color: %10; }
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
    background: %4; border-radius: %6px;
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

// The media detail page is the one editorial surface in the application: a
// neutral near-black bed, a steel-blue accent, wide whitespace and pill
// controls. Every rule is scoped to QWidget#titlePage so the settings-led
// palette in base() keeps driving the rest of the application. The reference
// is deliberately generous with whitespace; spacing and column widths are
// owned by TitlePage's layout, while this sheet owns colour, type scale,
// radii and control states.
QString titlePage(const Palette& p, const Appearance& a) {
    // A radius step scales every rounded corner on the page by the ratio the
    // viewer chose against the sheet's own MD default, so one control moves
    // chips, cards, posters and the details panel together.
    const double scale = radiusSmall(a) / 8.0;
    const auto scaled = [scale](int value) {
        return qMax(0, qRound(value * scale));
    };
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
    background: %2;
    color: %6;
    border: 1px solid transparent;
    border-radius: %13px;
    min-height: %12px;
    max-height: %12px;
    padding: 0 %38px;
    font-size: %27px;
    font-weight: 500;
}
QWidget#titlePage QWidget#titleHeader QPushButton:hover:enabled { border-color: %4; }
QWidget#titlePage QWidget#titleHeader QPushButton:pressed { background: %3; }
QWidget#titlePage QWidget#titleHeader QPushButton:focus { border-color: %4; }
QWidget#titlePage QWidget#titleHeader QPushButton:disabled { color: %7; }
QWidget#titlePage QPushButton#settingsChip {
    background: %2;
    border: 1px solid %8;
    border-radius: %13px;
    min-width: %12px;
    max-width: %12px;
    min-height: %12px;
    max-height: %12px;
    padding: 0;
}
QWidget#titlePage QPushButton#settingsChip:hover { border-color: %4; }
QWidget#titlePage QPushButton#settingsChip:pressed { background: %3; }
QWidget#titlePage QPushButton#settingsChip:focus { border-color: %4; }
QWidget#titlePage QLabel#status,
QWidget#titlePage QLabel#muted { color: %7; font-size: %30px; }

QWidget#titlePage QLabel#titleHeading {
    color: %6;
    font-size: %22px;
    font-weight: 800;
}
QWidget#titlePage[compact="true"] QLabel#titleHeading { font-size: %23px; }
QWidget#titlePage QLabel#titleSubtitle {
    color: %7;
    font-size: %24px;
    font-weight: 400;
}
QWidget#titlePage QWidget#titleAccentRule {
    background: %4;
    border: none;
    min-height: %21px;
    max-height: %21px;
}
QWidget#titlePage QLabel#titleStudio {
    color: %7;
    font-size: %28px;
    font-weight: 600;
}
QWidget#titlePage QLabel#titleFacts { color: %7; font-size: %27px; }
QWidget#titlePage QLabel#titleScores {
    color: %4;
    font-size: %25px;
    font-weight: 600;
}
QWidget#titlePage QLabel#titleDescription { color: %6; font-size: %25px; }
QWidget#titlePage QLabel#titleLinks { color: %4; font-size: %25px; }

QWidget#titlePage QPushButton#textAction {
    background: %2;
    color: %6;
    border: 1px solid transparent;
    border-radius: %13px;
    min-height: %12px;
    max-height: %12px;
    padding: 0 %38px;
    font-size: %27px;
    font-weight: 500;
}
QWidget#titlePage QPushButton#textAction:hover:enabled { border-color: rgba(%9, 0.60); }
QWidget#titlePage QPushButton#textAction:pressed { background: %3; }
QWidget#titlePage QPushButton#textAction:checked { background: %3; border-color: %4; color: %6; }
QWidget#titlePage QPushButton#textAction:checked:hover { border-color: %5; }
QWidget#titlePage QPushButton#textAction:focus { border-color: %4; }
QWidget#titlePage QPushButton#textAction:disabled { color: %7; border-color: transparent; }

QWidget#titlePage QWidget#titleWatchPanel {
    background: %2;
    border: 1px solid %8;
    border-radius: %14px;
}
QWidget#titlePage QLabel#titleWatchEpisode {
    color: %6;
    font-size: %31px;
    font-weight: 500;
}
QWidget#titlePage QLabel#titleWatchMeta { color: %7; font-size: %29px; }
QWidget#titlePage QLabel#titleWatchStatus { color: %7; font-size: %30px; }
QWidget#titlePage QPushButton#titleWatchButton {
    background: %4;
    color: %1;
    border: 1px solid %4;
    border-radius: %13px;
    min-height: %12px;
    max-height: %12px;
    padding: 0 %38px;
    font-size: %27px;
    font-weight: 700;
}
QWidget#titlePage QPushButton#titleWatchButton:hover:enabled { background: %5; border-color: %5; }
QWidget#titlePage QPushButton#titleWatchButton:pressed { background: %4; border-color: %4; }
QWidget#titlePage QPushButton#titleWatchButton:focus { border-color: %6; }
QWidget#titlePage QPushButton#titleWatchButton:disabled {
    background: %2;
    border-color: %8;
    color: %7;
}

QWidget#titlePage QLabel#releaseKind {
    color: %7;
    font-size: %27px;
    font-weight: 500;
}
QWidget#titlePage QWidget#releaseChipsHost { background: transparent; }
QWidget#titlePage QPushButton#releaseChip {
    background: %2;
    color: %7;
    border: 1px solid transparent;
    border-radius: %13px;
    min-height: %12px;
    max-height: %12px;
    padding: 0 %20px;
    font-size: %27px;
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

QWidget#titlePage QTabWidget#episodeTabs::pane {
    border: none;
    background: transparent;
    padding: 0;
    margin-top: 0;
}
QWidget#titlePage QTabWidget#episodeTabs QTabBar::tab {
    background: %2;
    color: %7;
    border: 1px solid transparent;
    border-radius: %13px;
    padding: %33px %34px;
    margin-right: %35px;
    font-size: %27px;
    font-weight: 500;
}
QWidget#titlePage QTabWidget#episodeTabs QTabBar::tab:selected {
    background: %3;
    color: %6;
    border: 1px solid %4;
}
QWidget#titlePage QTabWidget#episodeTabs QTabBar::tab:hover:!selected {
    color: %6;
    border-color: rgba(%9, 0.45);
}
QWidget#titlePage QTabWidget#episodeTabs QTabBar::tab:disabled { color: rgba(%10, 0.45); }

QWidget#titlePage QLabel#titleSectionHeading,
QWidget#titlePage QLabel#episodeSectionHeading {
    color: %6;
    font-size: %26px;
    font-weight: 700;
}
QWidget#titlePage QWidget#titleSectionHeader,
QWidget#titlePage QWidget#titleEpisodeHeader {
    background: transparent;
    border-bottom: 1px solid %8;
}
QWidget#titlePage QLabel#titleEpisodeCount { color: %7; font-size: %27px; }

QWidget#titlePage QScrollArea#animeCarousel { background: transparent; border: none; }
QWidget#titlePage QWidget#animeCarouselTrack { background: transparent; }
QWidget#titlePage QPushButton#carouselControl {
    background: %2;
    color: %6;
    border: 1px solid %8;
    border-radius: %19px;
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
    border-radius: %14px;
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
    font-size: %29px;
}
QWidget#titlePage QLabel#carouselKind {
    color: %7;
    font-size: %28px;
    font-weight: 600;
}
QWidget#titlePage QLabel#carouselTitle {
    color: %6;
    font-size: %27px;
    font-weight: 500;
}
QWidget#titlePage QLabel#carouselStatus {
    background: %3;
    color: %7;
    border-radius: %13px;
    padding: 2px 6px;
    font-size: %28px;
    font-weight: 600;
}
QWidget#titlePage QLabel#carouselStatusLive {
    background: rgba(%9, 0.16);
    color: %5;
    border-radius: %13px;
    padding: 2px 6px;
    font-size: %28px;
    font-weight: 700;
}
QWidget#titlePage QLabel#carouselEpisodes { color: %7; font-size: %29px; }

QWidget#titlePage QPushButton#episodeButton {
    background: %2;
    color: %6;
    border: 1px solid transparent;
    border-radius: %16px;
    min-height: %17px;
    max-height: %17px;
    padding: 0;
    font-size: %27px;
    font-weight: 500;
}
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
    border-color: transparent;
    color: rgba(%10, 0.35);
}
QWidget#titlePage QPushButton#episodeButton:checked:disabled {
    background: %2;
    border-color: transparent;
    color: rgba(%10, 0.35);
}
QWidget#titlePage QWidget#episodeWatchedMark {
    background: %4;
    border: none;
    border-radius: 1px;
}
QWidget#titlePage QWidget#titleEpisodeLegend { background: transparent; }
QWidget#titlePage QLabel#episodeLegendLabel { color: %7; font-size: %28px; }
QWidget#titlePage QWidget#episodeLegendCurrent {
    background: transparent;
    border: 1px solid %4;
    border-radius: %41px;
}
QWidget#titlePage QWidget#episodeLegendWatched {
    background: %4;
    border: none;
    border-radius: 1px;
}
QWidget#titlePage QWidget#episodeLegendUnavailable {
    background: %2;
    border: none;
    border-radius: %41px;
}
QWidget#titlePage QPushButton#titleWatchWatchedToggle {
    background: transparent;
    color: %7;
    border: 1px solid %8;
    border-radius: %13px;
    min-height: %12px;
    max-height: %12px;
    padding: 0 %38px;
    font-size: %27px;
    font-weight: 500;
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
    font-size: %27px;
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
    font-size: %32px;
    font-weight: 700;
}
QWidget#titlePage QLabel#titleDetailsTitle {
    color: %6;
    font-size: %31px;
    font-weight: 600;
}
QWidget#titlePage QWidget#titleDetailsFacts { background: transparent; }
QWidget#titlePage QLabel#titleDetailsThumb {
    background: %1;
    border: 1px solid %8;
    border-radius: %36px;
}
QWidget#titlePage QPushButton#titleDetailsClose {
    background: %2;
    color: %6;
    border: 1px solid %8;
    border-radius: %19px;
    min-width: %37px;
    max-width: %37px;
    min-height: %37px;
    max-height: %37px;
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
        .arg(kTitleControlHeight)
        .arg(scaled(kTitlePillRadiusRound))
        .arg(scaled(kTitleCardRadius))
        .arg(scaled(kTitlePosterRadius))
        .arg(scaled(kTitleEpisodeRadius))
        .arg(kTitleEpisodeButtonHeight)
        .arg(kTitleCircleControlSize)
        .arg(scaled(kTitleCircleRadius))
        .arg(kTitleChipPaddingH)
        .arg(kTitleAccentRuleHeight)
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
        .arg(scaled(kTitleLegendSwatchRadius));
}

}  // namespace

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
            .arg(p.wash, 23);
    return application + titlePage(p, appearance);
}

QString buildStylesheet() { return buildStylesheet(Appearance{}); }

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
    pal.setColor(QPalette::Button, QColor(p.surface));
    pal.setColor(QPalette::ButtonText, QColor(p.text));
    pal.setColor(QPalette::BrightText, QColor(p.accentSoftBright));
    pal.setColor(QPalette::Highlight, QColor(p.accentValue));
    pal.setColor(QPalette::HighlightedText, QColor(p.accentOnFill));
    pal.setColor(QPalette::ToolTipBase, QColor(p.surfaceHover));
    pal.setColor(QPalette::ToolTipText, QColor(p.text));
    pal.setColor(QPalette::PlaceholderText, QColor(p.muted));
    pal.setColor(QPalette::Link, QColor(p.accent));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(p.textDisabled));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(p.textDisabled));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(p.textDisabled));
    QApplication::setPalette(pal);
}

}  // namespace mikai::app::theme

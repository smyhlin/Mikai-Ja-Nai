#include "theme/Theme.hpp"

#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

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
QLabel#titleHeading { font-size: 22px; font-weight: 700; background: transparent; }
QLabel#sectionHeading { font-size: 15px; font-weight: 600; padding-top: %15px; background: transparent; }
QLabel#titleSectionHeading, QLabel#episodeSectionHeading { font-size: 15px; font-weight: 700; background: transparent; }
QLabel#muted, QLabel#status { color: %4; background: transparent; }
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
    background: rgba(255, 255, 255, 0.03);
    border-color: rgba(255, 255, 255, 0.06);
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
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid %10;
    border-radius: %21px;
    padding: 2px %3px;
    font-size: 12px;
}
QLabel#nativeComponentStatus[statusSeverity="failure"] { color: %22; font-weight: 600; }
QLabel#nativeComponentStatus[statusSeverity="success"] { color: %11; font-weight: 600; }
QLabel#nativeComponentStatus[statusSeverity="cancelled"] { color: %4; }
QProgressBar#nativeComponentTabProgress { background: %1; border: 1px solid %10;
    border-radius: %6px; min-height: 8px; max-height: 8px; }
QProgressBar#nativeComponentTabProgress::chunk { background: %11; border-radius: %6px; }
QScrollArea#titleOverview { background: transparent; border: none; }
QLabel#titleCover { background: %8; color: %4; border: 1px solid %10; border-radius: %6px; }
QLabel#titleFacts { color: %4; font-size: 13px; background: transparent; }
QLabel#titleDescription { background: transparent; color: %2; font-size: 14px; }
QLabel#titleScores { color: %11; font-weight: 600; font-size: 13px; background: transparent; }
QLabel#titleLinks { color: %11; background: transparent; font-size: 13px; }
QWidget#titleDetails { background: %5; color: %2; border: 1px solid %10; border-radius: %6px; }
QWidget#titleDetails QLabel { background: transparent; }
QPushButton#episodeButton { background: %8; border: 1px solid %10;
    border-radius: %21px; font-weight: 600; padding: 0 %15px; }
QPushButton#episodeButton:hover { background: %9; border-color: %11; color: %11; }
QPushButton#episodeButton:pressed { background: %9; }
QPushButton#episodeButton:disabled { color: %4; background: rgba(255, 255, 255, 0.02); border-color: transparent; }
QLabel#releaseKind { color: %4; font-size: 13px; font-weight: 600; background: transparent; }
QPushButton#releaseChip { background: %5; color: %2; border: 1px solid %10;
    border-radius: %6px; font-weight: 600; padding: 0 10px; }
QPushButton#releaseChip:hover:enabled { border-color: %11; background: %8; }
QPushButton#releaseChip:checked { background: rgba(189, 172, 255, 0.14); border-color: %11; color: %12; }
QPushButton#releaseChip:pressed { background: %9; }
QLineEdit, QComboBox, QSpinBox {
    background: %5; border: 1px solid %10;
    border-radius: %6px; padding: 6px %7px;
    min-height: 24px; font-size: 14px;
    selection-background-color: %11; selection-color: %1;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: %11; background: %8; }
QComboBox::drop-down { border: none; width: 24px; }
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
QPushButton#carouselControl:disabled { color: rgba(255, 255, 255, 0.22); background: rgba(255, 255, 255, 0.03); border-color: rgba(255, 255, 255, 0.06); }
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
QLabel#carouselStatus { background: rgba(255, 255, 255, 0.06); color: %4;
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
QTabWidget#episodeTabs::pane { border: 1px solid %10; border-radius: %6px; background: %5;
    margin-top: 0; padding: %15px %3px; }
QTabBar::tab { background: transparent; color: %4; border: none;
    border-bottom: 2px solid transparent; padding: 7px %18px; margin-right: 2px;
    font-weight: 600; }
QTabBar::tab:selected { color: %2; border-bottom: 2px solid %11;
    background: rgba(189, 172, 255, 0.08); border-top-left-radius: %21px;
    border-top-right-radius: %21px; }
QTabBar::tab:hover:!selected { color: %2; background: rgba(255, 255, 255, 0.03);
    border-top-left-radius: %21px; border-top-right-radius: %21px; }
QTabWidget#episodeTabs QTabBar::tab { padding: 6px %7px; margin-right: 2px; }
QTabWidget#episodeTabs QTabBar::tab:selected { background: %5; border: 1px solid %10;
    border-bottom: 2px solid %11; border-top-left-radius: %21px;
    border-top-right-radius: %21px; margin-bottom: -1px; }
QScrollArea#episodeScroll { background: transparent; border: none;
    border-bottom-left-radius: %21px; border-bottom-right-radius: %21px; }
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
QListWidget { background: %1; border: 1px solid %10; border-radius: %6px; padding: 6px; outline: none; }
QListWidget::item { min-height: %19px; padding: %3px %7px; border-radius: %6px;
    border-left: 2px solid transparent; }
QListWidget::item:hover { background: %8; }
QListWidget::item:selected { background: %9; color: %2; border-left: 2px solid %11; }
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

}  // namespace

QString buildStylesheet() {
    return base()
        .arg(QLatin1String(kBackground), 1)
        .arg(QLatin1String(kText), 2)
        .arg(kSpacingSmall, 3)
        .arg(QLatin1String(kMuted), 4)
        .arg(QLatin1String(kSurface), 5)
        .arg(kRadiusSmall, 6)
        .arg(kSpacingMedium, 7)
        .arg(QLatin1String(kSurfaceHover), 8)
        .arg(QLatin1String(kSurfaceActive), 9)
        .arg(QLatin1String(kBorder), 10)
        .arg(QLatin1String(kAccent), 11)
        .arg(QLatin1String(kAccentHover), 12)
        .arg(kSettingsControlSize, 13)
        .arg(kSecondaryControlSize, 14)
        .arg(kSpacingXs, 15)
        .arg(kMenuItemMinHeight, 16)
        .arg(kRadius, 17)
        .arg(kSpacingLarge, 18)
        .arg(kSearchRowMinHeight, 19)
        .arg(QLatin1String(kVideoBackground), 20)
        .arg(kTitlePillRadius, 21)
        .arg(QLatin1String(kDanger), 22);
}

}  // namespace mikai::app::theme

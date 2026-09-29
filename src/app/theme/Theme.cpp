#include "theme/Theme.hpp"

#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

namespace mikai::app::theme {
namespace {

QString base() {
    return QStringLiteral(R"QSS(
QWidget {
    background: %1;
    color: %2;
    font-family: "Segoe UI", system-ui, sans-serif;
    font-size: 14px;
}
QLabel#heading { font-size: 22px; font-weight: 600; }
QLabel#sectionHeading { font-size: 16px; font-weight: 600; padding-top: %3px; }
QLabel#muted, QLabel#status { color: %4; }
QLabel#descriptionPanel { background: %5; color: %2; border: none;
    border-radius: %6px; padding: %7px; }
QScrollArea#titleOverview, QScrollArea#episodeScroll { background: transparent; border: none; }
QLabel#titleCover { background: %8; color: %4; border: none; border-radius: %6px; }
QLabel#titleFacts { color: %4; font-size: 13px; }
QLabel#titleDescription { background: transparent; color: %2; font-size: 14px; }
QWidget#titleDetails { background: %5; color: %2; border: none; border-radius: %6px; }
QWidget#titleDetails QLabel { background: transparent; }
QPushButton#episodeButton { background: %5; border: 1px solid transparent;
    border-radius: %6px; font-weight: 600; }
QPushButton#episodeButton:hover { background: %8; }
QPushButton#episodeButton:pressed { background: %9; }
QPushButton#episodeButton:disabled { color: %4; background: %1; }
QLineEdit, QComboBox {
    background: %5; border: 1px solid %10;
    border-radius: %6px; padding: %3px %7px;
    min-height: 22px; selection-background-color: %11; selection-color: %1;
}
QLineEdit:focus, QComboBox:focus { border-color: %11; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView { background: %5; border: 1px solid %10;
    selection-background-color: %8; }
QPushButton {
    background: %5; border: 1px solid transparent; border-radius: %6px;
    padding: %3px %7px; min-height: 22px;
}
QPushButton:hover { background: %8; }
QPushButton:pressed { background: %9; }
QPushButton:focus { border-color: %11; }
QPushButton:disabled { color: %4; background: %5; }
QPushButton#primary { background: %11; color: %1; border: none; font-weight: 600; }
QPushButton#primary:hover { background: %12; }
QPushButton#textAction { background: transparent; color: %11; border: none;
    padding: 0 %3px 0 0; min-height: 24px; }
QPushButton#textAction:hover { color: %12; background: transparent; }
QPushButton#settingsChip { padding: 0; min-width: %13px;
    max-width: %13px; min-height: %13px; max-height: %13px; }
QPushButton#animeCard { background: %5; border: 1px solid transparent;
    border-radius: %6px; padding: 0; }
QPushButton#animeCard:hover { background: %8; }
QPushButton#animeCard:focus { border-color: %11; }
QPushButton#carouselControl { padding: 0; min-width: %14px; max-height: %14px; }
QWidget#homeFeedRow { background: %5; border: none; border-radius: %6px; }
QWidget#homeFeedRow:hover { background: %8; }
QPushButton#homeFeedTitle { text-align: left; background: transparent; border: none;
    padding: %15px; font-weight: 600; }
QLabel#homeFeedMeta { background: transparent; color: %4; font-size: 12px; }
QLabel#homeFeedPoster { background: %1; border: none; border-radius: %6px; }
QScrollArea#animeCarousel { background: transparent; border: none; }
QScrollArea#discoveryScroll { background: transparent; border: none; }
QWidget#animeCarouselTrack { background: transparent; }
QLabel#carouselPoster { background: %1; color: %4; border-radius: %6px; font-size: 12px; }
QLabel#carouselKind { background: transparent; color: %11; font-size: 12px; }
QLabel#carouselTitle { background: transparent; color: %2; font-weight: 600; }
QLabel#carouselStatus { background: transparent; color: %4; }
QLabel#carouselStatusLive { background: transparent; color: %11; }
QLabel#carouselEpisodes { background: transparent; color: %2; }
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
QPushButton#pagerControl:hover { background: %5; }
QPushButton#pagerControl:disabled { color: %4; background: transparent; }
QPushButton#pagerChip { background: transparent; color: %4;
    border: 1px solid transparent; border-radius: %17px; padding: 7px %18px; }
QPushButton#pagerChip:hover { color: %2; background: %5; }
QPushButton#pagerChipActive { background: %9; color: %2; font-weight: 600; }
QTabWidget::pane { border: none; border-top: 1px solid %10; background: %5; }
QTabBar::tab { background: transparent; color: %4; border: none;
    border-bottom: 2px solid transparent; padding: 7px %18px; margin-right: 2px; }
QTabBar::tab:selected { color: %2; border-bottom: 2px solid %11; }
QTabBar::tab:hover { color: %2; }
QListWidget { background: %1; border: none; border-radius: %6px; padding: 6px; outline: none; }
QListWidget::item { min-height: %19px; padding: %3px %7px; border-radius: %6px;
    border-left: 2px solid transparent; }
QListWidget::item:hover { background: %8; }
QListWidget::item:selected { background: %9; color: %2; border-left: 2px solid %11; }
QScrollArea { border: none; }
QScrollBar:vertical { background: %1; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: %10; border-radius: 5px; min-height: 30px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QToolTip { background: %8; color: %2; border: 1px solid %10; padding: %15px %3px; }
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
        .arg(QLatin1String(kVideoBackground), 20);
}

}  // namespace mikai::app::theme

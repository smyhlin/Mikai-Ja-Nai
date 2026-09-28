"""Design tokens and the one application-wide Qt stylesheet."""

from mikai_ui.theme.colors import (
    ACCENT,
    ACCENT_HOVER,
    BACKGROUND,
    BORDER,
    CHROME_FILL,
    CHROME_HAIRLINE,
    DANGER,
    MUTED,
    SURFACE,
    SURFACE_ACTIVE,
    SURFACE_HOVER,
    TEXT,
    VIDEO_BACKGROUND,
)
from mikai_ui.theme.metrics import (
    PLAYER_FIELD_HEIGHT,
    PLAYER_FIELD_ROW_HEIGHT,
    PILL_RADIUS,
    RADIUS,
    RADIUS_SMALL,
    SECONDARY_CONTROL_SIZE,
    SEARCH_ROW_MIN_HEIGHT,
    SETTINGS_CONTROL_SIZE,
    SPACING_LARGE,
    SPACING_MEDIUM,
    SPACING_SMALL,
    SPACING_XS,
    VOLUME_GROOVE_HEIGHT,
    VOLUME_HANDLE_SIZE,
)


def build_stylesheet() -> str:
    """Return the shared application style using the centralized tokens."""
    return f"""
    QWidget {{
        background: {BACKGROUND};
        color: {TEXT};
        font-family: "Segoe UI", system-ui, sans-serif;
        font-size: 14px;
    }}
    QLabel#heading {{ font-size: 22px; font-weight: 600; }}
    QLabel#sectionHeading {{ font-size: 16px; font-weight: 600; padding-top: {SPACING_SMALL}px; }}
    QLabel#muted, QLabel#status {{ color: {MUTED}; }}
    QLabel#descriptionPanel {{ background: {SURFACE}; color: {TEXT}; border: none;
        border-radius: {RADIUS_SMALL}px; padding: {SPACING_MEDIUM}px; }}
    QScrollArea#titleOverview, QScrollArea#episodeScroll {{ background: transparent; border: none; }}
    QLabel#titleCover {{ background: {SURFACE_HOVER}; color: {MUTED}; border: none;
        border-radius: {RADIUS_SMALL}px; }}
    QLabel#titleFacts {{ color: {MUTED}; font-size: 13px; }}
    QLabel#titleDescription {{ background: transparent; color: {TEXT}; font-size: 14px; }}
    QWidget#titleDetails {{ background: {SURFACE}; color: {TEXT}; border: none;
        border-radius: {RADIUS_SMALL}px; }}
    QWidget#titleDetails QLabel {{ background: transparent; }}
    QPushButton#episodeButton {{ background: {SURFACE}; border: 1px solid transparent;
        border-radius: {RADIUS_SMALL}px; font-weight: 600; }}
    QPushButton#episodeButton:hover {{ background: {SURFACE_HOVER}; }}
    QPushButton#episodeButton:pressed {{ background: {SURFACE_ACTIVE}; }}
    QPushButton#episodeButton:disabled {{ color: {MUTED}; background: {BACKGROUND}; }}
    QLineEdit, QComboBox {{
        background: {SURFACE}; border: 1px solid {BORDER};
        border-radius: {RADIUS_SMALL}px; padding: {SPACING_SMALL}px {SPACING_MEDIUM}px;
        min-height: 22px; selection-background-color: {ACCENT}; selection-color: {BACKGROUND};
    }}
    QLineEdit:focus, QComboBox:focus {{ border-color: {ACCENT}; }}
    QComboBox::drop-down {{ border: none; width: 24px; }}
    QComboBox QAbstractItemView {{ background: {SURFACE}; border: 1px solid {BORDER};
        selection-background-color: {SURFACE_HOVER}; }}
    QPushButton {{
        background: {SURFACE}; border: 1px solid transparent; border-radius: {RADIUS_SMALL}px;
        padding: {SPACING_SMALL}px {SPACING_MEDIUM}px; min-height: 22px;
    }}
    QPushButton:hover {{ background: {SURFACE_HOVER}; }}
    QPushButton:pressed {{ background: {SURFACE_ACTIVE}; }}
    QPushButton:focus {{ border-color: {ACCENT}; }}
    QPushButton:disabled {{ color: {MUTED}; background: {SURFACE}; }}
    QPushButton#primary {{ background: {ACCENT}; color: {BACKGROUND}; border: none;
        font-weight: 600; }}
    QPushButton#primary:hover {{ background: {ACCENT_HOVER}; }}
    QPushButton#textAction {{ background: transparent; color: {ACCENT}; border: none;
        padding: 0 {SPACING_SMALL}px 0 0; min-height: 24px; }}
    QPushButton#textAction:hover {{ color: {ACCENT_HOVER}; background: transparent; }}
    QPushButton#settingsChip {{ padding: 0; min-width: {SETTINGS_CONTROL_SIZE}px;
        max-width: {SETTINGS_CONTROL_SIZE}px; min-height: {SETTINGS_CONTROL_SIZE}px;
        max-height: {SETTINGS_CONTROL_SIZE}px; }}
    QPushButton#animeCard {{ background: {SURFACE}; border: 1px solid transparent;
        border-radius: {RADIUS_SMALL}px; padding: 0; }}
    QPushButton#animeCard:hover {{ background: {SURFACE_HOVER}; }}
    QPushButton#animeCard:focus {{ border-color: {ACCENT}; }}
    QPushButton#carouselControl {{ padding: 0; min-width: {SECONDARY_CONTROL_SIZE}px;
        max-height: {SECONDARY_CONTROL_SIZE}px; }}
    QWidget#homeFeedRow {{ background: {SURFACE}; border: none;
        border-radius: {RADIUS_SMALL}px; }}
    QWidget#homeFeedRow:hover {{ background: {SURFACE_HOVER}; }}
    QPushButton#homeFeedTitle {{ text-align: left; background: transparent; border: none;
        padding: {SPACING_XS}px; font-weight: 600; }}
    QLabel#homeFeedMeta {{ background: transparent; color: {MUTED}; font-size: 12px; }}
    QLabel#homeFeedPoster {{ background: {BACKGROUND}; border: none;
        border-radius: {RADIUS_SMALL}px; }}
    QScrollArea#animeCarousel {{ background: transparent; border: none; }}
    QScrollArea#discoveryScroll {{ background: transparent; border: none; }}
    QWidget#animeCarouselTrack {{ background: transparent; }}
    QLabel#carouselPoster {{ background: {BACKGROUND}; color: {MUTED};
        border-radius: {RADIUS_SMALL}px; font-size: 12px; }}
    QLabel#carouselKind {{ background: transparent; color: {ACCENT}; font-size: 12px; }}
    QLabel#carouselTitle {{ background: transparent; color: {TEXT}; font-weight: 600; }}
    QLabel#carouselStatus {{ background: transparent; color: {MUTED}; }}
    QLabel#carouselStatusLive {{ background: transparent; color: {ACCENT}; }}
    QLabel#carouselEpisodes {{ background: transparent; color: {TEXT}; }}
    QMenu {{ background: {SURFACE}; color: {TEXT}; border: 1px solid {BORDER};
        border-radius: {RADIUS_SMALL}px; padding: {SPACING_XS}px; }}
    QMenu::item {{ min-height: {PLAYER_FIELD_ROW_HEIGHT}px;
        padding: 0 {SPACING_MEDIUM}px; border-radius: {RADIUS_SMALL}px; }}
    QMenu::item:selected {{ background: {SURFACE_ACTIVE}; color: {TEXT}; }}
    QMenu::item:disabled {{ color: {MUTED}; }}
    QMenu::separator {{ height: 1px; background: {BORDER}; margin: {SPACING_XS}px; }}
    QMenu::section {{
        color: {MUTED}; font-size: 12px; font-weight: 600;
        padding: {SPACING_SMALL}px {SPACING_MEDIUM}px {SPACING_XS}px {SPACING_MEDIUM}px;
        border: none; }}
    QPushButton#pagerControl {{ background: transparent; color: {TEXT};
        border: 1px solid transparent; border-radius: {RADIUS}px; padding: 0;
        min-width: {SECONDARY_CONTROL_SIZE}px; min-height: {SECONDARY_CONTROL_SIZE}px; }}
    QPushButton#pagerControl:hover {{ background: {SURFACE}; }}
    QPushButton#pagerControl:disabled {{ color: {MUTED}; background: transparent; }}
    QPushButton#pagerChip {{ background: transparent; color: {MUTED};
        border: 1px solid transparent; border-radius: {RADIUS}px; padding: 7px {SPACING_LARGE}px; }}
    QPushButton#pagerChip:hover {{ color: {TEXT}; background: {SURFACE}; }}
    QPushButton#pagerChipActive {{ background: {SURFACE_ACTIVE}; color: {TEXT};
        font-weight: 600; }}
    QWidget#playerControls QLabel, QWidget#playerStatusCard QLabel,
    /* a distinct selector does not inherit the base rule's box, so restate it:
       without the padding and min-height the current row sits inset and taller */
    QSlider#volume {{ background: transparent; }}
    QSlider#volume::groove:horizontal {{ height: {VOLUME_GROOVE_HEIGHT}px;
        background: {SURFACE_ACTIVE}; border-radius: 2px; }}
    QSlider#volume::sub-page:horizontal {{ background: {MUTED}; border-radius: 2px; }}
    QSlider#volume::handle:horizontal {{ background: {TEXT};
        width: {VOLUME_HANDLE_SIZE}px; height: {VOLUME_HANDLE_SIZE}px;
        margin: {VOLUME_GROOVE_HEIGHT // 2 - VOLUME_HANDLE_SIZE // 2}px 0;
        border-radius: {VOLUME_HANDLE_SIZE // 2}px; }}
    QTabWidget::pane {{ border: none; border-top: 1px solid {BORDER};
        background: {SURFACE}; }}
    QTabBar::tab {{ background: transparent; color: {MUTED}; border: none;
        border-bottom: 2px solid transparent; padding: 7px {SPACING_LARGE}px; margin-right: 2px; }}
    QTabBar::tab:selected {{ color: {TEXT}; border-bottom: 2px solid {ACCENT}; }}
    QTabBar::tab:hover {{ color: {TEXT}; }}
    QListWidget {{ background: {BACKGROUND}; border: none;
        border-radius: {RADIUS_SMALL}px; padding: 6px; outline: none; }}
    QListWidget::item {{ min-height: {SEARCH_ROW_MIN_HEIGHT}px;
        padding: {SPACING_SMALL}px {SPACING_MEDIUM}px; border-radius: {RADIUS_SMALL}px;
        border-left: 2px solid transparent; }}
    QListWidget::item:hover {{ background: {SURFACE_HOVER}; }}
    QListWidget::item:selected {{ background: {SURFACE_ACTIVE}; color: {TEXT};
        border-left: 2px solid {ACCENT}; }}
    QScrollArea {{ border: none; }}
    QScrollBar:vertical {{ background: {BACKGROUND}; width: 10px; margin: 0; }}
    QScrollBar::handle:vertical {{ background: {BORDER}; border-radius: 5px; min-height: 30px; }}
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}
    QToolTip {{ background: {SURFACE_HOVER}; color: {TEXT}; border: 1px solid {BORDER};
        padding: {SPACING_XS}px {SPACING_SMALL}px; }}
    QDialog#playerWindow {{ background: {VIDEO_BACKGROUND}; color: {TEXT}; }}
    QWidget#playerPip {{ background: {CHROME_FILL}; border: none;

        border-radius: {PILL_RADIUS}px; }}
    QWidget#pipVideo {{ background: {VIDEO_BACKGROUND}; border: none; }}
    QWidget#pipBar {{ background: {CHROME_FILL}; border: none;

        border-bottom-left-radius: {PILL_RADIUS}px;

        border-bottom-right-radius: {PILL_RADIUS}px; }}
    QLabel#pipStatus {{ background: transparent; color: {MUTED}; font-size: 12px; }}
    QPushButton#pipPlay, QPushButton#pipClose {{ background: transparent;
        border: 1px solid transparent; border-radius: {RADIUS_SMALL}px; padding: 0;
        /* QSS height is the content box, so subtract the 1px border to land on
           the 32px the widget asks for */
        min-height: {PLAYER_FIELD_HEIGHT - 2}px; max-height: {PLAYER_FIELD_HEIGHT - 2}px; }}
    QPushButton#pipPlay:hover, QPushButton#pipClose:hover {{ background: {SURFACE_HOVER};
        border-color: {ACCENT}; }}
    QWidget#videoStage, QWidget#videoSurface {{ background: {VIDEO_BACKGROUND}; border: none; }}
    QDialog#coverViewer {{ background: {VIDEO_BACKGROUND}; }}
    QWidget#coverViewerView {{ background: transparent; border: none; }}
    """


__all__ = [
    "ACCENT",
    "ACCENT_HOVER",
    "BACKGROUND",
    "BORDER",
    "CHROME_FILL",
    "CHROME_HAIRLINE",
    "DANGER",
    "MUTED",
    "SURFACE",
    "SURFACE_ACTIVE",
    "SURFACE_HOVER",
    "SEARCH_ROW_MIN_HEIGHT",
    "TEXT",
    "VIDEO_BACKGROUND",
    "build_stylesheet",
]

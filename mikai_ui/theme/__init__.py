"""Design tokens and the one application-wide Qt stylesheet."""

from mikai_ui.theme.colors import (
    ACCENT,
    ACCENT_HOVER,
    BACKGROUND,
    BORDER,
    CHROME_FILL,
    CHROME_HAIRLINE,
    DANGER,
    GHOST_HOVER,
    GHOST_PRESSED,
    MUTED,
    SURFACE,
    SURFACE_ACTIVE,
    SURFACE_HOVER,
    TEXT,
    VIDEO_BACKGROUND,
)
from mikai_ui.theme.metrics import (
    PLAYER_FIELD_HEIGHT,
    PLAYER_FIELD_MIN_WIDTH,
    PLAYER_FIELD_ROW_HEIGHT,
    PRIMARY_CONTROL_SIZE,
    RADIUS,
    RADIUS_LARGE,
    RADIUS_SMALL,
    SECONDARY_CONTROL_SIZE,
    SEARCH_ROW_MIN_HEIGHT,
    SETTINGS_CONTROL_SIZE,
    SPACING_LARGE,
    SPACING_MEDIUM,
    SPACING_SMALL,
    SPACING_XS,
    TIMELINE_GROOVE_HEIGHT,
    TIMELINE_HANDLE_SIZE,
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
    QLabel#descriptionPanel {{ background: {SURFACE}; color: {TEXT}; border: 1px solid {BORDER};
        border-radius: {RADIUS_SMALL}px; padding: {SPACING_MEDIUM}px; }}
    QScrollArea#titleOverview, QScrollArea#episodeScroll {{ background: transparent; border: none; }}
    QLabel#titleCover {{ background: {SURFACE}; color: {MUTED}; border: 1px solid {BORDER};
        border-radius: {RADIUS_SMALL}px; }}
    QLabel#titleFacts {{ color: {MUTED}; font-size: 13px; }}
    QLabel#titleDescription {{ background: transparent; color: {TEXT}; font-size: 14px; }}
    QWidget#titleDetails {{ background: {SURFACE}; color: {TEXT}; border: 1px solid {BORDER};
        border-radius: {RADIUS_SMALL}px; }}
    QWidget#titleDetails QLabel {{ background: transparent; }}
    QWidget#episodeCard {{ background: {SURFACE}; color: {TEXT}; border: 1px solid {BORDER};
        border-radius: {RADIUS_SMALL}px; }}
    QWidget#episodeCard QLabel {{ background: transparent; }}
    QWidget#qualityHolder {{ background: transparent; border: none; }}
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
        background: {SURFACE}; border: 1px solid {BORDER}; border-radius: {RADIUS_SMALL}px;
        padding: {SPACING_SMALL}px {SPACING_MEDIUM}px; min-height: 22px;
    }}
    QPushButton:hover {{ background: {SURFACE_HOVER}; border-color: {SURFACE_ACTIVE}; }}
    QPushButton:pressed {{ background: {SURFACE_ACTIVE}; }}
    QPushButton:focus {{ border-color: {ACCENT}; }}
    QPushButton:disabled {{ color: {MUTED}; background: {SURFACE}; border-color: {BORDER}; }}
    QPushButton#primary {{ background: {ACCENT}; color: {BACKGROUND}; border: none;
        font-weight: 600; }}
    QPushButton#primary:hover {{ background: {ACCENT_HOVER}; }}
    QPushButton#textAction {{ background: transparent; color: {ACCENT}; border: none;
        padding: 0 {SPACING_SMALL}px 0 0; min-height: 24px; }}
    QPushButton#textAction:hover {{ color: {ACCENT_HOVER}; background: transparent; }}
    QPushButton#episodeCopy {{ background: {SURFACE_HOVER}; color: {TEXT};
        border-color: {BORDER}; font-weight: 600; }}
    QPushButton#episodeCopy:hover {{ border-color: {ACCENT}; }}
    QPushButton#qualityChip {{ padding: {SPACING_XS}px {SPACING_SMALL}px; min-height: 24px; }}
    QPushButton#qualityRetry {{ color: {ACCENT}; min-height: 24px; }}
    QPushButton#settingsChip {{ padding: 0; min-width: {SETTINGS_CONTROL_SIZE}px;
        max-width: {SETTINGS_CONTROL_SIZE}px; min-height: {SETTINGS_CONTROL_SIZE}px;
        max-height: {SETTINGS_CONTROL_SIZE}px; }}
    QPushButton#animeCard {{ background: {SURFACE}; border: 1px solid {BORDER};
        border-radius: {RADIUS_SMALL}px; padding: 0; }}
    QPushButton#animeCard:hover {{ background: {SURFACE_HOVER}; border-color: {ACCENT}; }}
    QPushButton#animeCard:focus {{ border-color: {ACCENT}; }}
    QPushButton#carouselControl {{ padding: 0; min-width: {SECONDARY_CONTROL_SIZE}px;
        max-height: {SECONDARY_CONTROL_SIZE}px; }}
    QWidget#homeFeedRow {{ background: {SURFACE}; border: 1px solid {BORDER};
        border-radius: {RADIUS_SMALL}px; }}
    QWidget#homeFeedRow:hover {{ background: {SURFACE_HOVER}; }}
    QPushButton#homeFeedTitle {{ text-align: left; background: transparent; border: none;
        padding: {SPACING_XS}px; font-weight: 600; }}
    QLabel#homeFeedMeta {{ background: transparent; color: {MUTED}; font-size: 12px; }}
    QLabel#homeFeedPoster {{ background: {SURFACE_HOVER}; border: 1px solid {BORDER};
        border-radius: {RADIUS_SMALL}px; }}
    QScrollArea#animeCarousel {{ background: transparent; border: none; }}
    QScrollArea#discoveryScroll {{ background: transparent; border: none; }}
    QWidget#animeCarouselTrack {{ background: transparent; }}
    QLabel#carouselPoster {{ background: {SURFACE_HOVER}; color: {MUTED};
        border-radius: {RADIUS_SMALL}px; font-size: 12px; }}
    QLabel#carouselKind {{ background: transparent; color: {ACCENT}; font-size: 12px; }}
    QLabel#carouselTitle {{ background: transparent; color: {TEXT}; font-weight: 600; }}
    QLabel#carouselStatus {{ background: transparent; color: {MUTED}; }}
    QLabel#carouselStatusLive {{ background: transparent; color: {ACCENT}; }}
    QLabel#carouselEpisodes {{ background: transparent; color: {TEXT}; }}
    QPushButton#playerPrimary {{ background: {ACCENT}; border: none; padding: 0;
        border-radius: {PRIMARY_CONTROL_SIZE // 2}px;
        min-width: {PRIMARY_CONTROL_SIZE}px; max-width: {PRIMARY_CONTROL_SIZE}px;
        min-height: {PRIMARY_CONTROL_SIZE}px; max-height: {PRIMARY_CONTROL_SIZE}px; }}
    QPushButton#playerPrimary:hover {{ background: {ACCENT_HOVER}; }}
    QPushButton#playerPrimary:pressed {{ background: {ACCENT}; }}
    QPushButton#playerPrimary:disabled {{ background: {SURFACE_HOVER}; }}
    QPushButton#playerSecondary {{ background: transparent; border: none;
        border-radius: {RADIUS_SMALL}px; padding: 0;
        min-width: {SECONDARY_CONTROL_SIZE}px; max-width: {SECONDARY_CONTROL_SIZE}px;
        min-height: {SECONDARY_CONTROL_SIZE}px; max-height: {SECONDARY_CONTROL_SIZE}px; }}
    QPushButton#playerSecondary:hover {{ background: {GHOST_HOVER}; }}
    QPushButton#playerSecondary:disabled {{ background: transparent; }}
    QPushButton#playerGhost {{ background: transparent; color: {MUTED}; border: none;
        padding: 0 {SPACING_MEDIUM}px; }}
    QPushButton#playerGhost:hover {{ color: {TEXT}; }}
    QComboBox#playerField {{ background: transparent; border: none;
        color: {MUTED}; padding: 0 {SPACING_SMALL}px;
        min-height: {PLAYER_FIELD_HEIGHT}px; max-height: {PLAYER_FIELD_HEIGHT}px;
        min-width: {PLAYER_FIELD_MIN_WIDTH}px; }}
    QComboBox#playerField:hover {{ color: {TEXT}; background: {GHOST_HOVER};
        border-radius: {RADIUS_SMALL}px; }}
    QComboBox#playerField:disabled {{ color: {MUTED}; background: transparent; }}
    QComboBox#playerField::drop-down {{ border: none; width: {SPACING_LARGE}px; }}
    QComboBox#playerField QAbstractItemView {{ background: {SURFACE}; color: {TEXT};
        border: 1px solid {BORDER}; border-radius: {RADIUS_SMALL}px;
        padding: {SPACING_XS}px; outline: none;
        selection-background-color: {SURFACE_HOVER}; }}
    QComboBox#playerField QAbstractItemView::item {{ height: {PLAYER_FIELD_ROW_HEIGHT}px;
        padding: 0 {SPACING_MEDIUM}px; border-radius: {RADIUS_SMALL}px; }}
    QLabel#playerTime {{ background: transparent; color: {TEXT}; font-size: 13px;
        font-variant-numeric: tabular-nums; }}
    QPushButton#pagerControl {{ background: {SURFACE_HOVER}; color: {TEXT};
        border: 1px solid {BORDER}; border-radius: {RADIUS}px; padding: 0;
        min-width: {SECONDARY_CONTROL_SIZE}px; min-height: {SECONDARY_CONTROL_SIZE}px; }}
    QPushButton#pagerControl:hover {{ border-color: {ACCENT}; }}
    QPushButton#pagerControl:disabled {{ color: {MUTED}; }}
    QPushButton#pagerChip {{ background: {SURFACE_HOVER}; color: {MUTED};
        border: 1px solid {BORDER}; border-radius: {RADIUS}px; padding: 7px {SPACING_LARGE}px; }}
    QPushButton#pagerChip:hover {{ color: {TEXT}; }}
    QPushButton#pagerChipActive {{ background: {ACCENT}; color: {BACKGROUND};
        border-color: {ACCENT}; font-weight: 600; }}
    QPushButton#playerSecondary:pressed {{ background: {GHOST_PRESSED}; }}
    QWidget#playerControls {{ background: {CHROME_FILL}; color: {TEXT};
        border: none; border-top: 1px solid {CHROME_HAIRLINE}; }}
    QWidget#playerStatusCard {{ background: {CHROME_FILL}; color: {TEXT};
        border: 1px solid {BORDER}; border-radius: {RADIUS_LARGE}px; }}
    QLabel#playerStatusTitle {{ font-size: 17px; font-weight: 600; }}
    QWidget#playerControls QLabel, QWidget#playerStatusCard QLabel,
    QWidget#playerHeader QLabel {{ background: transparent; }}
    QLabel#playerToast {{ background: {CHROME_FILL}; color: {TEXT};
        border: 1px solid {BORDER}; border-radius: {RADIUS}px;
        padding: {SPACING_XS}px {SPACING_LARGE}px; font-size: 13px; }}
    QLabel#playerStreamChip {{ background: {CHROME_FILL}; color: {TEXT};
        border: 1px solid {BORDER}; border-radius: {RADIUS_SMALL}px;
        padding: {SPACING_XS}px {SPACING_SMALL}px; font-size: 12px; }}
    QSlider#timeline {{ background: transparent; }}
    QSlider#timeline::groove:horizontal {{ height: {TIMELINE_GROOVE_HEIGHT}px;
        background: {SURFACE_ACTIVE}; border-radius: 2px; }}
    QSlider#timeline::sub-page:horizontal {{ background: {ACCENT}; border-radius: 2px; }}
    QSlider#timeline::handle:horizontal {{ background: {TEXT};
        width: {TIMELINE_HANDLE_SIZE}px; height: {TIMELINE_HANDLE_SIZE}px;
        margin: {TIMELINE_GROOVE_HEIGHT // 2 - TIMELINE_HANDLE_SIZE // 2}px 0;
        border-radius: {TIMELINE_HANDLE_SIZE // 2}px; }}
    QSlider#timeline::handle:horizontal:hover {{ background: {ACCENT_HOVER}; }}
    QSlider#volume {{ background: transparent; }}
    QSlider#volume::groove:horizontal {{ height: {VOLUME_GROOVE_HEIGHT}px;
        background: {SURFACE_ACTIVE}; border-radius: 2px; }}
    QSlider#volume::sub-page:horizontal {{ background: {MUTED}; border-radius: 2px; }}
    QSlider#volume::handle:horizontal {{ background: {TEXT};
        width: {VOLUME_HANDLE_SIZE}px; height: {VOLUME_HANDLE_SIZE}px;
        margin: {VOLUME_GROOVE_HEIGHT // 2 - VOLUME_HANDLE_SIZE // 2}px 0;
        border-radius: {VOLUME_HANDLE_SIZE // 2}px; }}
    QProgressBar#playerSpinner {{ background: {SURFACE_ACTIVE}; border: none;
        border-radius: 2px; }}
    QProgressBar#playerSpinner::chunk {{ background: {ACCENT}; border-radius: 2px; }}
    QTabWidget::pane {{ border: 1px solid {BORDER}; border-radius: {RADIUS_SMALL}px;
        background: {SURFACE}; top: -1px; }}
    QTabBar::tab {{ background: {BACKGROUND}; color: {MUTED}; border: 1px solid {BORDER};
        border-bottom: none; border-top-left-radius: {RADIUS_SMALL}px;
        border-top-right-radius: {RADIUS_SMALL}px; padding: 7px {SPACING_LARGE}px; margin-right: 2px; }}
    QTabBar::tab:selected {{ background: {SURFACE}; color: {TEXT}; }}
    QTabBar::tab:hover {{ color: {TEXT}; }}
    QListWidget {{ background: {SURFACE}; border: 1px solid {BORDER};
        border-radius: {RADIUS_SMALL}px; padding: 6px; outline: none; }}
    QListWidget::item {{ min-height: {SEARCH_ROW_MIN_HEIGHT}px; padding: {SPACING_SMALL}px {SPACING_MEDIUM}px; border-radius: {RADIUS_SMALL}px; }}
    QListWidget::item:hover {{ background: {SURFACE_HOVER}; }}
    QListWidget::item:selected {{ background: {ACCENT}; color: {BACKGROUND}; }}
    QScrollArea {{ border: none; }}
    QScrollBar:vertical {{ background: {BACKGROUND}; width: 10px; margin: 0; }}
    QScrollBar::handle:vertical {{ background: {BORDER}; border-radius: 5px; min-height: 30px; }}
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}
    QToolTip {{ background: {SURFACE_HOVER}; color: {TEXT}; border: 1px solid {BORDER};
        padding: {SPACING_XS}px {SPACING_SMALL}px; }}
    QDialog#playerWindow {{ background: {VIDEO_BACKGROUND}; color: {TEXT}; }}
    QWidget#playerHeader {{ background: {CHROME_FILL}; color: {TEXT};
        border-bottom: 1px solid {CHROME_HAIRLINE}; }}
    QLabel#playerTitle {{ font-size: 15px; font-weight: 600; }}
    QWidget#videoStage, QWidget#videoSurface {{ background: {VIDEO_BACKGROUND}; border: none; }}
    QWidget#playerOverlay {{ background: transparent; border: none; }}
    QDialog#coverViewer {{ background: {VIDEO_BACKGROUND}; }}
    QWidget#coverViewerBar {{ background: {CHROME_FILL}; color: {TEXT};
        border-top: 1px solid {CHROME_HAIRLINE}; }}
    QWidget#coverViewerView {{ background: {VIDEO_BACKGROUND}; border: none; }}
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

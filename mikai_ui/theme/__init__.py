"""Design tokens and the one application-wide Qt stylesheet."""

from mikai_ui.theme.colors import (
    ACCENT,
    ACCENT_HOVER,
    BACKGROUND,
    BG,
    BORDER,
    CHROME_BACKGROUND,
    CHROME_BG,
    CHROME_BORDER,
    DANGER,
    ELEVATED,
    FG,
    MUTED,
    SURFACE,
    SURFACE_ACTIVE,
    SURFACE_HOVER,
    TEXT,
    VIDEO_BACKGROUND,
    VIDEO_BG,
)
from mikai_ui.theme.metrics import (
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
    TIMELINE_HANDLE_SIZE,
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
        max-width: {SECONDARY_CONTROL_SIZE}px; min-height: {SECONDARY_CONTROL_SIZE}px;
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
    QPushButton#playerPrimary {{ background: {ACCENT}; border: none; border-radius: 22px;
        min-width: {PRIMARY_CONTROL_SIZE}px; max-width: {PRIMARY_CONTROL_SIZE}px;
        min-height: {PRIMARY_CONTROL_SIZE}px; max-height: {PRIMARY_CONTROL_SIZE}px; }}
    QPushButton#playerPrimary:hover {{ background: {ACCENT_HOVER}; }}
    QPushButton#playerPrimary:pressed {{ background: {ACCENT}; }}
    QPushButton#playerPrimary:disabled {{ background: {SURFACE_HOVER}; }}
    QPushButton#playerSecondary {{ padding: 0; min-width: {SECONDARY_CONTROL_SIZE}px;
        max-width: {SECONDARY_CONTROL_SIZE}px; min-height: {SECONDARY_CONTROL_SIZE}px;
        max-height: {SECONDARY_CONTROL_SIZE}px; }}
    QPushButton#playerSecondary:pressed {{ background: {SURFACE_ACTIVE}; }}
    QWidget#playerControls {{ background: {CHROME_BACKGROUND}; color: {TEXT};
        border: 1px solid {CHROME_BORDER}; border-radius: {RADIUS}px; }}
    QWidget#playerStatusCard {{ background: {CHROME_BACKGROUND}; color: {TEXT};
        border: 1px solid {CHROME_BORDER}; border-radius: {RADIUS_LARGE}px; }}
    QLabel#playerStatusTitle {{ font-size: 18px; font-weight: 600; }}
    QLabel#playerToast {{ background: {CHROME_BACKGROUND}; color: {TEXT};
        border: 1px solid {CHROME_BORDER}; border-radius: {RADIUS}px;
        padding: {SPACING_SMALL}px {SPACING_LARGE}px; font-weight: 600; }}
    QSlider::groove:horizontal {{ height: 5px; background: {BORDER}; border-radius: 3px; }}
    QSlider::sub-page:horizontal {{ background: {ACCENT}; border-radius: 3px; }}
    QSlider::handle:horizontal {{ background: {TEXT}; width: {TIMELINE_HANDLE_SIZE}px;
        height: {TIMELINE_HANDLE_SIZE}px; margin: -5px 0; border-radius: 7px; }}
    QSlider::handle:horizontal:hover {{ background: {ACCENT_HOVER}; }}
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
    QWidget#playerHeader {{ background: {SURFACE}; color: {TEXT}; border-bottom: 1px solid {BORDER}; }}
    QWidget#videoStage, QWidget#videoSurface {{ background: {VIDEO_BACKGROUND}; border: none; }}
    QWidget#playerOverlay {{ background: transparent; border: none; }}
    QDialog#coverViewer {{ background: {VIDEO_BACKGROUND}; }}
    QWidget#coverViewerBar {{ background: {CHROME_BACKGROUND}; color: {TEXT};
        border-top: 1px solid {CHROME_BORDER}; }}
    QWidget#coverViewerView {{ background: {VIDEO_BACKGROUND}; border: none; }}
    """


__all__ = [
    "ACCENT",
    "ACCENT_HOVER",
    "BACKGROUND",
    "BG",
    "BORDER",
    "CHROME_BACKGROUND",
    "CHROME_BG",
    "CHROME_BORDER",
    "DANGER",
    "ELEVATED",
    "FG",
    "MUTED",
    "SURFACE",
    "SURFACE_ACTIVE",
    "SURFACE_HOVER",
    "SEARCH_ROW_MIN_HEIGHT",
    "TEXT",
    "VIDEO_BACKGROUND",
    "VIDEO_BG",
    "build_stylesheet",
]

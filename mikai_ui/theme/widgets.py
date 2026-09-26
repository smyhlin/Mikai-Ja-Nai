"""Shared vector icon rendering for native Qt controls."""

from __future__ import annotations

from PySide6.QtCore import QByteArray, QSize, Qt
from PySide6.QtGui import QIcon, QPainter, QPixmap
from PySide6.QtSvg import QSvgRenderer

from mikai_ui.theme.colors import TEXT

_ICONS = {
    "play": '<polygon points="6 3 20 12 6 21 6 3"/>',
    "pause": (
        '<rect x="14" y="4" width="4" height="16" rx="1"/>'
        '<rect x="6" y="4" width="4" height="16" rx="1"/>'
    ),
    "stop": '<rect width="18" height="18" x="3" y="3" rx="2"/>',
    "back": ('<path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/>'),
    "forward": (
        '<path d="M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/>'
    ),
    "volume": (
        '<polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/>'
        '<path d="M15.54 8.46a5 5 0 0 1 0 7.07"/>'
        '<path d="M19.07 4.93a10 10 0 0 1 0 14.14"/>'
    ),
    "volume_muted": (
        '<polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/>'
        '<line x1="22" x2="16" y1="9" y2="15"/>'
        '<line x1="16" x2="22" y1="9" y2="15"/>'
    ),
    "fullscreen": (
        '<polyline points="15 3 21 3 21 9"/><polyline points="9 21 3 21 3 15"/>'
        '<line x1="21" x2="14" y1="3" y2="10"/>'
        '<line x1="3" x2="10" y1="21" y2="14"/>'
    ),
    "windowed": (
        '<polyline points="4 14 10 14 10 20"/><polyline points="20 10 14 10 14 4"/>'
        '<line x1="14" x2="21" y1="10" y2="3"/>'
        '<line x1="3" x2="10" y1="21" y2="14"/>'
    ),
    "gear": (
        '<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0'
        "l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0"
        " 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0"
        "l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2"
        " 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2"
        " 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2"
        ' 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/>'
        '<circle cx="12" cy="12" r="3"/>'
    ),
    "replay": '<path d="M3 12a9 9 0 1 0 2.6-6.4L3 8"/><path d="M3 3v5h5"/>',
    "zoom-in": (
        '<circle cx="11" cy="11" r="8"/>'
        '<line x1="21" x2="16.65" y1="21" y2="16.65"/>'
        '<line x1="11" x2="11" y1="8" y2="14"/>'
        '<line x1="8" x2="14" y1="11" y2="11"/>'
    ),
    "zoom-out": (
        '<circle cx="11" cy="11" r="8"/>'
        '<line x1="21" x2="16.65" y1="21" y2="16.65"/>'
        '<line x1="8" x2="14" y1="11" y2="11"/>'
    ),
    "close": '<line x1="18" x2="6" y1="6" y2="18"/><line x1="6" x2="18" y1="6" y2="18"/>',
}


def make_icon(name: str, color: str = TEXT, size: int = 18) -> QIcon:
    """Render a small scalable-feeling SVG glyph into a HiDPI-aware Qt icon."""
    markup = (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" '
        f'viewBox="0 0 24 24" fill="none" stroke="{color}" stroke-width="2" '
        f'stroke-linecap="round" stroke-linejoin="round">{_ICONS[name]}</svg>'
    )
    renderer = QSvgRenderer(QByteArray(markup.encode("utf-8")))
    pixmap = QPixmap(QSize(size, size))
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    renderer.render(painter)
    painter.end()
    return QIcon(pixmap)

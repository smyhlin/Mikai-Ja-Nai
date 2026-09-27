"""Honest clipboard writes: verify, retry once, let callers report the truth."""

from __future__ import annotations

import logging

from PySide6.QtGui import QGuiApplication

log = logging.getLogger(__name__)


def copy_text(text: str) -> bool:
    """Copy ``text`` and verify it actually landed on the system clipboard.

    Windows COM can refuse an OLE clipboard write (CO_E_NOTINITIALIZED) and the
    failure is silent: the board keeps its previous content while every caller
    reports "copied". Read the value back, retry once, and let callers say what
    really happened. A board that already holds the wanted text counts as done.
    """
    clipboard = QGuiApplication.clipboard()
    for _attempt in (0, 1):
        clipboard.setText(text)
        if clipboard.text() == text:
            return True
    log.warning("clipboard write did not stick: %r", text[:80])
    return False

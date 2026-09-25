"""Small server quota indicator shared by the catalog and title pages."""

from __future__ import annotations

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QLabel, QWidget

from mikai_backend import AnimeRepository


class RateLimitLabel(QLabel):
    def __init__(self, repository: AnimeRepository, parent: QWidget | None = None) -> None:
        super().__init__("RPM —", parent)
        self._repository = repository
        self.setObjectName("muted")

        self._timer = QTimer(self)
        self._timer.setInterval(1000)
        self._timer.timeout.connect(self.refresh)
        self._timer.start()
        self.refresh()

    def refresh(self) -> None:
        snapshot_fn = getattr(self._repository, "rate_limit_state", None)
        if snapshot_fn is None:
            self.setText("RPM —")
            self.setToolTip("Стан ліміту недоступний для цього джерела даних")
            return
        try:
            state = snapshot_fn()
        except Exception:  # noqa: BLE001 - quota display must not interrupt browsing.
            return

        limit = state.get("limit")
        used = state.get("used")
        remaining = state.get("remaining")
        queued = int(state.get("queued") or 0)
        if used is None or limit is None:
            self.setText(f"RPM —/{limit or '—'}")
            detail = "Ще немає відповіді із заголовками ліміту"
        else:
            self.setText(f"RPM {used}/{limit}")
            detail = (
                f"Остання відповідь API: використано {used} із {limit} запитів за поточне вікно"
            )
            if remaining is not None:
                detail += f"; доступно {remaining}"
            reset = int(state.get("reset_in") or 0)
            if reset:
                detail += f"; оновлення через {reset} с"
        if queued:
            detail += f"; у черзі: {queued} запитів"
        self.setToolTip(detail)

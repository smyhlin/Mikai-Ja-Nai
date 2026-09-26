from __future__ import annotations

from typing import Any

from PySide6.QtGui import QIcon
from PySide6.QtCore import QSettings, QTimer
from PySide6.QtWidgets import (
    QCheckBox,
    QDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from mikai_backend import AnimeRepository
from mikai_ui import feeds
from mikai_ui.theme import ACCENT, FG
from mikai_ui.theme.widgets import make_icon
from mikai_ui.workers import Runner


def make_gear_icon(size: int = 18, color: str = FG) -> QIcon:
    return make_icon("gear", color=color, size=size)


class SettingsDialog(QDialog):
    def __init__(
        self,
        repository: AnimeRepository,
        runner: Runner,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._repository = repository
        self._runner = runner
        self._settings = QSettings("Mikai", "Mikai Wrapper")

        self.setWindowTitle("Налаштування")
        self.setMinimumWidth(480)

        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        header_row = QHBoxLayout()
        icon_label = QLabel()
        icon_label.setPixmap(make_gear_icon(22, ACCENT).pixmap(22, 22))
        heading = QLabel("Токен доступу Mikai API")
        heading.setObjectName("heading")
        header_row.addWidget(icon_label)
        header_row.addWidget(heading)
        header_row.addStretch()
        root.addLayout(header_row)

        desc = QLabel(
            "Ключ доступу підіймає ліміт з 60 до 300 запитів на хвилину і зберігається локально в .env файлі."
        )
        desc.setObjectName("muted")
        desc.setWordWrap(True)
        root.addWidget(desc)

        link = QLabel(
            f'<a style="color: {ACCENT}; text-decoration: none;" href="https://mikai.me/user/developers">Отримати ключ у кабінеті розробника на mikai.me ↗</a>'
        )
        link.setOpenExternalLinks(True)
        root.addWidget(link)

        self._token_input = QLineEdit()
        self._token_input.setPlaceholderText("mk_... (залиште порожнім для анонімного доступу)")
        self._token_input.setClearButtonEnabled(True)
        current_key = self._repository.get_api_key()
        if current_key:
            self._token_input.setText(current_key)
        root.addWidget(self._token_input)

        quota_title = QLabel("Використання API")
        quota_title.setObjectName("sectionHeading")
        root.addWidget(quota_title)
        self._quota = QLabel("")
        self._quota.setObjectName("descriptionPanel")
        self._quota.setWordWrap(True)
        root.addWidget(self._quota)
        self._quota_timer = QTimer(self)
        self._quota_timer.setInterval(1000)
        self._quota_timer.timeout.connect(self._refresh_quota)
        self._quota_timer.start()
        self._refresh_quota()

        feeds_title = QLabel("Блоки головної сторінки")
        feeds_title.setObjectName("sectionHeading")
        root.addWidget(feeds_title)
        feeds_hint = QLabel(
            "Вимкнений блок не завантажується і не витрачає запити API. Пошук і сторінка "
            "тайтлу працюють завжди."
        )
        feeds_hint.setObjectName("muted")
        feeds_hint.setWordWrap(True)
        root.addWidget(feeds_hint)
        self._feed_boxes: dict[str, QCheckBox] = {}
        current_feeds = feeds.enabled(self._settings)
        for key in feeds.ALL_FEEDS:
            box = QCheckBox(feeds.FEED_LABELS[key])
            box.setChecked(key in current_feeds)
            root.addWidget(box)
            self._feed_boxes[key] = box

        self._status = QLabel("")
        self._status.setObjectName("status")
        self._status.setWordWrap(True)
        root.addWidget(self._status)

        buttons_row = QHBoxLayout()
        self._test_btn = QPushButton("Перевірити")
        self._test_btn.clicked.connect(self._test_token)
        buttons_row.addWidget(self._test_btn)
        buttons_row.addStretch()

        self._cancel_btn = QPushButton("Скасувати")
        self._cancel_btn.clicked.connect(self.reject)
        self._save_btn = QPushButton("Зберегти")
        self._save_btn.setObjectName("primary")
        self._save_btn.clicked.connect(self._save)

        buttons_row.addWidget(self._cancel_btn)
        buttons_row.addWidget(self._save_btn)
        root.addLayout(buttons_row)

    def _test_token(self) -> None:
        token = self._token_input.text().strip()
        self._status.setText("Перевірка токена...")
        self._test_btn.setEnabled(False)

        def on_result(result: dict[str, Any]) -> None:
            self._test_btn.setEnabled(True)
            if result.get("valid"):
                auth = result.get("authenticated")
                rate = result.get("rate_limit", 60)
                name = result.get("name")
                if auth and name:
                    self._status.setText(f"✓ Ключ дійсний (користувач: {name}, ліміт: {rate}/хв)")
                elif auth:
                    self._status.setText(f"✓ Ключ дійсний (ліміт: {rate}/хв)")
                else:
                    self._status.setText(f"Анонімний доступ (ліміт: {rate}/хв)")
            else:
                err = result.get("error", "Недійсний токен")
                self._status.setText(f"❌ Помилка: {err}")

        def on_error(err: str) -> None:
            self._test_btn.setEnabled(True)
            self._status.setText(f"❌ Помилка з'єднання: {err}")

        self._runner.run(
            lambda: self._repository.validate_api_key(token),
            on_result,
            on_error,
        )

    def _refresh_quota(self) -> None:
        snapshot_fn = getattr(self._repository, "rate_limit_state", None)
        if snapshot_fn is None:
            self._quota.setText("Стан ліміту недоступний для цього джерела даних.")
            return
        try:
            state = snapshot_fn()
        except Exception:  # noqa: BLE001 - settings must remain usable if stats fail.
            return
        limit = state.get("limit") or "—"
        remaining = state.get("remaining")
        used = state.get("used")
        reset_in = int(state.get("reset_in") or 0)
        queued = int(state.get("queued") or 0)
        in_flight = int(state.get("in_flight") or 0)
        if used is None or remaining is None:
            count = f"Ліміт: {limit} запитів/хв · лічильник очікує наступну відповідь API"
        else:
            count = f"За вікно: використано {used}, доступно {remaining} із {limit} запитів"
            if reset_in:
                count += f" · оновлення через {reset_in} с"
        count += f"\nАктивні: {in_flight} · у черзі: {queued}"
        if queued:
            count += " · запити продовжаться автоматично після оновлення ліміту"
        self._quota.setText(count)

    def _save(self) -> None:
        token = self._token_input.text().strip() or None
        self._repository.set_api_key(token)
        chosen = {key for key, box in self._feed_boxes.items() if box.isChecked()}
        feeds.save(self._settings, chosen)
        self.accept()

import os
import time

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest
from PySide6.QtCore import QObject, Signal
from PySide6.QtWidgets import QApplication, QPushButton, QScrollArea


def tab_chips(title, index: int | None = None) -> list:
    tab = title._tabs.currentWidget() if index is None else title._tabs.widget(index)
    scroll = tab if isinstance(tab, QScrollArea) else tab.findChild(QScrollArea)
    layout = scroll.widget().layout()
    return [
        layout.itemAt(i).widget()
        for i in range(layout.count())
        if layout.itemAt(i).widget() is not None
    ]


def pagination_controls(title, index: int | None = None):
    """Prev/next arrows plus the range chips of a release's episode pager. The chips
    come from the layout, not findChildren: replaced chips are deleted on the next
    event-loop pass, so a search would see two generations at once."""
    tab_index = title._tabs.currentIndex() if index is None else index
    state = title._tab_states[tab_index]
    layout = state["chips_layout"]
    chips = [layout.itemAt(i).widget() for i in range(layout.count())]
    return state["prev_btn"], chips, state["next_btn"]


def episode_button(chip) -> QPushButton:
    for button in chip.findChildren(QPushButton):
        if button.text().startswith("Серія"):
            return button
    raise AssertionError("episode button missing")


def quality_buttons(chip) -> list[QPushButton]:
    return [
        button
        for button in chip.findChildren(QPushButton)
        if button.text().endswith("p") and not button.text().startswith("Серія")
    ]


from mikai_backend import MikaiClient
from mikai_ui.main import MainWindow


class FakeMpvAdapter(QObject):
    positionChanged = Signal(float)
    durationChanged = Signal(float)
    stateChanged = Signal(str)
    bufferingChanged = Signal(bool)
    mediaEnded = Signal()
    errorOccurred = Signal(str)
    mediaLoaded = Signal()
    volumeChanged = Signal(float)
    muteChanged = Signal(bool)
    speedChanged = Signal(float)

    def __init__(self) -> None:
        super().__init__()
        self.loads: list[str] = []
        self.seeks: list[float] = []
        self.volumes: list[int] = []
        self.mutes: list[bool] = []
        self.speeds: list[float] = []
        self.pauses: list[bool] = []
        self.closed = False

    def load(self, url: str) -> None:
        self.loads.append(url)

    def play(self) -> None:
        self.pauses.append(False)

    def set_paused(self, paused: bool) -> None:
        self.pauses.append(paused)

    def toggle_pause(self) -> None:
        self.set_paused(not self.pauses[-1] if self.pauses else True)

    def stop(self) -> None:
        self.stateChanged.emit("stopped")

    def seek(self, position: float) -> None:
        self.seeks.append(position)

    def set_volume(self, volume: int) -> None:
        self.volumes.append(volume)

    def set_muted(self, muted: bool) -> None:
        self.mutes.append(muted)

    def set_speed(self, speed: float) -> None:
        self.speeds.append(speed)

    def close(self) -> None:
        self.closed = True


def pump_until(app: QApplication, cond, timeout: float = 25.0) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        app.processEvents()
        if cond():
            return True
        time.sleep(0.01)
    return False


@pytest.fixture(autouse=True)
def isolated_settings(tmp_path, monkeypatch):
    """MainWindow and TitlePage both build QSettings("Mikai", "Mikai Wrapper"); keep every
    test off the user's real store. Qt creates a QSettings internally before user code
    runs, so setDefaultFormat cannot switch it after the fact — patch the symbols."""
    from PySide6.QtCore import QSettings

    import mikai_ui.main as main_module
    import mikai_ui.settings_dialog as settings_module
    import mikai_ui.title_page as title_page_module

    store = tmp_path / "settings.ini"

    def isolated_qsettings(*args, **kwargs):
        return QSettings(str(store), QSettings.Format.IniFormat)

    monkeypatch.setattr(main_module, "QSettings", isolated_qsettings)
    monkeypatch.setattr(title_page_module, "QSettings", isolated_qsettings)
    monkeypatch.setattr(settings_module, "QSettings", isolated_qsettings)
    yield store


@pytest.fixture(scope="session")
def app():
    yield QApplication.instance() or QApplication([])


@pytest.fixture
def window(app):
    win = MainWindow(MikaiClient())
    win.show()
    app.processEvents()
    yield win
    win._runner.wait_for_done(10000)
    win.close()
    win.deleteLater()
    app.processEvents()

import os
import time

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest
from PySide6.QtCore import QEvent, QObject, Signal
from PySide6.QtWidgets import QApplication, QScrollArea


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
    """Prev/next arrows plus the range chips from a release's episode pager."""
    tab_index = title._tabs.currentIndex() if index is None else index
    pager = title._tab_states[tab_index]["pager"]
    layout = pager._chip_layout
    chips = [layout.itemAt(i).widget() for i in range(layout.count())]
    return pager.prev_btn, chips, pager.next_btn


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
    streamInfoChanged = Signal(object)

    def __init__(self) -> None:
        super().__init__()
        self.loads: list[str] = []
        self.seeks: list[float] = []
        self.volumes: list[int] = []
        self.mutes: list[bool] = []
        self.speeds: list[float] = []
        self.pauses: list[bool] = []
        self.closed = False
        self.pauses: list[bool] = []
        self.canvases: list[tuple[float, float, float]] = []
        self.closed = False
        self.stream: dict = {}

    def load(self, url: str) -> None:
        self.loads.append(url)

    def play(self) -> None:
        self.pauses.append(False)

    def set_paused(self, paused: bool) -> None:
        self.pauses.append(paused)

    def toggle_pause(self) -> None:
        self.set_paused(not self.pauses[-1] if self.pauses else True)

    def emit_stream(self, **fields) -> None:
        self.stream.update(fields)
        self.streamInfoChanged.emit(dict(self.stream))

    def seek(self, position: float) -> None:
        self.seeks.append(position)

    def set_volume(self, volume: int) -> None:
        self.volumes.append(volume)

    def set_muted(self, muted: bool) -> None:
        self.mutes.append(muted)

    def set_speed(self, speed: float) -> None:
        self.speeds.append(speed)

    def set_canvas(self, zoom: float, pan_x: float, pan_y: float) -> None:
        self.canvases.append((zoom, pan_x, pan_y))

    def close(self) -> None:
        self.closed = True


from mikai_backend import MikaiClient
from mikai_ui.main import MainWindow


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


@pytest.fixture(autouse=True)
def reap_unclosed_players():
    """Close players a test opened and never closed.

    A ShellPlayerDialog owns native panels and two Qt.Popup top-levels. Left
    alive they outlive the session QApplication, and the interpreter tears them
    down in an arbitrary order: it dies AFTER pytest has printed its success
    line, leaving only Qt's "<cannot get C stack on this system>". The live
    tests open one per MainWindow, so this has to be global rather than scoped
    to the shell player's own module.
    """
    yield
    app = QApplication.instance()
    if app is None:
        return
    from mikai_ui.shell_player import ShellPlayerDialog

    stale = [w for w in app.topLevelWidgets() if isinstance(w, ShellPlayerDialog)]
    for widget in stale:
        widget.close()
        widget.deleteLater()
    if stale:
        # deleteLater only queues; without this the dialog and its native
        # panels are still alive when the session QApplication goes
        app.sendPostedEvents(None, QEvent.Type.DeferredDelete)


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

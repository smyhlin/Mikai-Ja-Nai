import logging
import os
import time

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest
from PySide6.QtWidgets import QApplication, QScrollArea

log = logging.getLogger(__name__)


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


_live_players: list = []


def live_players() -> list:
    """Every PlayerProcess built since the session started, oldest first."""
    return list(_live_players)


@pytest.fixture(autouse=True)
def _track_players(monkeypatch):
    """Record every PlayerProcess a test builds, so teardown can kill it."""
    from mikai_ui import player_process

    original = player_process.PlayerProcess.__init__

    def tracked(self, *args, **kwargs):
        original(self, *args, **kwargs)
        _live_players.append(self)

    monkeypatch.setattr(player_process.PlayerProcess, "__init__", tracked)


@pytest.fixture(autouse=True)
def reap_unclosed_players():
    """Close players a test opened and never closed.

    The live tests open a player per MainWindow, and a leaked player is a child
    process still showing a window after pytest has printed its result.
    """
    yield
    # A player is a child process now, so the thing to reap is the process, not
    # a widget: a leaked one outlives pytest and keeps a window on screen after
    # the run has printed its result.
    for player in list(_live_players):
        try:
            player.close()
        except Exception:  # noqa: BLE001 - teardown must not mask the test result
            log.warning("could not close a leaked player", exc_info=True)
    _live_players.clear()


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

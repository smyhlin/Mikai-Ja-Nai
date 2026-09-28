"""Live end-to-end test of the player process boundary.

The player is a separate executable driven over JSON on stdin/stdout, because
PySide6 and the C++ Qt are separate builds whose Qt6Core.dll collides by DLL
name. Nothing else in the suite crosses that boundary: the hermetic tests stop
at the title page and the live tests never open a player.

What this pins is the part most likely to rot silently - spawn, config parse,
command round trip and clean exit - by asserting the child exits normally after
the host closes it. A player that starts but cannot talk is exactly the failure
the boundary introduces, and it looks like "the window is just black".
"""

from __future__ import annotations

import pytest
from conftest import live_players, pump_until, tab_chips
from PySide6.QtCore import QProcess, Qt
from PySide6.QtTest import QTest

from test_app import open_title_for, release_index


def test_opening_an_episode_spawns_a_player_that_exits_cleanly(window, app):
    title = open_title_for(window, app, "Чорний смолоскип")
    title._selector.setCurrentIndex(release_index(title, "Озвучення", "Рідний Голос"))
    app.processEvents()

    buttons = tab_chips(title)
    assert buttons, "expected episode buttons in the dub release"

    QTest.mouseClick(buttons[0], Qt.MouseButton.LeftButton)

    # Real extraction runs here, then the spawn itself waits on the libmpv
    # runtime check, so neither step is instant.
    assert pump_until(app, lambda: len(live_players()) > 0, timeout=60.0), (
        "no player process was spawned"
    )
    player = live_players()[-1]
    assert pump_until(app, lambda: player.process is not None, timeout=60.0), (
        "the player never started"
    )
    process = player.process
    assert process.state() == QProcess.ProcessState.Running, "player exited immediately"

    exits: list[tuple[int, QProcess.ExitStatus]] = []
    process.finished.connect(lambda code, status: exits.append((code, status)))

    player.close()
    assert pump_until(app, lambda: bool(exits), timeout=30.0), "player did not exit"

    code, status = exits[0]
    # A clean exit is the round trip: the command reached the child, the child
    # tore itself down and its stdin reader stopped. A crash here would mean the
    # reader thread or the GL teardown faulted, which is what the first port did.
    assert status == QProcess.ExitStatus.NormalExit, f"player crashed (code {code})"
    assert code == 0, f"player exited with {code}"

    title.shutdown()
    app.processEvents()

"""The player process boundary.

The player is a separate executable driven over JSON on stdin/stdout, because
PySide6 and the C++ Qt are separate builds whose Qt6Core.dll / Qt6Gui.dll /
Qt6Widgets.dll collide by DLL name: a player DLL fails to import with
WinError 127. Nothing else in the suite crosses that boundary, and a player that
starts but cannot talk looks exactly like a black window, so it is worth pinning.

This drives PlayerProcess directly rather than through the title page. The click
chain is heavy - network, several thread pools - and hanging the boundary's
correctness off it made this test fail about one run in six at the spawn wait,
for reasons that had nothing to do with the boundary. The wiring is asserted
separately and hermetically in test_units.py.
"""

from __future__ import annotations

import pytest
from conftest import pump_until
from PySide6.QtCore import QProcess

from mikai_ui.player_process import PlayerProcess, find_player_exe

TEST_STREAM = "av://lavfi:testsrc2=size=1280x720:rate=25"


@pytest.fixture
def player(app):
    """A real player child, torn down even when the assertions fail."""
    if find_player_exe() is None:
        pytest.skip("mikai-player.exe is not built; see CMakeLists.txt")
    process = PlayerProcess("boundary", TEST_STREAM, {720: TEST_STREAM}, 720)
    yield process
    process.close()


def test_the_child_answers_a_command_and_exits_cleanly(player, app):
    player.start()
    assert pump_until(app, lambda: player.process is not None, timeout=30.0), "never started"

    child = player.process
    assert child.state() == QProcess.ProcessState.Running, "child exited at once"

    exits: list[tuple[int, QProcess.ExitStatus]] = []
    child.finished.connect(lambda code, status: exits.append((code, status)))

    # The round trip. current_height is set from the launch config too, so zero it
    # first: only the child's own answer can bring it back, and the value it
    # reports is one the Python side already sent it.
    player._current_height = 0
    player.query_quality()
    assert pump_until(app, lambda: player.current_height == 720, timeout=30.0), (
        f"child never answered; height={player.current_height} state={child.state()}"
    )

    player.close()
    assert pump_until(app, lambda: bool(exits), timeout=30.0), "child did not exit"

    code, status = exits[0]
    # A clean exit is the rest of the round trip: the close command arrived, the
    # child tore itself down, and nothing faulted on the way out. The first
    # attempt at this port crashed here with 0xC0000005, because the stdin reader
    # was a thread parked in fgets and stopping it held the CRT's stdin lock.
    assert status == QProcess.ExitStatus.NormalExit, f"child crashed (code {code})"
    assert code == 0, f"child exited with {code}"

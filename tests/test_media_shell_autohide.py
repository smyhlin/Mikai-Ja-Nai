"""The lightbox: idle chrome, backdrop presses and the fullscreen toggle.

A player window fills the screen in every mode, so the shell's own chrome is
the only thing standing between the viewer and the picture.  These tests pin
the three promises that keeps honest: the chrome goes away on its own after
AUTOHIDE_MS, the backdrop is not a window-move handle, and the fullscreen
control only changes how the video fills the screen.
"""

from __future__ import annotations

import pytest
from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QMouseEvent
from PySide6.QtWidgets import QApplication

from mikai_ui.media_shell import (
    STATE_DEFAULT_FIT,
    STATE_FULLSCREEN_ACTIVE,
    STATE_FULLSCREEN_AUTO_HIDDEN,
    PlayerShell,
)


CHROME = ("_dock", "_wc", "_btn_prev", "_btn_next", "_footer_l", "_footer_r")


@pytest.fixture
def shell(app: QApplication):
    """A bare top-level shell — the host dialog is not under test here."""
    w = PlayerShell(frameless=True, demo_clock=False)
    w.show()
    app.processEvents()
    yield w
    w.close()
    w.deleteLater()
    app.processEvents()


def _hidden_chrome(w: PlayerShell) -> list[str]:
    """Names of the chrome pieces the viewer cannot see right now."""
    return [name for name in CHROME if not getattr(w, name).isVisible()]


def _send(w: PlayerShell, kind: QMouseEvent.Type, local: QPoint, buttons=Qt.MouseButton.NoButton):
    """Deliver one mouse event to the shell with a real global position."""
    moving = kind == QMouseEvent.Type.MouseMove
    button = Qt.MouseButton.NoButton if moving else Qt.MouseButton.LeftButton
    QApplication.sendEvent(
        w,
        QMouseEvent(
            kind,
            QPointF(local),
            QPointF(w.mapToGlobal(local)),
            button,
            buttons,
            Qt.KeyboardModifier.NoModifier,
        ),
    )


def test_idle_hides_the_chrome_in_the_centred_preview(app, shell) -> None:
    """The preview is the state the shell spends most of its life in.

    Waiting there used to be the one case where the controls never left, so
    the dock sat over the picture until the viewer happened to press a key.
    """
    assert shell.state() == STATE_DEFAULT_FIT
    assert not _hidden_chrome(shell), "the preview starts with its controls up"

    shell._hide_timer.timeout.emit()
    app.processEvents()

    assert shell.state() == STATE_FULLSCREEN_AUTO_HIDDEN
    assert _hidden_chrome(shell) == list(CHROME)
    assert shell.cursor().shape() == Qt.CursorShape.BlankCursor


def test_idle_hides_the_chrome_in_fullscreen(app, shell) -> None:
    shell._enter_fullscreen()
    app.processEvents()
    assert shell.state() == STATE_FULLSCREEN_ACTIVE

    shell._hide_timer.timeout.emit()
    app.processEvents()

    assert _hidden_chrome(shell) == list(CHROME)
    assert shell.cursor().shape() == Qt.CursorShape.BlankCursor


def test_wake_restores_the_chrome_and_rearms_the_idle_timer(app, shell) -> None:
    shell._hide_timer.timeout.emit()
    app.processEvents()
    assert _hidden_chrome(shell), "nothing to restore if the chrome never went"

    shell._wake()
    app.processEvents()

    assert shell.state() == STATE_DEFAULT_FIT
    assert not _hidden_chrome(shell)
    assert shell._hide_timer.isActive(), "the chrome would never go away again"
    assert shell._hide_timer.remainingTime() > 0


def test_a_mouse_move_wakes_the_shell_in_the_preview(app, shell) -> None:
    """Hovering the backdrop is the cheapest gesture a viewer makes.

    The idle timer has to be rearmed by the move itself, or the chrome comes
    back and vanishes again at once.
    """
    shell._hide_timer.timeout.emit()
    app.processEvents()
    shell._hide_timer.stop()
    assert not shell._hide_timer.isActive()

    _send(shell, QMouseEvent.Type.MouseMove, QPoint(40, shell.height() // 2))
    app.processEvents()

    assert shell.state() == STATE_DEFAULT_FIT
    assert not _hidden_chrome(shell)
    assert shell._hide_timer.isActive(), "the move did not rearm the idle timer"
    assert shell._hide_timer.remainingTime() > 0


def test_pressing_the_backdrop_does_not_move_the_window(app, shell) -> None:
    """The lightbox is the window; dragging its letterbox moved the picture.

    The backdrop is where a click reaches the shell's normal press path now, so
    nothing about the geometry may change while the button is held down.
    """
    backdrop = QPoint(20, shell.height() // 2)
    assert not shell._surface.geometry().contains(backdrop), "not on the letterbox"

    before = shell.geometry()
    _send(shell, QMouseEvent.Type.MouseButtonPress, backdrop)
    _send(
        shell,
        QMouseEvent.Type.MouseMove,
        backdrop + QPoint(140, 90),
        Qt.MouseButton.LeftButton,
    )
    _send(shell, QMouseEvent.Type.MouseButtonRelease, backdrop + QPoint(140, 90))
    app.processEvents()

    assert shell.geometry() == before


def test_the_shell_starts_in_the_centred_preview(app) -> None:
    w = PlayerShell(frameless=True, demo_clock=False)
    try:
        assert w.state() == STATE_DEFAULT_FIT
    finally:
        w.close()
        w.deleteLater()
        app.processEvents()


def test_the_shell_can_start_in_fullscreen(app) -> None:
    w = PlayerShell(frameless=True, demo_clock=False, start_fullscreen=True)
    try:
        assert w.state() == STATE_FULLSCREEN_ACTIVE
    finally:
        w.close()
        w.deleteLater()
        app.processEvents()


def test_the_fullscreen_control_toggles_the_video_fit(app, shell) -> None:
    shell._btn_fs.click()
    app.processEvents()
    assert shell.state() == STATE_FULLSCREEN_ACTIVE

    shell._btn_fs.click()
    app.processEvents()
    assert shell.state() == STATE_DEFAULT_FIT


def test_leaving_fullscreen_still_hides_the_chrome_on_idle(app, shell) -> None:
    """The control that drops the video out of full screen is a wake, too.

    Stopping the countdown here used to leave the preview's controls up until
    the viewer happened to move the mouse again.
    """
    shell._btn_fs.click()
    app.processEvents()
    shell._btn_fs.click()
    app.processEvents()

    assert shell.state() == STATE_DEFAULT_FIT
    assert not _hidden_chrome(shell)

    shell._hide_timer.timeout.emit()
    app.processEvents()
    assert _hidden_chrome(shell) == list(CHROME)


def test_the_debug_readouts_never_appear(app, shell) -> None:
    """The state pill and the manifest HUD repeat what the dock already says."""
    for state in (STATE_DEFAULT_FIT, STATE_FULLSCREEN_ACTIVE, STATE_FULLSCREEN_AUTO_HIDDEN):
        if state == STATE_FULLSCREEN_ACTIVE:
            shell._enter_fullscreen()
        elif state == STATE_FULLSCREEN_AUTO_HIDDEN:
            shell._enter_auto_hidden()
        else:
            shell._exit_fullscreen()
        app.processEvents()

        assert not shell._badge.isVisible(), f"the state pill showed in {state}"
        assert not shell._hud.isVisible(), f"the manifest HUD showed in {state}"

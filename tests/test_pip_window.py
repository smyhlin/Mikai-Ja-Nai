"""The float as a user meets it: a background float has no controls, and the
pointer over the picture brings them back.

The float does not move by holding the picture, so there is no drag here to
test. Every event is delivered with QApplication.sendEvent at an explicit
screen position.
"""

from __future__ import annotations

import time

from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
from PySide6.QtGui import QMouseEvent
from PySide6.QtWidgets import QApplication, QWidget

from mikai_ui.pip_window import PipWindow

FLOAT_ORIGIN = QPoint(400, 300)


def _float(app):
    """A shown float with a stand-in surface filling the picture area.

    The stand-in is a plain QWidget that accepts mouse events on purpose: a
    NATIVE surface (the real mpv wid) swallows them instead of letting them
    propagate, and tests/test_player_architecture.py already covers that half
    with a stand-in that accepts nothing.
    """
    pip = PipWindow()
    pip.resize(480, 270)
    video = QWidget()
    pip.set_video(video)
    pip.move(FLOAT_ORIGIN)
    pip.show()
    app.processEvents()
    return pip, video


def _send(
    app,
    widget,
    kind,
    global_pos,
    *,
    button=Qt.MouseButton.NoButton,
    buttons=Qt.MouseButton.NoButton,
):
    local = global_pos - widget.mapToGlobal(QPoint(0, 0))
    event = QMouseEvent(
        kind,
        QPointF(local),
        QPointF(global_pos),
        button,
        buttons,
        Qt.KeyboardModifier.NoModifier,
    )
    QApplication.sendEvent(widget, event)
    app.processEvents()


def _hover(app, widget, global_pos):
    """A move with nothing held: the pointer passing over the picture."""
    _send(app, widget, QEvent.Type.MouseMove, global_pos)


def _settled(app, predicate, timeout=3.0):
    """Wait out a fade, which runs on the real clock rather than in one tick."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        app.processEvents()
        if predicate():
            return True
        time.sleep(0.01)
    return predicate()


def test_the_controls_belong_to_the_focused_float(app):
    pip, video = _float(app)
    try:
        QApplication.sendEvent(pip, QEvent(QEvent.Type.WindowDeactivate))
        assert _settled(app, lambda: not pip._bar.isVisible()), "a background float has no controls"
        assert _settled(app, lambda: video.height() == pip.height()), (
            "the bar's space goes to the picture"
        )
        assert video.geometry() == pip._video_holder.rect(), video.geometry()

        QApplication.sendEvent(pip, QEvent(QEvent.Type.WindowActivate))
        assert _settled(app, pip._bar.isVisible), "the float the user just came back to shows them"

        QApplication.sendEvent(pip, QEvent(QEvent.Type.WindowDeactivate))
        assert _settled(app, lambda: not pip._bar.isVisible()), pip._bar.isVisible()

        # a hover that never activates the window still has to bring them back
        _hover(app, video, video.mapToGlobal(QPoint(40, 30)))
        assert _settled(app, pip._bar.isVisible), "the pointer over the picture is the way back"
    finally:
        pip.close()

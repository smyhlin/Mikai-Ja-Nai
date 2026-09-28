"""The float as the user meets it: the picture is the window and the controls
float on it.

The picture fills every pixel, so the two halves of one gesture are the whole
way the float is driven -- a click is play/pause, a press-and-hold is a move --
and both are delivered at explicit screen positions, because a drag measured in
widget space is a drag that drifts away from the cursor.
"""

from __future__ import annotations

import time

import pytest
from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
from PySide6.QtGui import QKeyEvent, QMouseEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QWidget

from mikai_ui.pip_window import (
    BAR_REVEAL_HOLD_MS,
    SEEK_STEP_SECONDS,
    PipWindow,
    _format_seconds,
)

FLOAT_ORIGIN = QPoint(400, 300)


def _float(app):
    """A shown float with a stand-in surface behind the overlay.

    The stand-in is a plain QWidget: the overlay is a NATIVE child stacked over
    it, so the picture never sees a mouse event and the whole gesture is
    testable from the overlay alone.
    """
    pip = PipWindow()
    pip.resize(480, 270)
    pip.set_video(QWidget())
    pip.move(FLOAT_ORIGIN)
    pip.show()
    app.processEvents()
    return pip


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


def _drag(app, widget, start, end, *, steps=1):
    """A press, a travel, and a release -- the one gesture that both moves the
    float and has to stay silent about playback."""
    _send(
        app,
        widget,
        QEvent.Type.MouseButtonPress,
        start,
        button=Qt.MouseButton.LeftButton,
        buttons=Qt.MouseButton.LeftButton,
    )
    for step in range(1, steps + 1):
        point = QPoint(
            start.x() + (end.x() - start.x()) * step // steps,
            start.y() + (end.y() - start.y()) * step // steps,
        )
        _send(
            app,
            widget,
            QEvent.Type.MouseMove,
            point,
            buttons=Qt.MouseButton.LeftButton,
        )
    _send(
        app,
        widget,
        QEvent.Type.MouseButtonRelease,
        end,
        button=Qt.MouseButton.LeftButton,
        buttons=Qt.MouseButton.NoButton,
    )


def _settled(app, predicate, timeout=3.0):
    """Wait out a fade, which runs on the real clock rather than in one tick."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        app.processEvents()
        if predicate():
            return True
        time.sleep(0.01)
    return predicate()


def _key(pip, key):
    pip.keyPressEvent(QKeyEvent(QEvent.Type.KeyPress, key, Qt.KeyboardModifier.NoModifier, text=""))


def test_the_picture_is_the_window(app):
    """No chrome row: a band left to the controls is a band of black the user
    watches instead of the video."""
    pip = _float(app)
    try:
        assert pip._video_holder.geometry() == pip.rect(), pip._video_holder.geometry()
        assert pip._video.geometry() == pip.rect(), pip._video.geometry()
        assert pip._overlay.geometry() == pip.rect(), pip._overlay.geometry()
        # the controls float above the picture rather than sharing the layout
        assert pip._play_button.parentWidget() is pip._overlay
        # the shared sheet caps #pipPlay at 30px for the main player's bar; the
        # float's centre button must override that or it renders as a lozenge
        assert pip._play_button.size().width() == pip._play_button.size().height()
        assert pip._play_button.width() >= 64, pip._play_button.size()
    finally:
        pip.close()


def test_a_click_on_the_picture_is_play_pause_and_leaves_the_float_alone(app):
    pip = _float(app)
    try:
        toggled = []
        pip.togglePlayRequested.connect(lambda: toggled.append(True))
        before = pip.pos()

        _drag(app, pip._overlay, QPoint(500, 400), QPoint(500, 400))

        assert toggled == [True], toggled
        assert pip.pos() == before, pip.pos()
    finally:
        pip.close()


def test_a_press_held_and_dragged_moves_the_float_by_the_whole_travel(app):
    pip = _float(app)
    try:
        toggled = []
        pip.togglePlayRequested.connect(lambda: toggled.append(True))
        before = pip.pos()
        start = QPoint(520, 410)
        end = QPoint(573, 449)

        _drag(app, pip._overlay, start, end, steps=6)

        assert pip.pos() == before + (end - start), (pip.pos(), before, end - start)
        assert toggled == [], "a move is not a playback command"
    finally:
        pip.close()


def test_a_nudge_short_of_the_threshold_still_counts_as_a_click(app):
    """The threshold is what tells a shaky hand clicking from a hand dragging:
    a couple of pixels of travel is still a click on the picture."""
    pip = _float(app)
    try:
        toggled = []
        pip.togglePlayRequested.connect(lambda: toggled.append(True))
        before = pip.pos()
        start = QPoint(500, 400)

        _drag(app, pip._overlay, start, start + QPoint(3, 3))

        assert pip.pos() == before, pip.pos()
        assert toggled == [True], toggled
    finally:
        pip.close()


def test_the_controls_belong_to_the_focused_float(app):
    pip = _float(app)
    try:
        # the float has to go dark long before the idle hold would have taken
        # the controls away anyway, or a background float would sit under another
        # app wearing its own chrome for another second and a half
        quickly = BAR_REVEAL_HOLD_MS / 1000.0 / 2.0
        QApplication.sendEvent(pip, QEvent(QEvent.Type.WindowActivate))
        assert _settled(app, lambda: pip.controls_shown), "a shown float has its controls"
        _send(app, pip._overlay, QEvent.Type.MouseMove, QPoint(520, 380))
        QApplication.sendEvent(pip, QEvent(QEvent.Type.WindowDeactivate))
        assert _settled(app, lambda: not pip.controls_shown, quickly), (
            "a background float has no controls"
        )

        QApplication.sendEvent(pip, QEvent(QEvent.Type.WindowActivate))
        assert _settled(app, lambda: pip.controls_shown), (
            "the float the user came back to shows them"
        )

        # a hover that never activates the window still has to bring them back
        QApplication.sendEvent(pip, QEvent(QEvent.Type.WindowDeactivate))
        assert _settled(app, lambda: not pip.controls_shown, quickly)
        _send(app, pip._overlay, QEvent.Type.MouseMove, QPoint(520, 380))
        assert _settled(app, lambda: pip.controls_shown), "the pointer is the way back"
    finally:
        pip.close()


def test_the_controls_retire_when_the_pointer_goes_still(app):
    """They belong to the pointer, not to the window: a float the user walked
    away from has nothing left to offer but the picture."""
    pip = _float(app)
    try:
        _send(app, pip._overlay, QEvent.Type.MouseMove, QPoint(520, 380))
        assert _settled(app, lambda: pip.controls_shown), "the pointer brings them"
        assert _settled(app, lambda: not pip.controls_shown, timeout=5.0), (
            "and going still takes them away"
        )
    finally:
        pip.close()


def test_the_float_reports_the_playback_state_it_is_given(app):
    pip = _float(app)
    try:
        toggled = []
        pip.togglePlayRequested.connect(lambda: toggled.append(True))
        pip._play_button.click()
        app.processEvents()
        assert toggled == [True], "the big button in the middle is the way to play"

        paused = pip._play_button.icon().cacheKey()
        assert "Пауза" in pip._play_button.toolTip()
        pip.set_playing(False)
        assert "Продовжити" in pip._play_button.toolTip(), pip._play_button.toolTip()
        assert pip._play_button.icon().cacheKey() != paused, (
            "the glyph carries the state, not only the tooltip"
        )
        pip.set_playing(True)
        assert "Пауза" in pip._play_button.toolTip(), pip._play_button.toolTip()
        assert pip._play_button.icon().cacheKey() == paused, pip._play_button.toolTip()
    finally:
        pip.close()


def test_the_volume_slider_reports_its_own_move_and_quietly_accepts_the_players(
    app,
):
    pip = _float(app)
    try:
        changes = []
        pip.volumeChanged.connect(changes.append)
        pip.set_volume(0.4)
        assert changes == [], "the player telling the float the volume is not a request"
        assert pip._volume_slider.value() == 40

        QTest.mouseClick(
            pip._volume_slider,
            Qt.MouseButton.LeftButton,
            pos=QPoint(pip._volume_slider.width() - 2, pip._volume_slider.height() // 2),
        )
        app.processEvents()
        assert changes and 0.0 <= changes[-1] <= 1.0, changes
        assert changes[-1] == pytest.approx(pip._volume_slider.value() / 100), changes
    finally:
        pip.close()


def test_the_mute_button_answers_with_the_state_it_lands_in(app):
    pip = _float(app)
    try:
        mutes = []
        pip.muteChanged.connect(mutes.append)
        loud = pip._mute_button.icon().cacheKey()

        pip._mute_button.click()
        app.processEvents()
        assert mutes == [True], mutes
        assert "Увімкнути звук" in pip._mute_button.toolTip(), pip._mute_button.toolTip()
        assert pip._mute_button.icon().cacheKey() != loud, (
            "a speaker glyph on a muted float lies about the float"
        )

        pip._mute_button.click()
        app.processEvents()
        assert mutes == [True, False], mutes
        assert "Вимкнути звук" in pip._mute_button.toolTip(), pip._mute_button.toolTip()
        assert pip._mute_button.icon().cacheKey() == loud, pip._mute_button.toolTip()
    finally:
        pip.close()


def test_dragging_the_scrub_asks_for_that_moment_of_the_stream(app):
    pip = _float(app)
    try:
        pip.set_time(30.0, 120.0)
        asks = []
        pip.seekAbsoluteRequested.connect(asks.append)
        scrub = pip._scrub
        middle = scrub.height() // 2
        # the handle sits at 30s of 120s, so the press has to land on it: a press
        # in the groove pages the slider and never starts a drag
        handle = QPoint(scrub.width() // 4, middle)
        target = QPoint(int(scrub.width() * 0.75), middle)

        QTest.mousePress(scrub, Qt.MouseButton.LeftButton, pos=handle)
        QTest.mouseMove(scrub, target)
        QTest.mouseRelease(scrub, Qt.MouseButton.LeftButton, pos=target)
        app.processEvents()

        assert len(asks) == 1, asks
        assert asks[0] == pytest.approx(scrub.value() / 1000.0), asks
        assert 80.0 < asks[0] < 100.0, asks

        # the position the player reports afterwards is not a new request
        reported = asks[0]
        pip.set_time(reported, 120.0)
        assert asks == [reported], asks
        assert pip._elapsed.text() == _format_seconds(reported), pip._elapsed.text()
        assert pip._remaining.text() == "-" + _format_seconds(120.0 - reported)
    finally:
        pip.close()


def test_the_stream_position_is_reported_without_a_known_end(app):
    pip = _float(app)
    try:
        pip.set_time(12.0, 0.0)
        assert pip._elapsed.text() == "--:--", pip._elapsed.text()
        assert pip._remaining.text() == "--:--", pip._remaining.text()
        pip.set_time(75.0, 3600.0)
        assert pip._elapsed.text() == "1:15", pip._elapsed.text()
        assert pip._remaining.text() == "-58:45", pip._remaining.text()
    finally:
        pip.close()


def test_the_float_still_answers_the_keys_the_player_owns(app):
    pip = _float(app)
    try:
        seeks = []
        pip.seekRequested.connect(seeks.append)
        toggled = []
        pip.togglePlayRequested.connect(lambda: toggled.append(True))
        closed = []
        pip.closeRequested.connect(lambda: closed.append(True))

        _key(pip, Qt.Key.Key_Left)
        _key(pip, Qt.Key.Key_Right)
        _key(pip, Qt.Key.Key_Space)
        _key(pip, Qt.Key.Key_Escape)

        assert seeks == [-SEEK_STEP_SECONDS, SEEK_STEP_SECONDS], seeks
        assert toggled == [True], toggled
        assert closed == [True], closed
    finally:
        pip.close()


def test_the_shortcut_survives_a_pointer_that_landed_on_a_control(app):
    """Space is the shortcut the player teaches everywhere else, and a control
    that takes the keyboard would quietly break it: the user nudges the volume,
    then presses Space, and nothing happens.
    """
    pip = _float(app)
    try:
        for control in (pip._volume_slider, pip._scrub, pip._play_button):
            assert control.focusPolicy() == Qt.FocusPolicy.NoFocus, control

        toggled = []
        pip.togglePlayRequested.connect(lambda: toggled.append(True))
        # the key the user presses after touching a control, delivered the way
        # the widget manager would: to whatever holds the focus
        QTest.keyClick(pip._volume_slider, Qt.Key.Key_Space)
        app.processEvents()
        assert toggled == [True], "Space has to reach the float from a control too"
    finally:
        pip.close()


def test_the_restore_and_close_buttons_answer_for_themselves(app):
    pip = _float(app)
    try:
        restored = []
        pip.restoreRequested.connect(lambda: restored.append(True))
        closed = []
        pip.closeRequested.connect(lambda: closed.append(True))

        pip._restore_button.click()
        pip._close_button.click()
        app.processEvents()

        assert restored == [True], restored
        assert closed == [True], closed
    finally:
        pip.close()

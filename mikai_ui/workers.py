from __future__ import annotations

from collections.abc import Callable
from typing import Any

from PySide6.QtCore import QObject, QRunnable, QThreadPool, Signal, Slot


class TaskSignals(QObject):
    finished = Signal(object)
    failed = Signal(str)


class Task(QRunnable):
    def __init__(self, fn: Callable[..., Any], *args: Any, **kwargs: Any) -> None:
        super().__init__()
        self._fn = fn
        self._args = args
        self._kwargs = kwargs
        self.signals = TaskSignals()

    @Slot()
    def run(self) -> None:
        try:
            result = self._fn(*self._args, **self._kwargs)
        except Exception as exc:  # noqa: BLE001 - a task can run any repository callable.
            self._emit(self.signals.failed, str(exc))
        else:
            self._emit(self.signals.finished, result)

    @staticmethod
    def _emit(signal: Any, *args: Any) -> None:
        # teardown can delete the signals object while this worker is in flight
        try:
            signal.emit(*args)
        except RuntimeError:
            pass


class Runner(QObject):
    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._pool = QThreadPool(self)
        # bound concurrent thumbnail/quality extraction requests
        self._pool.setMaxThreadCount(4)
        self._tasks: set[Task] = set()

    def run(
        self,
        fn: Callable[..., Any],
        on_ok: Callable[[Any], None],
        on_err: Callable[[str], None],
    ) -> None:
        task = Task(fn)
        self._tasks.add(task)

        def _done(result: Any) -> None:
            self._tasks.discard(task)
            on_ok(result)

        def _failed(message: str) -> None:
            self._tasks.discard(task)
            on_err(message)

        task.signals.finished.connect(_done)
        task.signals.failed.connect(_failed)
        self._pool.start(task)

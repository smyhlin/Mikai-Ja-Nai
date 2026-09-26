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
        self._bulk_pool = QThreadPool(self)
        # the home page and the episode grid fan out one task per card; on the shared
        # pool that would queue a title the user just opened behind the whole feed
        self._bulk_pool.setMaxThreadCount(4)
        self._bulk_tasks: set[Task] = set()

    def run(
        self,
        fn: Callable[..., Any],
        on_ok: Callable[[Any], None],
        on_err: Callable[[str], None],
    ) -> None:
        self._start(self._pool, self._tasks, fn, on_ok, on_err)

    def run_bulk(
        self,
        fn: Callable[..., Any],
        on_ok: Callable[[Any], None],
        on_err: Callable[[str], None],
    ) -> None:
        self._start(self._bulk_pool, self._bulk_tasks, fn, on_ok, on_err)

    def wait_for_done(self, timeout_ms: int) -> bool:
        self._bulk_pool.waitForDone(timeout_ms)
        return self._pool.waitForDone(timeout_ms)

    def _start(
        self,
        pool: QThreadPool,
        tasks: set[Task],
        fn: Callable[..., Any],
        on_ok: Callable[[Any], None],
        on_err: Callable[[str], None],
    ) -> None:
        task = Task(fn)
        tasks.add(task)

        def _done(result: Any) -> None:
            tasks.discard(task)
            on_ok(result)

        def _failed(message: str) -> None:
            tasks.discard(task)
            on_err(message)

        task.signals.finished.connect(_done)
        task.signals.failed.connect(_failed)
        pool.start(task)

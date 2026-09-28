"""The mikai player, hosted as a separate process over newline-delimited JSON.

The player is an executable, not a DLL, and that is not a workaround. PySide6
6.11.2 and the C++ Qt 6.8.3 are separate Qt builds whose ``Qt6Core.dll`` /
``Qt6Gui.dll`` / ``Qt6Widgets.dll`` collide by DLL name: the Windows loader
binds the second copy to the first, the import table fails to resolve, and the
import dies with WinError 127. A process boundary is the only shape that works.
So ``mikai-player.exe`` owns its own window, its own Qt and its own mpv, and this
module speaks to it: commands down the child's stdin, events up its stdout, one
JSON object per line. The wire contract lives in ``src/player/main.cpp``.

``QProcess`` owns the child rather than ``subprocess.PIPE`` because teardown is
a correctness requirement here, not a convenience: a leaked player window is a
top-level window that outlives mikai. ``QProcess`` gives ``finished``,
``terminate`` and ``kill`` on the GUI thread without hand-rolled bookkeeping, and
on Windows its stdout arrives through Qt's own ``QWindowsPipeReader`` thread and
is delivered back as a queued event, so every handler below runs on the GUI
thread. A second ``threading.Thread`` reading the same pipe would race Qt's
reader for the bytes and drop events, so the reader is deliberately Qt's and not
ours.

Launch-time state the protocol cannot carry yet, kept here and reported rather
than dropped:

* ``variants`` / ``current_embed`` — the dub/sub list. The launch config carries
  only caption/url/qualities/height and no command delivers a variant list, so
  the child's track picker starts empty and ``trackProbe`` never fires on its
  own. Kept intact on ``_variants`` / ``_current_embed``; the moment
  ``main.cpp`` grows a way to carry them they are already here to send.
* ``copyUrl`` — a bare notification with no payload, because the host is the one
  that handed the player a stream URL. The URL launched with is what gets
  copied.
"""

from __future__ import annotations

import json
import logging
import os
import sys
import tempfile
from pathlib import Path

from PySide6.QtCore import QObject, QProcess, QProcessEnvironment, QUrl, Signal, Slot
from PySide6.QtGui import QDesktopServices, QGuiApplication

from mikai_backend import mpv_runtime

log = logging.getLogger(__name__)

# The build output CMake writes the player into; the env override exists so a
# dev build outside the tree can be pointed at without a rebuild.
PLAYER_EXE_NAME = "mikai-player.exe"
PLAYER_EXE_ENV = "MIKAI_PLAYER_EXE"
BUILD_DIR_NAME = "build-cmake"


def repo_root() -> Path:
    """The checkout root, tolerating a frozen build where __file__ is unpacked."""
    here = Path(__file__).resolve()
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS", here.parent))
    return here.parent.parent


def find_player_exe() -> Path | None:
    """The player executable, or None if it was never built."""
    override = os.environ.get(PLAYER_EXE_ENV, "").strip()
    if override:
        candidate = Path(override).expanduser()
        return candidate if candidate.is_file() else None
    for folder in (repo_root() / BUILD_DIR_NAME, Path.cwd() / BUILD_DIR_NAME):
        candidate = folder / PLAYER_EXE_NAME
        if candidate.is_file():
            return candidate.resolve()
    return None


def require_player_exe() -> Path:
    """The player executable or a clear error naming both ways to provide it."""
    exe = find_player_exe()
    if exe is not None:
        return exe
    raise FileNotFoundError(
        f"{PLAYER_EXE_NAME} not found. Build it with CMake into "
        f"{(repo_root() / BUILD_DIR_NAME) / PLAYER_EXE_NAME}, or point "
        f"{PLAYER_EXE_ENV} at an existing executable."
    )


def usable_qualities(qualities: dict[int, str] | None) -> dict[int, str]:
    """The heights that actually have a URL, which is all the child can play."""
    return {
        int(height): url
        for height, url in (qualities or {}).items()
        if isinstance(url, str) and url.strip()
    }


def qualities_to_json(qualities: dict[int, str]) -> dict[str, str]:
    """Quality keys go over the wire as strings; the child parses them back."""
    return {str(height): url for height, url in sorted(qualities.items())}


def clamp_index(index: int, length: int) -> int:
    """An index the child can actually highlight in its episode drawer.

    The drawer has no row for an out-of-range index, so sending one leaves the
    child pointing at nothing; 0 is the same fallback the in-process player used.
    """
    return index if 0 <= index < length else 0


class PlayerProcess(QObject):
    """``ShellPlayerDialog``'s public surface, backed by a child process.

    The signals and method names are the ones ``title_page.py`` already uses, so
    the page's wiring does not change shape when the player moves out of
    process. ``show()`` is a no-op: the child raises and shows its own window at
    launch, exactly as the in-process dialog used to.

    ``current_height`` replaces ``ShellPlayerDialog.controller.state.quality``,
    the value ``title_page.py`` reads to re-extract the stream the player is on
    RIGHT NOW. There is no controller here to ask; the child reports the height
    it settled on and that is kept in step.
    """

    runtimeRequested = Signal()
    playbackFailed = Signal()
    variantRequested = Signal(str, str)
    episodeRequested = Signal(object, int)
    trackProbeRequested = Signal(str)

    def __init__(
        self,
        caption: str,
        url: str,
        qualities: dict[int, str] | None = None,
        initial_height: int | None = None,
        variants: list[dict] | None = None,
        current_embed: str = "",
        playlist: list[dict] | None = None,
        episode_index: int = 0,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self._caption = caption
        # the stream URL we launched with: the child sends copyUrl bare, so this
        # is the value the user asked to copy
        self._url = url
        self._qualities = usable_qualities(qualities)
        self._current_height = int(initial_height or 0)
        # the track list the protocol cannot carry yet; see the module docstring
        self._variants = [dict(variant) for variant in (variants or [])]
        self._current_embed = current_embed
        self._playlist: list[dict] = []
        self._episode_index = 0
        # the title page writes the playing episode here, so a stream recovery
        # re-extracts the track that is playing, not the one we opened with
        self._page_entry: dict | None = None

        self._closed = False
        self._process: QProcess | None = None
        # stdout arrives in chunks that split lines; the tail waits for its
        # newline so a half-delivered event is never parsed
        self._stdout_buffer = ""
        # the config file has to outlive argv[1] being read, which the child
        # does before its event loop starts, but keeping it until close removes
        # the question of when that read happened
        self._config_dir: tempfile.TemporaryDirectory | None = None

        self.set_playlist(playlist, episode_index)

    # ── launching ──────────────────────────────────────────────────────

    def start(self) -> None:
        """Spawn the child, or raise if it is not there to spawn.

        Called lazily rather than from ``__init__`` so a caller that only
        inspects the object does not leak a process.
        """
        if self._closed or self._process is not None:
            return
        try:
            exe = require_player_exe()
        except FileNotFoundError as exc:
            log.error("%s", exc)
            raise

        self._config_dir = tempfile.TemporaryDirectory(prefix="mikai-player-")
        config = Path(self._config_dir.name) / "config.json"
        config.write_text(
            json.dumps(
                {
                    "caption": self._caption,
                    "url": self._url,
                    "qualities": qualities_to_json(self._qualities),
                    "height": self._current_height,
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )

        process = QProcess(self)
        environment = QProcessEnvironment.systemEnvironment()
        # libmpv-2.dll is an import-time dependency, so the loader has to find
        # it before main() runs; mpv_runtime already provisioned the directory
        # the app uses everywhere else, so ask it rather than look twice.
        runtime_dir = mpv_runtime.find_runtime()
        if runtime_dir is not None:
            path = str(runtime_dir)
            existing = environment.value("PATH")
            if existing:
                path = path + os.pathsep + existing
            environment.insert("PATH", path)
        process.setProcessEnvironment(environment)
        process.setWorkingDirectory(str(exe.parent))
        process.finished.connect(self._on_finished)
        process.errorOccurred.connect(self._on_error)

        try:
            process.start(str(exe), [str(config)])
        except Exception:
            log.exception("failed to spawn %s", exe)
            self._config_dir.cleanup()
            self._config_dir = None
            raise

        self._process = process
        process.readyReadStandardOutput.connect(self._on_stdout)
        # stderr is drained, not merely redirected: a full pipe blocks the child
        # mid-startup, and a player that cannot speak is worse than a noisy log
        process.readyReadStandardError.connect(self._on_stderr)
        log.info("player started: %s", exe)

    def show(self) -> None:
        """A no-op: the child showed its own window at launch."""
        self.start()

    # ── lifetime ───────────────────────────────────────────────────────

    def close(self) -> None:
        """Ask the child to close, then make sure it actually did."""
        if self._closed:
            return
        self._closed = True
        process = self._process
        self._process = None
        if process is not None:
            self._send({"cmd": "close"}, process=process)
            if not process.waitForFinished(1500):
                process.terminate()
                if not process.waitForFinished(1000):
                    process.kill()
                    process.waitForFinished(500)
        self._config_dir.cleanup() if self._config_dir else None
        self._config_dir = None

    def __del__(self) -> None:
        # the last resort against a player window outliving mikai: __del__ runs
        # before the QObject destructor, so the child is gone before QProcess is
        # torn down and cannot warn about a process still running
        try:
            self.close()
        except Exception:  # interpreter teardown, nothing useful left to log
            pass

    @Slot(int, QProcess.ExitStatus)
    def _on_finished(self, exit_code: int, status: QProcess.ExitStatus) -> None:
        log.info("player exited: code=%s status=%s", exit_code, status.name)
        self._closed = True

    @Slot(QProcess.ProcessError)
    def _on_error(self, error: QProcess.ProcessError) -> None:
        log.error("player process error: %s", error.name)

    # ── stdout ─────────────────────────────────────────────────────────

    @Slot()
    def _on_stdout(self) -> None:
        process = self._process
        if process is None:
            return
        chunk = bytes(process.readAllStandardOutput()).decode("utf-8", "replace")
        self._stdout_buffer += chunk
        while "\n" in self._stdout_buffer:
            line, self._stdout_buffer = self._stdout_buffer.split("\n", 1)
            line = line.strip()
            if line:
                self._on_event(line)

    def _on_event(self, line: str) -> None:
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            log.warning("malformed player event: %r", line)
            return
        if not isinstance(event, dict):
            log.warning("player event is not an object: %r", line)
            return
        name = event.get("ev")
        handler = self._EVENTS.get(str(name))
        if handler is None:
            log.warning("unknown player event: %r", line)
            return
        handler(self, event)

    def _on_variant(self, event: dict) -> None:
        self.variantRequested.emit(str(event.get("embed", "")), str(event.get("label", "")))

    def _on_episode(self, event: dict) -> None:
        # the child knows the index but not the embed behind it, so the item the
        # page expects is resolved here, from the playlist it sent us
        index = int(event.get("index", -1))
        if not 0 <= index < len(self._playlist):
            log.warning("player asked for episode %d, playlist has %d", index, len(self._playlist))
            return
        self.episodeRequested.emit(dict(self._playlist[index]), index)

    def _on_track_probe(self, event: dict) -> None:
        self.trackProbeRequested.emit(str(event.get("embed", "")))

    def _on_open_original(self, event: dict) -> None:
        # "open in browser" means the stream the player is on, which is the one
        # it was launched with
        if self._url:
            QDesktopServices.openUrl(QUrl(self._url))

    def _on_copy_url(self, event: dict) -> None:
        # bare notification by design: the host handed the player this URL, so
        # the host is the one that knows it
        if not self._url:
            return
        QGuiApplication.clipboard().setText(self._url)
        log.info("stream url copied")

    def _on_playback_failed(self, event: dict) -> None:
        self.playbackFailed.emit()

    def _on_closed(self, event: dict) -> None:
        # the child's window is gone; nothing left to drive
        self._closed = True
        log.info("player window closed")

    def _on_quality(self, event: dict) -> None:
        self._current_height = int(event.get("height", 0))

    _EVENTS = {
        "variant": _on_variant,
        "episode": _on_episode,
        "trackProbe": _on_track_probe,
        "openOriginal": _on_open_original,
        "copyUrl": _on_copy_url,
        "playbackFailed": _on_playback_failed,
        "closed": _on_closed,
        "quality": _on_quality,
    }

    @Slot()
    def _on_stderr(self) -> None:
        process = self._process
        if process is None:
            return
        text = bytes(process.readAllStandardError()).decode("utf-8", "replace").strip()
        if text:
            log.warning("player stderr: %s", text)

    # ── commands ───────────────────────────────────────────────────────

    def _send(self, command: dict, process: QProcess | None = None) -> bool:
        target = process if process is not None else self._process
        if target is None or target.state() != QProcess.ProcessState.Running:
            return False
        try:
            payload = (json.dumps(command, ensure_ascii=False) + "\n").encode("utf-8")
            written = target.write(payload)
        except Exception:
            log.exception("failed to write command %s", command.get("cmd"))
            return False
        if written != len(payload):
            log.warning("short write for command %s: %d of %d bytes", command.get("cmd"), written, len(payload))
        return True

    def apply_episode(
        self,
        label: str,
        embed: str,
        index: int,
        qualities: dict[int, str],
        variants: list[dict],
    ) -> None:
        """Step to another episode in place: same window, same position."""
        if self._closed:
            return
        usable = usable_qualities(qualities)
        if not usable:
            return
        self._episode_index = clamp_index(int(index), len(self._playlist))
        self._variants = [dict(variant) for variant in variants]
        self._current_embed = embed
        self._caption = label
        self._qualities = usable
        self._current_height = max(usable)
        self._send(
            {
                "cmd": "applyEpisode",
                "url": usable[self._current_height],
                "caption": label,
                "index": self._episode_index,
                "embed": embed,
                "qualities": qualities_to_json(usable),
                "height": self._current_height,
            }
        )
        self._send_playlist()

    def apply_variant(self, qualities: dict[int, str], label: str, embed: str = "") -> None:
        """Swap the dub/sub track; the child resumes at the same position."""
        if self._closed:
            return
        usable = usable_qualities(qualities)
        if not usable:
            return
        if embed:
            self._current_embed = embed
        self._qualities = usable
        self._current_height = max(usable)
        self._send(
            {
                "cmd": "applyVariant",
                "qualities": qualities_to_json(usable),
                "label": label,
                "embed": embed,
            }
        )

    def apply_refreshed_streams(self, qualities: dict[int, str], height: int | None = None) -> None:
        """Reload from a re-extracted quality map after a failed stream load."""
        if self._closed:
            return
        usable = usable_qualities(qualities)
        if not usable:
            return
        self._qualities = usable
        target = int(height) if height in usable else max(usable)
        self._current_height = target
        self._send(
            {
                "cmd": "applyRefreshedStreams",
                "qualities": qualities_to_json(usable),
                "height": target,
            }
        )

    def set_track_quality(self, embed: str, quality: int) -> None:
        """Badge one track with the quality a probe found for it."""
        if self._closed or int(quality) <= 0:
            return
        for variant in self._variants:
            if variant.get("embed") == embed:
                variant["quality"] = int(quality)
        self._send({"cmd": "setTrackQuality", "embed": embed, "quality": int(quality)})

    def set_playlist(self, playlist: list[dict] | None, index: int = 0) -> None:
        """Hand the child its episode labels; the embeds stay here."""
        self._playlist = [dict(item) for item in (playlist or [])]
        self._episode_index = clamp_index(int(index), len(self._playlist))
        if self._process is not None:
            self._send_playlist()

    def _send_playlist(self) -> None:
        # the child takes labels only: it asks back for an index, and the embed
        # behind it is resolved here when the page answers with the episode
        self._send(
            {
                "cmd": "setPlaylist",
                "labels": [str(item.get("label", "")) for item in self._playlist],
                "index": int(self._episode_index),
            }
        )

    def quality_changed(self, height: int) -> None:
        """Ask the child which height it settled on.

        The child only reports ``quality`` in answer to this, so it is how the
        Python side bootstraps ``current_height`` after a track swap.
        """
        self._send({"cmd": "qualityChanged", "height": int(height)})

    # ── state ──────────────────────────────────────────────────────────

    @property
    def current_height(self) -> int:
        """The height the player is on.

        Replaces ``ShellPlayerDialog.controller.state.quality``: there is no
        controller in this process, so the value is whatever the child last
        reported, or the height we last asked it for.
        """
        return self._current_height

    @property
    def variants(self) -> list[dict]:
        """The dub/sub list, retained because the wire cannot carry it yet."""
        return [dict(variant) for variant in self._variants]

    @property
    def process(self) -> QProcess | None:
        """The child, for a caller that needs to watch it directly."""
        return self._process

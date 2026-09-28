"""DEV TOOL -- not part of the application. Delete before shipping.

Proves the process boundary end to end without mikai: launch ``mikai-player.exe``
against a config JSON, send a command, print the events that come back, close.
Every run needs a hand on the keyboard, because the point is to look at the
child's window, so this is a script to run by hand, not a test.

    python src/player/player_deploy.py
    python src/player/player_deploy.py --url <stream> --height 720

It drives the real ``PlayerProcess`` rather than reimplementing the protocol, so
a run that prints events is evidence about the shipping bridge and not about
this file.
"""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path

# Running this file directly puts src/ on sys.path, not the repo root.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from PySide6.QtCore import QTimer  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from mikai_ui.player_process import PlayerProcess  # noqa: E402

log = logging.getLogger("player_deploy")

SAMPLE_URL = "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Launch the player process by hand.")
    parser.add_argument("--url", default=SAMPLE_URL, help="stream URL to hand the player")
    parser.add_argument("--caption", default="Серія 1 · 1080p", help="window caption")
    parser.add_argument("--height", type=int, default=1080, help="starting quality")
    parser.add_argument(
        "--seconds", type=float, default=20.0, help="how long to listen for events"
    )
    return parser.parse_args()


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")
    args = parse_args()

    # the child opens a window and the bridge answers with the clipboard, so a
    # real QApplication has to exist before either is touched
    app = QApplication.instance() or QApplication(sys.argv)

    player = PlayerProcess(
        args.caption,
        args.url,
        qualities={args.height: args.url},
        initial_height=args.height,
    )
    # wired before launch, so nothing the child says on startup is missed
    player.variantRequested.connect(lambda embed, label: print(f"  <- variant {embed} {label}"))
    player.episodeRequested.connect(lambda item, index: print(f"  <- episode {index} {item}"))
    player.trackProbeRequested.connect(lambda embed: print(f"  <- trackProbe {embed}"))
    player.playbackFailed.connect(lambda: print("  <- playbackFailed"))

    try:
        player.show()
    except FileNotFoundError as exc:
        print(f"cannot start: {exc}")
        return 1

    # the one command worth sending blind: the child answers with the height it
    # is on, which is the whole request/response round trip
    player.quality_changed(args.height)
    print(f"player running for {args.seconds:g}s -- the window is the real test")
    # Qt owns the loop, so the reader's events are actually delivered; a plain
    # sleep here would prove nothing about the signal path
    QTimer.singleShot(int(args.seconds * 1000), app.quit)
    app.exec()

    print("closing")
    player.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

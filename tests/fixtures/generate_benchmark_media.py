#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import subprocess
import sys


CLIP_SIZES = ((480, 360), (640, 480), (768, 576), (1280, 720), (1920, 1080))


def main():
    parser = argparse.ArgumentParser(description="Generate Mikai's synthetic benchmark clips.")
    parser.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "resources" / "benchmarks",
    )
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for width, height in CLIP_SIZES:
        output = args.output / f"{width}x{height}.mp4"
        if output.exists():
            raise FileExistsError(f"Refusing to overwrite {output}")
        command = [
            args.ffmpeg,
            "-hide_banner", "-loglevel", "error", "-n",
            "-f", "lavfi", "-i", f"color=c=black:s={width}x{height}:r=2997/125",
            "-frames:v", "2158", "-an", "-c:v", "libx264", "-preset", "ultrafast",
            "-crf", "23", "-pix_fmt", "yuv420p", "-threads", "4",
            "-color_range", "tv", "-colorspace", "bt709", "-color_primaries", "bt709",
            "-color_trc", "bt709", "-movflags", "+faststart", "-map_metadata", "-1",
            "-metadata", "title=Mikai synthetic benchmark", str(output),
        ]
        subprocess.run(command, check=True)
        print(f"Created {output}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

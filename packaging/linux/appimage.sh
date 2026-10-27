#!/usr/bin/env bash
# Consumes an already configured/built Linux tree and writes one AppImage.
set -euo pipefail
usage() {
  printf '%s\n' 'Usage: bash packaging/linux/appimage.sh --build-dir DIR --output-dir FRESH_DIR [--appimagetool PATH]'
}
build_dir=''
output_dir=''
appimage_tool=''
while (($#)); do
  case "$1" in
    --build-dir|--output-dir|--appimagetool)
      if (($# < 2)) || [[ -z "$2" ]]; then usage >&2; exit 2; fi
      case "$1" in
        --build-dir) build_dir="$2" ;;
        --output-dir) output_dir="$2" ;;
        --appimagetool) appimage_tool="$2" ;;
      esac
      shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done
if [[ -z "$build_dir" || -z "$output_dir" ]]; then usage >&2; exit 2; fi
if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
  printf '%s\n' 'AppImage packaging requires a native Linux x86_64 build host.' >&2
  exit 1
fi
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
args=(-DBUILD_DIR="$build_dir" -DOUTPUT_DIR="$output_dir")
if [[ -n "$appimage_tool" ]]; then args+=(-DAPPIMAGE_TOOL="$appimage_tool"); fi
exec cmake "${args[@]}" -P "$script_dir/AppImage.cmake"

#!/usr/bin/env bash
# Builds the Debian package in a Debian trixie container against a Qt 6.12 SDK.
#
# The container is not a convenience: the package's glibc floor is the base
# image's, so a binary built on a rolling host would not start on the
# distribution the package is for.
set -euo pipefail
usage() {
  printf '%s\n' 'Usage: bash packaging/linux/deb-package.sh --output-dir FRESH_DIR --qt-dir QT_SDK [--work-dir DIR] [--image TAG] [--mpv-archive FILE] [--rebuild-image]'
}
output_dir=''
qt_dir=''
work_dir=''
mpv_archive=''
image='mikai-deb-builder:trixie'
rebuild=0
while (($#)); do
  case "$1" in
    --output-dir|--qt-dir|--work-dir|--image|--mpv-archive)
      if (($# < 2)) || [[ -z "$2" ]]; then usage >&2; exit 2; fi
      case "$1" in
        --output-dir) output_dir="$2" ;;
        --qt-dir) qt_dir="$2" ;;
        --work-dir) work_dir="$2" ;;
        --image) image="$2" ;;
        --mpv-archive) mpv_archive="$2" ;;
      esac
      shift 2 ;;
    --rebuild-image) rebuild=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done
if [[ -z "$output_dir" || -z "$qt_dir" ]]; then usage >&2; exit 2; fi
if [[ ! -d "$qt_dir/lib/cmake/Qt6" ]]; then
  printf 'Not a Qt 6 SDK directory: %s\n' "$qt_dir" >&2
  exit 2
fi
if [[ ! -x "$qt_dir/bin/lrelease" ]]; then
  printf 'The Qt SDK has no lrelease; translation compilation would fail: %s\n' "$qt_dir" >&2
  exit 2
fi
if ! command -v docker >/dev/null 2>&1; then
  printf '%s\n' 'Docker is required to build a package on a different distribution than this host.' >&2
  exit 1
fi
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/../.." && pwd -P)"

mpv_archive="${mpv_archive:-$repo_root/release-artifacts/mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst}"
if [[ ! -f "$mpv_archive" ]]; then
  printf 'The bundled runtime archive is missing: %s\n' "$mpv_archive" >&2
  printf 'Build it with: bash packaging/mpv/build-tls-archive.sh\n' >&2
  exit 1
fi

if [[ -e "$output_dir" ]]; then
  if [[ ! -d "$output_dir" || -n "$(ls -A "$output_dir")" ]]; then
    printf 'Output directory is not an empty directory: %s\n' "$output_dir" >&2
    exit 1
  fi
fi
mkdir -p "$output_dir"

if ((rebuild)) || ! docker image inspect "$image" >/dev/null 2>&1; then
  docker build -t "$image" -f "$repo_root/packaging/debian/docker/Dockerfile" "$repo_root/packaging/debian/docker"
fi

revision="$(git -C "$repo_root" rev-parse --short HEAD)"
if [[ -n "$(git -C "$repo_root" status --porcelain --untracked-files=no)" ]]; then
  revision="$revision-dirty"
fi

# The container writes as root; a failed run must still leave nothing behind.
cleanup() {
  rm -rf "$work_dir" 2>/dev/null && return 0
  docker run --rm -v "$(dirname "$work_dir"):/parent" --entrypoint rm "$image" \
    -rf "/parent/$(basename "$work_dir")" >/dev/null 2>&1 || true
}
if [[ -z "$work_dir" ]]; then
  work_dir="$(mktemp -d /var/tmp/mikai-deb.XXXXXX)"
  trap cleanup EXIT
else
  mkdir -p "$work_dir"
fi

docker run --rm \
  -e REVISION="$revision" \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  -v "$repo_root:/src:ro" \
  -v "$qt_dir:/qt:ro" \
  -v "$mpv_archive:/mpv/runtime.tar.zst:ro" \
  -v "$work_dir:/work" \
  "$image" \
  bash /src/packaging/debian/in-container.sh

shopt -s nullglob
packages=("$work_dir"/out/*.deb)
if (( ${#packages[@]} == 0 )); then
  printf '%s\n' 'The container produced no package.' >&2
  exit 3
fi
for package in "${packages[@]}"; do
  cp -- "$package" "$output_dir/"
done
chown "$(id -u):$(id -g)" "$output_dir"/*.deb 2>/dev/null || true
printf 'Created %s\n' "$output_dir"/*.deb

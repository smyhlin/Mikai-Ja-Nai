#!/usr/bin/env bash
# Builds a pacman package from the committed source tree.
#
# The package is built, not repackaged: makepkg configures this checkout, runs
# the repository's own CTest gate, and installs the result into $pkgdir. A dirty
# worktree is refused because the tarball must name the revision it came from.
set -euo pipefail
usage() {
  printf '%s\n' 'Usage: bash packaging/linux/arch-package.sh --output-dir FRESH_DIR [--work-dir DIR] [--skip-check]'
}
output_dir=''
work_dir=''
skip_check=0
while (($#)); do
  case "$1" in
    --output-dir|--work-dir)
      if (($# < 2)) || [[ -z "$2" ]]; then usage >&2; exit 2; fi
      case "$1" in
        --output-dir) output_dir="$2" ;;
        --work-dir) work_dir="$2" ;;
      esac
      shift 2 ;;
    --skip-check) skip_check=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done
if [[ -z "$output_dir" ]]; then usage >&2; exit 2; fi
if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
  printf '%s\n' 'Arch packaging requires a native Linux x86_64 build host.' >&2
  exit 1
fi
for tool in makepkg git cmake ninja sha256sum; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'Missing packaging tool: %s\n' "$tool" >&2
    exit 1
  fi
done
for dependency in qt6-base mpv libglvnd; do
  if ! pacman -Q "$dependency" >/dev/null 2>&1; then
    printf 'Missing build dependency: %s (pacman -S --needed %s)\n' "$dependency" "$dependency" >&2
    exit 1
  fi
done
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/../.." && pwd -P)"
cd "$repo_root"

if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
  printf '%s\n' 'Refusing to package a modified worktree; commit or stash first.' >&2
  exit 1
fi
revision="$(git rev-parse --short HEAD)"
version="$(sed -n 's/^project(mikai VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt | head -1)"
if [[ -z "$version" || -z "$revision" ]]; then
  printf '%s\n' 'Could not read the project version or revision.' >&2
  exit 3
fi

if [[ -e "$output_dir" ]]; then
  if [[ ! -d "$output_dir" || -n "$(ls -A "$output_dir")" ]]; then
    printf 'Output directory is not an empty directory: %s\n' "$output_dir" >&2
    exit 1
  fi
fi
mkdir -p "$output_dir"

if [[ -z "$work_dir" ]]; then
  work_dir="$(mktemp -d "${TMPDIR:-/tmp}/mikai-arch-package.XXXXXX")"
  trap 'rm -rf "$work_dir"' EXIT
else
  mkdir -p "$work_dir"
fi
tarball="$work_dir/mikai-$version.tar.gz"
git archive --format=tar.gz --prefix="mikai-$version/" -o "$tarball" HEAD
digest="$(sha256sum "$tarball" | cut -d' ' -f1)"

build_dir="$work_dir/pkgbuild"
mkdir -p "$build_dir/out"
sed -e "s/@SHA256@/$digest/" -e "s/@REVISION@/$revision/" \
  packaging/arch/PKGBUILD > "$build_dir/PKGBUILD"
cp "$tarball" "$build_dir/mikai-$version.tar.gz"

makepkg_args=(--noconfirm --force --cleanbuild)
if ((skip_check)); then makepkg_args+=(--nocheck); fi
(
  cd "$build_dir"
  PKGDEST="$build_dir/out" SRCDEST="$build_dir" makepkg "${makepkg_args[@]}"
)

shopt -s nullglob
packages=("$build_dir"/out/*.pkg.tar.*)
if (( ${#packages[@]} == 0 )); then
  printf '%s\n' 'makepkg produced no package.' >&2
  exit 3
fi
for package in "${packages[@]}"; do
  cp -- "$package" "$output_dir/"
done
pkgrel="$(sed -n "s/^pkgrel=\(.*\)/\1/p" "$build_dir/PKGBUILD" | head -1)"
(
  cd "$build_dir"
  makepkg --printsrcinfo > "$output_dir/mikai-$version-${pkgrel:-1}.SRCINFO"
)
printf 'Created %s\n' "$output_dir"/*

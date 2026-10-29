#!/usr/bin/env bash
# Rebuilds the pinned Linux libmpv runtime with an FFmpeg that carries a TLS
# backend.
#
# The archive published at the pinned commit was configured without
# --enable-openssl or --enable-gnutls, so every https URL fails with
# "No protocol handler found" while local files still play. Nothing else about
# the runtime changes here: the same mpv fork, libplacebo, shaderc, libass and
# Vulkan bytes are kept, and only the FFmpeg family is replaced by a build of the
# same tag with identical configure flags plus --enable-openssl. OpenSSL 3.0 from
# the Ubuntu 22.04 baseline travels with it, because the FFmpeg build links
# libssl.so.3.
#
# Usage: bash packaging/mpv/build-tls-archive.sh [--output DIR]
set -euo pipefail

output_dir=''
while (($#)); do
  case "$1" in
    --output)
      [[ $# -ge 2 && -n "$2" ]] || { printf 'Usage: %s [--output DIR]\n' "$0" >&2; exit 2; }
      output_dir="$2"; shift 2 ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
  esac
done

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/../.." && pwd -P)"
output_dir="${output_dir:-$repo_root/release-artifacts}"
mkdir -p "$output_dir"

upstream_name='mpv-linux-x64-2026-10-24-c493f1c.tar.zst'
upstream_sha='b6281a2c924157a01f4a66f49dda3029b11378ffd982a974cdcbdb474c0befc7'
upstream_url="https://github.com/the-database/mpv/releases/download/2026-10-24-c493f1c/$upstream_name"
ffmpeg_tag='n9.0.2'
ffnvcodec_tag='n13.0.19.0'
result_name='mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst'

command -v docker >/dev/null 2>&1 || { printf 'docker is required\n' >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { printf 'curl is required\n' >&2; exit 1; }

work="$(mktemp -d /var/tmp/mikai-mpv-tls.XXXXXX)"
trap 'rm -rf "$work"' EXIT

curl -fsSL -o "$work/$upstream_name" "$upstream_url"
printf '%s  %s\n' "$upstream_sha" "$work/$upstream_name" | sha256sum -c -

mkdir -p "$work/base" "$work/fflibs"
tar --zstd -xf "$work/$upstream_name" -C "$work/base"

cat > "$work/ffmpeg-build.sh" <<'FFMPEG'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential pkg-config libssl-dev nasm git ca-certificates >/dev/null
git clone --depth 1 -b "${FFNVCODEC_TAG}" https://github.com/FFmpeg/nv-codec-headers.git /work/nv
make -C /work/nv PREFIX=/opt/ff install >/dev/null
export PKG_CONFIG_PATH=/opt/ff/lib/pkgconfig
export LD_LIBRARY_PATH=/opt/ff/lib
git clone --depth 1 -b "${FFMPEG_TAG}" https://github.com/FFmpeg/FFmpeg.git /work/ffmpeg
cd /work/ffmpeg
# The upstream flags plus TLS; every other switch is unchanged on purpose, so the
# replacement libraries differ only in protocol support.
./configure --prefix=/opt/ff --enable-gpl --enable-version3 \
  --enable-shared --disable-static --enable-ffnvcodec --enable-nvdec --enable-cuvid \
  --enable-nvenc --enable-openssl --disable-programs --disable-doc --disable-debug \
  --extra-cflags="-I/opt/ff/include" --extra-ldflags="-L/opt/ff/lib"
make -j"$(nproc)" >/dev/null
make install >/dev/null
cp -a /opt/ff/lib/libav*.so* /opt/ff/lib/libsw*.so* /work/out/fflibs/
cp -aL /lib/x86_64-linux-gnu/libssl.so.3 /lib/x86_64-linux-gnu/libcrypto.so.3 /work/out/fflibs/
FFMPEG

docker run --rm --network host \
  -e FFMPEG_TAG="$ffmpeg_tag" -e FFNVCODEC_TAG="$ffnvcodec_tag" \
  -v "$work/fflibs:/work/out/fflibs" \
  -v "$work/ffmpeg-build.sh:/build.sh:ro" \
  ubuntu:22.04 bash /build.sh

cp -a "$work/base" "$work/bundle"
python3 - "$work" <<'PY'
import glob, os, shutil, sys
work = sys.argv[1]
# The archive keeps unversioned soname files; the TLS build names its outputs
# libX.so.MAJOR.MINOR.PATCH, so the versioned file is copied onto that name.
mapping = {'avcodec': '63', 'avdevice': '63', 'avfilter': '12', 'avformat': '63',
           'avutil': '61', 'swresample': '7', 'swscale': '10'}
for family, major in mapping.items():
    candidates = sorted(glob.glob(f'{work}/fflibs/lib{family}.so.*.*'))
    if not candidates:
        raise SystemExit(f'missing built lib{family}')
    shutil.copy2(candidates[0], f'{work}/bundle/lib{family}.so.{major}')
for lib in ('libssl.so.3', 'libcrypto.so.3'):
    shutil.copy2(f'{work}/fflibs/{lib}', f'{work}/bundle/{lib}')
PY

if strings "$work/bundle/libavformat.so.63" | grep -q "recompile FFmpeg with openssl"; then
  printf 'The rebuilt libavformat still reports no TLS backend.\n' >&2
  exit 3
fi
rm -f "$work/bundle/mpv"
tar --zstd -C "$work/bundle" -cf "$output_dir/$result_name" .
sha256sum "$output_dir/$result_name"
printf 'Created %s\n' "$output_dir/$result_name"

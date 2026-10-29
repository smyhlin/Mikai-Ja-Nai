#!/usr/bin/env bash
# Runs inside the trixie build image. Configures this checkout against the Qt
# 6.12 SDK mounted at $QT_DIR, installs into /opt/mikai, relocates the desktop
# integration files, derives Depends from the shipped ELF objects, builds the
# package, then installs and starts it once before removing it again.
set -euo pipefail

SRC="${SRC:-/src}"
QT_DIR="${QT_DIR:-/qt}"
WORK="${WORK:-/work}"
REVISION="${REVISION:-unknown}"
BUILD="$WORK/build"
STAGE="$WORK/stage"
OUT="$WORK/out"

if [[ ! -f "$SRC/CMakeLists.txt" ]]; then
  printf 'Source tree not found at %s\n' "$SRC" >&2
  exit 2
fi
if [[ ! -d "$QT_DIR/lib/cmake/Qt6" ]]; then
  printf 'Qt 6.12 SDK not found at %s\n' "$QT_DIR" >&2
  exit 2
fi
VERSION="$(sed -n 's/^project(mikai VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt" | head -1)"
if [[ -z "$VERSION" ]]; then
  printf 'Could not read the project version.\n' >&2
  exit 3
fi

rm -rf "$BUILD" "$STAGE" "$OUT"
mkdir -p "$BUILD" "$STAGE" "$OUT"

# The SDK's tools and libraries want its ICU 73; the distribution has a newer
# major. This is a build-time path only, cleared before the package is shipped
# or tested, so nothing can resolve a dependency the package fails to carry.
export LD_LIBRARY_PATH="/opt/icu73/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$QT_DIR" \
  -DCMAKE_INSTALL_PREFIX=/opt/mikai \
  -DMIKAI_LINUX_DEPLOY_QT=ON \
  -DMIKAI_LINUX_BUNDLE_MPV=OFF \
  -DMIKAI_BUILD_PROBES=OFF \
  -DMIKAI_BUILD_REVISION="$REVISION"
cmake --build "$BUILD"
DESTDIR="$STAGE" cmake --install "$BUILD" --prefix /opt/mikai

# Desktop integration belongs in the shared hierarchy; the application keeps its
# private Qt runtime, shaders and licence files together under /opt/mikai.
mkdir -p "$STAGE/usr/bin" "$STAGE/usr/share/doc"
for entry in applications icons metainfo; do
  if [[ -d "$STAGE/opt/mikai/share/$entry" ]]; then
    mv "$STAGE/opt/mikai/share/$entry" "$STAGE/usr/share/"
  fi
done
if [[ -d "$STAGE/opt/mikai/share/licenses/mikai" ]]; then
  mv "$STAGE/opt/mikai/share/licenses/mikai" "$STAGE/usr/share/doc/mikai"
  install -m644 "$STAGE/usr/share/doc/mikai/LICENSE" "$STAGE/usr/share/doc/mikai/copyright"
fi
ln -s /opt/mikai/bin/mikai "$STAGE/usr/bin/mikai"

if [[ ! -f "$STAGE/opt/mikai/bin/mikai" || ! -f "$STAGE/opt/mikai/bin/qt.conf" ]]; then
  printf 'The install did not produce the expected /opt/mikai tree.\n' >&2
  exit 3
fi

# The Qt 6.12 SDK needs ICU 73, which this distribution does not provide, so it
# travels with the private runtime. Every library in that directory resolves its
# siblings through $ORIGIN rather than the host's search path.
for icu in libicuuc libicui18n libicudata; do
  for lib in /opt/icu73/lib/$icu.so.73*; do
    [[ -e "$lib" ]] || continue
    cp -P "$lib" "$STAGE/opt/mikai/lib/"
  done
done
if ! ls "$STAGE/opt/mikai/lib"/libicui18n.so.73 >/dev/null 2>&1; then
  printf 'ICU 73 was not found in /opt/icu73/lib; the private runtime would be incomplete.\n' >&2
  exit 3
fi
while IFS= read -r library; do
  patchelf --set-rpath '$ORIGIN' "$library"
done < <(find "$STAGE/opt/mikai/lib" -maxdepth 1 -name '*.so*' -not -type d -print)

unset LD_LIBRARY_PATH

# Dependencies: every soname the shipped objects ask for that this tree does not
# carry itself, mapped back to the package that provides it in the build image.
elfs=()
while IFS= read -r candidate; do
  if file --brief "$candidate" | grep -q ELF; then elfs+=("$candidate"); fi
done < <(find "$STAGE" -type f \( -name '*.so' -o -name '*.so.*' -o -name mikai \) -print)
own_libraries="$(find "$STAGE/opt/mikai" -name '*.so*' -not -type d -printf '%f\n' | sort -u)"
declared=()
while read -r soname; do
  [[ -z "$soname" ]] && continue
  if grep -qxF "$soname" <<<"$own_libraries"; then continue; fi
  provider="$(dpkg -S "*/$soname" 2>/dev/null | head -1 | cut -d: -f1 || true)"
  if [[ -z "$provider" ]]; then
    printf 'No package owns %s\n' "$soname" >&2
    continue
  fi
  declared+=("$provider")
done < <(for elf in "${elfs[@]}"; do objdump -p "$elf" | awk '/NEEDED/{print $2}'; done | sort -u)

glibc_floor="$(for elf in "${elfs[@]}"; do objdump -T "$elf" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' || true; done \
  | sort -uV | tail -1 | sed 's/^GLIBC_//')"
[[ -z "$glibc_floor" ]] && glibc_floor="2.36"
# libmpv is opened with dlopen, so no ELF object asks for it by name. It stays a
# distribution dependency: the player cannot decode without it.
mapfile -t dependencies < <(printf '%s\n' libc6 libstdc++6 libgcc-s1 libmpv2 "${declared[@]}" | sort -u)
dependency_line="$(printf '%s\n' "${dependencies[@]}" | grep -vx 'libc6\|libstdc++6\|libgcc-s1' | paste -sd, - | sed 's/,/, /g')"
dependency_line="libc6 (>= $glibc_floor), libstdc++6, libgcc-s1${dependency_line:+, $dependency_line}"

installed_size="$(du -sk "$STAGE" | cut -f1)"
mkdir -p "$STAGE/DEBIAN"
sed -e "s/@PACKAGE@/mikai/" -e "s/@VERSION@/$VERSION/" \
  -e "s/@INSTALLED_SIZE@/$installed_size/" -e "s/@DEPENDS@/$dependency_line/" \
  "$SRC/packaging/debian/control.in" > "$STAGE/DEBIAN/control"

(
  cd "$STAGE"
  find . -path ./DEBIAN -prune -o -type f -print0 \
    | xargs -0 md5sum > DEBIAN/md5sums
)
deb="$OUT/mikai_${VERSION}_amd64.deb"
dpkg-deb --root-owner-group -Zxz --build "$STAGE" "$deb"

# Install, start and remove inside this image: the package must work as a
# package, not only as an extracted tree.
dpkg -i "$deb"
installed_version="$(mikai --version)"
printf 'installed: %s\n' "$installed_version"
# A normal user, because a root run would write the native store into the
# package tree and make the removal check below meaningless.
if ! id -u tester >/dev/null 2>&1; then useradd --create-home --shell /bin/bash tester; fi
startup_log="$(su tester -c 'HOME=/home/tester QT_QPA_PLATFORM=offscreen timeout 8 /usr/bin/mikai' 2>&1 || true)"
printf '%s\n' "$startup_log" | tail -5
if printf '%s\n' "$startup_log" | grep -qiE 'could not find the Qt platform plugin|cannot load library'; then
  printf 'The installed package cannot load its Qt platform plugin.\n' >&2
  exit 4
fi
dpkg -r mikai >/dev/null
if [[ -e /usr/bin/mikai ]]; then
  printf 'Removal left /usr/bin/mikai behind.\n' >&2
  exit 5
fi
if [[ -n "${HOST_UID:-}" && -n "${HOST_GID:-}" ]]; then
  chown -R "$HOST_UID:$HOST_GID" "$WORK"
fi
printf 'Created %s\n' "$deb"

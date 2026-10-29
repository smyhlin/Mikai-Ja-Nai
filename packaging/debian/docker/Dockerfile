# Builds the Debian package against the distribution's libmpv and a Qt 6.12 SDK
# mounted at /qt. The base matters: the binary's glibc floor is the base's, so
# the image is the oldest distribution the package is meant to install on.
#
# The Qt 6.12 SDK links ICU 73, and no current distribution ships that major
# version (Debian 13 has 76). It is built here and shipped beside the Qt
# runtime: the alternative is a Qt built from source, which the SDK makes
# unnecessary.
FROM debian:trixie-slim AS icu73

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      curl \
      ca-certificates \
 && curl -fsSL -o /tmp/icu.tgz \
      https://github.com/unicode-org/icu/releases/download/release-73-2/icu4c-73_2-src.tgz \
 && echo "818a80712ed3caacd9b652305e01afc7fa167e6f2e94996da44b90c2ab604ce1  /tmp/icu.tgz" \
      | sha256sum -c - \
 && mkdir -p /tmp/icu \
 && tar -xzf /tmp/icu.tgz -C /tmp/icu --strip-components=1 \
 && cd /tmp/icu/source \
 && ./configure --prefix=/opt/icu73 --disable-samples --disable-tests --disable-extras \
 && make -j"$(nproc)" \
 && make install \
 && rm -rf /tmp/icu /tmp/icu.tgz

FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      ninja-build \
      pkg-config \
      patchelf \
      file \
      dpkg-dev \
      libmpv-dev \
      libgl-dev \
      libegl-dev \
      libxkbcommon0 \
      libxkbcommon-x11-0 \
      libxcb-cursor0 \
      libxcb-icccm4 \
      libxcb-image0 \
      libxcb-keysyms1 \
      libxcb-randr0 \
      libxcb-render-util0 \
      libxcb-shape0 \
      libxcb-xfixes0 \
      libxcb-xinerama0 \
      libxcb-xkb1 \
      libfontconfig1 \
      libfreetype6 \
      libdbus-1-3 \
      libxi6 \
      libsm6 \
      libice6 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=icu73 /opt/icu73 /opt/icu73

# Qt's own deployment script reads the SDK's plugin tree and rewrites RPATHs.
ENV QT_SDK=/qt

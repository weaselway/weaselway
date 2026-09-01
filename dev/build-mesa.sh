#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../_build/mesa"
SOURCE_DIR="${SCRIPT_DIR}/../../mesa/"
PREFIX=/usr
# install into the multiarch libdir so this build actually overrides the distro mesa
# (/usr/lib64 is not on the loader path)
LIBDIR="lib/$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)"

# BUILDTYPE=debugoptimized
BUILDTYPE=release

export PKG_CONFIG_PATH="${PREFIX}/${LIBDIR}/pkgconfig:${PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

if [ ! -f "${BUILD_DIR}/okay" ]; then
  meson setup --reconfigure "${BUILD_DIR}" "${SOURCE_DIR}" \
    --buildtype="${BUILDTYPE}" \
    -Dprefix="${PREFIX}" \
    -Dlibdir="${LIBDIR}" \
    -Dglvnd=enabled \
    -Dplatforms=x11,wayland \
    -Degl-native-platform=surfaceless \
    -Dgallium-drivers=softpipe,d3d12 \
    -Dvulkan-drivers=swrast,microsoft-experimental \
    -Dgallium-d3d12-graphics=enabled \
    -Dgallium-d3d12-video=enabled \
    -Dshader-cache=enabled

  touch "${BUILD_DIR}/okay"
fi

meson compile -C "${BUILD_DIR}"
sudo meson install --no-rebuild -C "${BUILD_DIR}"

#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../_build/mutter"
SOURCE_DIR="${SCRIPT_DIR}/../../mutter/"
PREFIX=/usr
# install into the multiarch libdir so this build actually overrides the distro mutter
# (/usr/lib64 is not on the loader path)
LIBDIR="lib/$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)"

export PKG_CONFIG_PATH="${PREFIX}/${LIBDIR}/pkgconfig:${PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

if [ ! -f "${BUILD_DIR}/okay" ]; then
    meson setup --reconfigure "${BUILD_DIR}" "${SOURCE_DIR}" \
        -Dbuildtype="debugoptimized" \
        -Dprefix="${PREFIX}" \
        -Dlibdir="${LIBDIR}" \
        -Dudev_dir="${PREFIX}/lib/udev" \
        -Drdp=enabled \
        -Dtests=disabled \
        -Ddocs=false \
        -Dprofiler=false \
        -Dcogl_tests=false \
        -Dclutter_tests=false \
        -Dmutter_tests=false \
        -Dinstalled_tests=false \
        -Dintrospection=true

  touch "${BUILD_DIR}/okay"
fi

meson compile -C "${BUILD_DIR}"
sudo meson install --no-rebuild -C "${BUILD_DIR}"

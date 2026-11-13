#!/usr/bin/env bash

# Installs weaselway on an Ubuntu 26.04 WSL distro: GNOME and PipeWire from
# Ubuntu, the patched Mesa and weaselwayd compiled here, the dxgdrm module and
# the Windows viewer from GitHub releases. See ubuntu/README.md.
#
# Run it from a checkout of this repository, as your user (it uses sudo), with
# --with-flatpak or --without-flatpak.
# Running it again rebuilds and reinstalls, e.g. after a WSL kernel update.

set -euo pipefail

# The versions the NixOS image ships (flake.lock).
DXGDRM_VERSION=v1.3.2
VIEWER_VERSION=v1.2.0
MESA_REV=8d9fc5da13127d588386c59beeaceb14285f5aed # branch flatpak-wsl
# The runtime branches the Flatpak GL extension is built for; apps on other
# branches get the runtime's own Mesa.
FLATPAK_BRANCHES=(25.08 26.08)

PREFIX=/opt/weaselway
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${XDG_CACHE_HOME:-${HOME}/.cache}/weaselway-build"
KERNEL="$(uname -r)"

die() {
    echo "error: $*" >&2
    exit 1
}

step() {
    echo
    echo "==> $*"
}

# No default: the Flatpak extension adds two Mesa builds and the Flatpak SDKs,
# so the user decides.
case "$*" in
--with-flatpak) WITH_FLATPAK=1 ;;
--without-flatpak) WITH_FLATPAK=0 ;;
*) die "usage: $0 --with-flatpak|--without-flatpak" ;;
esac

[ "$(id -u)" != 0 ] || die "run this as your user, not as root; it uses sudo where needed"
[ -n "${WSL_DISTRO_NAME:-}" ] || die "this is not a WSL distro"
[ -d /run/systemd/system ] || die "systemd is not running; set systemd=true under [boot] in /etc/wsl.conf and run wsl --shutdown"

mkdir -p "${BUILD}"

# First, so that an unsupported kernel fails before anything is installed.
step "Downloading dxgdrm ${DXGDRM_VERSION} for ${KERNEL}"
curl -fL --progress-bar -o "${BUILD}/dxgdrm.tar.gz" \
    "https://github.com/weaselway/dxgdrm/releases/download/${DXGDRM_VERSION}/dxgdrm-${KERNEL}.tar.gz" ||
    die "dxgdrm ${DXGDRM_VERSION} has no module for the kernel ${KERNEL}"
curl -fsSL -o "${BUILD}/dxgdrm_drm.h" \
    "https://raw.githubusercontent.com/weaselway/dxgdrm/${DXGDRM_VERSION}/dxgdrm_drm.h"

step "Installing packages"
# GNOME recommends a display manager, NetworkManager and more that have no
# place in WSL: gdm would take the display, NetworkManager the network.
sudo tee /etc/apt/preferences.d/weaselway > /dev/null <<'EOF'
# Installed by weaselway: GNOME's recommends that do not work in WSL.
Package: gdm3 network-manager modemmanager wpasupplicant cups cups-daemon bluez plymouth snapd
Pin: release *
Pin-Priority: -1
EOF
# The WSL image ships snapd, and the pin above only stops new installs. Snaps
# bring their own Mesa without d3d12 on dxgdrm, so remove snapd and its snaps.
sudo DEBIAN_FRONTEND=noninteractive apt-get purge -y --auto-remove snapd
# Ubuntu's firefox is a snap too; Mozilla publishes a .deb in its own repository.
sudo install -d -m 0755 /etc/apt/keyrings
sudo curl -fsSL -o /etc/apt/keyrings/packages.mozilla.org.asc https://packages.mozilla.org/apt/repo-signing-key.gpg
sudo tee /etc/apt/sources.list.d/mozilla.sources > /dev/null <<'EOF'
Types: deb
URIs: https://packages.mozilla.org/apt
Suites: mozilla
Components: main
Signed-By: /etc/apt/keyrings/packages.mozilla.org.asc
EOF
sudo tee /etc/apt/preferences.d/mozilla > /dev/null <<'EOF'
# Installed by weaselway: Firefox from Mozilla, never Ubuntu's snap wrapper.
Package: *
Pin: origin packages.mozilla.org
Pin-Priority: 1000

Package: firefox*
Pin: release o=Ubuntu
Pin-Priority: -1
EOF
# Mesa's build dependencies come from its source package.
sudo sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    gnome-session gnome-shell gnome-terminal nautilus gnome-control-center \
    gnome-text-editor gnome-backgrounds xdg-desktop-portal-gnome pipewire-pulse wireplumber \
    mesa-utils vulkan-tools kbd curl git unzip build-essential pkg-config \
    freerdp3-dev libwinpr3-dev libssl-dev libglib2.0-dev libpng-dev \
    libwayland-dev wayland-protocols libgbm-dev libdrm-dev libegl-dev libgles-dev
sudo DEBIAN_FRONTEND=noninteractive apt-get build-dep -y mesa

step "Building Mesa"
if [ ! -d "${BUILD}/mesa/.git" ]; then
    git init -q "${BUILD}/mesa"
    git -C "${BUILD}/mesa" remote add origin https://github.com/weaselway/mesa.git
fi
git -C "${BUILD}/mesa" fetch -q --depth 1 origin "${MESA_REV}"
git -C "${BUILD}/mesa" checkout -q --force FETCH_HEAD

# The drivers of the NixOS image: d3d12 on the GPU, llvmpipe without one.
# shellcheck disable=SC2054 # meson takes comma-separated lists
MESON_FLAGS=(
    --prefix="${PREFIX}/mesa"
    --libdir=lib
    --buildtype=release
    -Dglvnd=enabled
    -Dplatforms=x11,wayland
    -Dgallium-drivers=d3d12,llvmpipe,softpipe,zink
    -Dvulkan-drivers=microsoft-experimental,swrast
    -Dgallium-d3d12-graphics=enabled
    -Dgallium-d3d12-video=enabled
    -Dgallium-va=enabled
    -Dvideo-codecs=all
    -Dgallium-rusticl=false
    -Dteflon=false
    -Dintel-rt=disabled
    -Dtools=
)
if [ -f "${BUILD}/mesa-build/build.ninja" ]; then
    meson setup --reconfigure "${BUILD}/mesa-build" "${BUILD}/mesa" "${MESON_FLAGS[@]}"
else
    meson setup "${BUILD}/mesa-build" "${BUILD}/mesa" "${MESON_FLAGS[@]}"
fi
meson compile -C "${BUILD}/mesa-build"
sudo meson install -C "${BUILD}/mesa-build" --no-rebuild --quiet

# Ahead of Ubuntu's Mesa: ld.so.conf.d is read in name order, and the first
# directory with a library wins. Ubuntu's Mesa stays installed for apt.
echo "${PREFIX}/mesa/lib" | sudo tee /etc/ld.so.conf.d/00-weaselway-mesa.conf > /dev/null
sudo ldconfig
sudo mkdir -p /etc/vulkan/icd.d
sudo ln -sf "${PREFIX}/mesa/share/vulkan/icd.d/dzn_icd.x86_64.json" /etc/vulkan/icd.d/

if [ "${WITH_FLATPAK}" = 1 ]; then
    step "Building the Flatpak GL extension for ${FLATPAK_BRANCHES[*]}"
    # Flatpak apps bring their own Mesa, without d3d12. This builds ours in the
    # SDK of each runtime branch, against its libraries, and installs it as an
    # unmaintained extension, which Flatpak prefers over the runtime's
    # GL.default. Apps see the one for their runtime's branch. prep-session.sh
    # gives it /dev/dxg and WSL's libraries at boot.
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y flatpak
    sudo flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
    for BRANCH in "${FLATPAK_BRANCHES[@]}"; do
        sudo flatpak install -y --noninteractive --or-update flathub "org.freedesktop.Sdk//${BRANCH}"
    done

    # The Intel driver needs libedit.so.2, which the runtimes lack.
    rm -rf "${BUILD}/libedit"
    mkdir -p "${BUILD}/libedit"
    (cd "${BUILD}/libedit" && apt-get source -q --download-only libedit)

    # Like the Mesa above, but only the drivers for WSL and without LLVM,
    # which the SDK only has as an extension: softpipe instead of llvmpipe.
    GL_PREFIX=/usr/lib/x86_64-linux-gnu/GL/host
    # shellcheck disable=SC2054 # meson takes comma-separated lists
    FLATPAK_MESON_FLAGS=(
        --prefix="${GL_PREFIX}"
        --libdir=lib
        --buildtype=release
        -Dglvnd=enabled
        -Dplatforms=x11,wayland
        -Dgallium-drivers=d3d12,softpipe
        -Dvulkan-drivers=microsoft-experimental
        -Dllvm=disabled
        -Dgallium-d3d12-graphics=enabled
        -Dgallium-d3d12-video=enabled
        -Dgallium-va=enabled
        -Dvideo-codecs=all
        -Dgallium-rusticl=false
        -Dteflon=false
        -Dintel-rt=disabled
        -Dtools=
    )

    for BRANCH in "${FLATPAK_BRANCHES[@]}"; do
        OUT="${BUILD}/flatpak-${BRANCH}"
        rm -rf "${OUT}/libedit" "${OUT}/dest" "${OUT}/stage"
        mkdir -p "${OUT}/libedit"
        tar -xf "${BUILD}"/libedit/libedit_*.orig.tar.* -C "${OUT}/libedit" --strip-components=1
        # The SDK has libtinfo but no libncurses.so to link it by.
        ln -sf /usr/lib/x86_64-linux-gnu/libtinfo.so.6 "${OUT}/libncurses.so"

        # The SDK lacks PyYAML, hence the network.
        # shellcheck disable=SC2016 # expanded by the SDK's shell
        flatpak run --share=network --filesystem="${BUILD}" --command=sh \
            "org.freedesktop.Sdk//${BRANCH}" -euc '
            SRC="$1" OUT="$2" GL_PREFIX="$3"
            shift 3
            [ -d "${OUT}/python/yaml" ] ||
                python3 -m pip install -q --target "${OUT}/python" pyyaml
            export PYTHONPATH="${OUT}/python"
            if [ -f "${OUT}/mesa-build/build.ninja" ]; then
                meson setup --reconfigure "${OUT}/mesa-build" "${SRC}" "$@"
            else
                meson setup "${OUT}/mesa-build" "${SRC}" "$@"
            fi
            meson compile -C "${OUT}/mesa-build"
            DESTDIR="${OUT}/dest" meson install -C "${OUT}/mesa-build" --no-rebuild --quiet

            # ldconfig finds libraries by SONAME, and the driver asks for libedit.so.2.
            cd "${OUT}/libedit"
            ./configure -q --prefix="${GL_PREFIX}" --disable-static LDFLAGS="-L${OUT}"
            make -s -j"$(nproc)" LT_VERSION=2:0:0
        ' sh "${BUILD}/mesa" "${OUT}" "${GL_PREFIX}" "${FLATPAK_MESON_FLAGS[@]}"

        # The layout of a GL extension: the vendor and ICD files at the top, where
        # Flatpak merges them with the other extensions'. wsl/ gets its binds at boot.
        mkdir -p "${OUT}/stage"
        mv "${OUT}/dest${GL_PREFIX}"/{lib,share} "${OUT}/stage/"
        mv "${OUT}"/stage/share/{glvnd,vulkan} "${OUT}/stage/"
        rm -rf "${OUT}/stage/lib/pkgconfig" "${OUT}"/stage/lib/*.a
        install -m755 "${OUT}/libedit/src/.libs/libedit.so.2.0.0" "${OUT}/stage/lib/libedit.so.2"

        # Never rm -rf the extension itself: wsl/ may hold binds of WSL's files.
        GL_EXTENSION="/var/lib/flatpak/extension/org.freedesktop.Platform.GL.host/x86_64/${BRANCH}"
        sudo mkdir -p "${GL_EXTENSION}/wsl/lib" "${GL_EXTENSION}/wsl/drivers"
        sudo rm -rf "${GL_EXTENSION}"/{lib,share,glvnd,vulkan}
        sudo cp -R "${OUT}"/stage/{lib,share,glvnd,vulkan} "${GL_EXTENSION}/"
    done

    # Flatpak keeps a library cache per app and never renews it for unmaintained
    # extensions (flatpak#5948).
    rm -rf "${HOME}"/.var/app/*/.ld.so
fi

step "Building weaselwayd"
make -C "${REPO}/weaselwayd" DXGDRM_INCLUDE="${BUILD}" weaselwayd check
sudo make -C "${REPO}/weaselwayd" PREFIX=/usr/local install

step "Installing dxgdrm and the viewer"
sudo rm -rf "${PREFIX}/dxgdrm/lib/modules/${KERNEL}"
sudo mkdir -p "${PREFIX}/dxgdrm"
sudo tar -C "${PREFIX}/dxgdrm" -xzf "${BUILD}/dxgdrm.tar.gz"
sudo install -m644 "${PREFIX}/dxgdrm/lib/udev/rules.d/99-dxgdrm.rules" /etc/udev/rules.d/

curl -fL --progress-bar -o "${BUILD}/viewer.zip" \
    "https://github.com/weaselway/freerdp/releases/download/${VIEWER_VERSION}/freerdp-${VIEWER_VERSION}.zip"
sudo rm -rf "${PREFIX}/viewer"
sudo unzip -q "${BUILD}/viewer.zip" -d "${PREFIX}/viewer"

step "Installing the scripts and the configuration"
for SCRIPT in ww-start-session ww-start-viewer ww-install-viewer-link; do
    sudo install -m755 "${REPO}/bin/${SCRIPT}.sh" "/usr/local/bin/${SCRIPT}"
done
sudo install -Dm755 "${REPO}/libexec/prep-session.sh" "${PREFIX}/libexec/prep-session.sh"

sudo install -Dm644 "${REPO}/environment.d/10-weaselway.conf" /etc/environment.d/10-weaselway.conf
# VA-API looks for d3d12_drv_video.so in Ubuntu's directory otherwise.
echo "LIBVA_DRIVERS_PATH=${PREFIX}/mesa/lib/dri" | sudo tee /etc/environment.d/11-weaselway-ubuntu.conf > /dev/null
sudo install -Dm644 "${REPO}/pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf" \
    /etc/pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf

# Login shells: WSL points PULSE_SERVER at WSLg's PulseAudio, which would hide
# PipeWire, and ww-start-viewer needs to know where the viewer is.
sudo tee /etc/profile.d/weaselway.sh > /dev/null <<EOF
unset PULSE_SERVER
export WEASELWAY_VIEWER="\${WEASELWAY_VIEWER:-${PREFIX}/viewer/sdl-freerdp.exe}"
EOF

# weaselwayd creates the viewer's input devices through uinput, as the user.
sudo tee /etc/udev/rules.d/70-weaselway-uinput.rules > /dev/null <<'EOF'
KERNEL=="uinput", SUBSYSTEM=="misc", GROUP="input", MODE="0660"
EOF
sudo usermod -aG render,video,input "$(id -un)"

# Loads dxgdrm, evdev, uinput and vhci_hcd, takes /tmp/.X11-unix back from WSLg and
# mounts WSLg's shared memory, once per boot. It also gives the Flatpak GL
# extension /dev/dri/dxg and WSL's libraries.
sudo tee /etc/systemd/system/weaselway-prep.service > /dev/null <<EOF
[Unit]
Description=Weaselway host preparation for the session
Documentation=https://github.com/weaselway/weaselway
After=local-fs.target wslg.service systemd-udevd.service

[Service]
Type=oneshot
RemainAfterExit=yes
Environment=DXGDRM_ROOT=${PREFIX}/dxgdrm
ExecStart=${PREFIX}/libexec/prep-session.sh

[Install]
WantedBy=multi-user.target
EOF

# On kernels without DRM core the tarball has it as a module, and logind only
# opens DRM devices if it is loaded before logind starts.
sudo mkdir -p /etc/systemd/system/modprobe@drm.service.d
sudo tee /etc/systemd/system/modprobe@drm.service.d/weaselway.conf > /dev/null <<EOF
[Service]
ExecStart=
ExecStart=-/usr/sbin/modprobe -abq -d ${PREFIX}/dxgdrm drm
EOF

# Lets weaselwayd lower its nice value to -11.
sudo mkdir -p /etc/systemd/system/user@.service.d
sudo tee /etc/systemd/system/user@.service.d/weaselway.conf > /dev/null <<'EOF'
[Service]
LimitNICE=-11
EOF

# Only for this user: a copy in another user's session, e.g. root's after
# wsl -u root, can't get the vsock port and restarts forever.
sudo tee /etc/systemd/user/weaselwayd.service > /dev/null <<EOF
[Unit]
Description=Weaselway display server for the Windows viewer
Documentation=https://github.com/weaselway/weaselway
ConditionUser=$(id -un)

[Service]
ExecStart=/usr/local/bin/weaselwayd
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
EOF

sudo systemctl daemon-reload
sudo udevadm control --reload
sudo systemctl enable weaselway-prep.service
sudo systemctl --global enable weaselwayd.service
# WSL's unit links pulse/native in the runtime directory to WSLg's PulseAudio,
# over pipewire-pulse's socket.
sudo systemctl --global mask wslg-session.service
# WSL has no TPM. The unit, which GNOME's dependencies bring along, triggers
# until it hits its start limit and leaves the system degraded.
sudo systemctl mask tpm-udev.path

cat <<EOF

Done. Restart WSL so that the module, the groups and the environment take
effect, in PowerShell:

    wsl --shutdown

Then start the distro again from the Windows desktop (not over SSH) and run:

    ww-start-session
    ww-start-viewer
EOF

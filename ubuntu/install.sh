#!/usr/bin/env bash

# Installs weaselway on an Ubuntu 26.04 WSL distro: GNOME and PipeWire from
# Ubuntu, the patched Mesa and weaselwayd compiled here, the dxgdrm module and
# the Windows viewer from GitHub releases. See ubuntu/README.md.
#
# Run it from a checkout of this repository, as your user (it uses sudo).
# Running it again rebuilds and reinstalls, e.g. after a WSL kernel update.

set -euo pipefail

# The versions the NixOS image ships (flake.lock).
DXGDRM_VERSION=v1.2.0
VIEWER_VERSION=v1.2.0rc1
MESA_REV=e2008be3fa70d4b6e2cc519abb3dfabb58d1e169 # branch mesa-26.2.1-wsl

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

# Loads dxgdrm, evdev and uinput, takes /tmp/.X11-unix back from WSLg and
# mounts WSLg's shared memory, once per boot.
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

sudo tee /etc/systemd/user/weaselwayd.service > /dev/null <<'EOF'
[Unit]
Description=Weaselway display server for the Windows viewer
Documentation=https://github.com/weaselway/weaselway

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

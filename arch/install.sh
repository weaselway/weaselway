#!/usr/bin/env bash

# Installs weaselway on an Arch Linux or CachyOS WSL distro: Plasma and
# PipeWire from the distro, the patched Mesa and weaselwayd compiled here, the
# dxgdrm module and the Windows viewer from GitHub releases. See arch/README.md.
#
# Run it from a checkout of this repository, as your user (it uses sudo).
# Running it again rebuilds and reinstalls, e.g. after a WSL kernel update.

set -euo pipefail

# The versions the NixOS image ships (flake.lock, nix/overlay.nix).
DXGDRM_VERSION=v1.3.2
VIEWER_VERSION=v1.2.0
MESA_REV=118d7c93713f7d1fa44b2138a8a1235128baa4a3 # branch mesa-26.2.7-wsl

PREFIX=/opt/weaselway
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${XDG_CACHE_HOME:-${HOME}/.cache}/weaselway-build"
KERNEL="$(uname -r)"
MODPROBE="$(command -v modprobe)"

# A WSL VM defaults to 8 GiB of RAM. Mesa and KWin peak at several GB while
# compiling, and with one job per CPU thread the VM starts swapping and the
# build appears to hang at the same place every time. Cap the job count;
# WEASELWAY_BUILD_JOBS overrides it on a machine with more memory.
BUILD_JOBS="${WEASELWAY_BUILD_JOBS:-$(nproc)}"
if [ "${BUILD_JOBS}" -gt 8 ]; then
    BUILD_JOBS=8
fi

die() {
    echo "error: $*" >&2
    exit 1
}

step() {
    echo
    echo "==> $*"
}

# KWin reports the whole buffer as damaged in every frame without weaselway's
# patch, so every frame is read back in full. The patch is a backport per KWin
# release; --without-kwin-patch runs the distro package instead.
WITH_KWIN_PATCH=1
case "${1:-}" in
"") ;;
--with-kwin-patch) WITH_KWIN_PATCH=1 ;;
--without-kwin-patch) WITH_KWIN_PATCH=0 ;;
*) die "usage: $0 [--with-kwin-patch|--without-kwin-patch]" ;;
esac

[ "$(id -u)" != 0 ] || die "run this as your user, not as root; it uses sudo where needed"
[ -n "${WSL_DISTRO_NAME:-}" ] || die "this is not a WSL distro"
[ -d /run/systemd/system ] || die "systemd is not running; set systemd=true under [boot] in /etc/wsl.conf and run wsl --terminate ${WSL_DISTRO_NAME}"
command -v sudo > /dev/null || die "sudo is not installed; as root, run: pacman -S sudo"

mkdir -p "${BUILD}"

# First, so that an unsupported kernel fails before anything is installed.
step "Downloading dxgdrm ${DXGDRM_VERSION} for ${KERNEL}"
curl -fL --progress-bar -o "${BUILD}/dxgdrm.tar.gz" \
    "https://github.com/weaselway/dxgdrm/releases/download/${DXGDRM_VERSION}/dxgdrm-${KERNEL}.tar.gz" ||
    die "dxgdrm ${DXGDRM_VERSION} has no module for the kernel ${KERNEL}"
curl -fsSL -o "${BUILD}/dxgdrm_drm.h" \
    "https://raw.githubusercontent.com/weaselway/dxgdrm/${DXGDRM_VERSION}/dxgdrm_drm.h"

step "Installing packages"
# Plasma without a display manager: ww-start-session creates the logind session
# itself, like a display manager would, and Plasma's login manager, SDDM and
# GDM have no place in WSL. plasma-desktop pulls plasma-workspace, kwin,
# breeze and polkit-kde-agent.
sudo pacman -S --needed --noconfirm --disable-download-timeout \
    plasma-desktop plasma-workspace plasma-integration kglobalacceld \
    xdg-desktop-portal xdg-desktop-portal-kde konsole \
    pipewire pipewire-pulse wireplumber \
    glib2 openssl libpng libdrm libglvnd mesa wayland wayland-protocols \
    vulkan-icd-loader mesa-utils vulkan-tools libva kbd kmod base-devel git curl unzip

# freerdp is deliberately not installed from the repository: the patched
# package is built below, and asking for the repository one would replace it.
# Build dependencies: the Arch PKGBUILD makedepends of freerdp, mesa and kwin,
# without the Rust and documentation ones weaselway does not build.
sudo pacman -S --needed --noconfirm --disable-download-timeout \
    cmake ninja meson python-mako python-yaml python-packaging python-ply python-pycparser \
    llvm clang glslang spirv-tools directx-headers libdisplay-info libxml2 \
    libx11 libxcb libxext libxrandr libxshmfence libxxf86vm systemd-libs zstd lm_sensors \
    expat libelf xcb-util-keysyms \
    alsa-lib ffmpeg icu krb5 libcbor libfido2 libjpeg-turbo libp11 libpulse libusb libwebp \
    libxdamage libxinerama \
    pam pkcs11-helper sdl3 sdl3_ttf sndio xorgproto \
    extra-cmake-modules kdoctools krunner plasma-wayland-protocols vulkan-headers xorg-xwayland

step "Building FreeRDP with weaselway's patches"
# Two patches on top of the repository's FreeRDP:
# - nix/freerdp-dsp-ffmpeg-pcm-s16.patch, the signed 16-bit PCM fix, without
#   which audio and the microphone are garbled;
# - freerdp-drdynvc-server-start.patch, needed from FreeRDP 3.33 on: its
#   drdynvc_server_start() returns an error unconditionally, so a server cannot
#   activate dynamic channels (gfxredir) and the client disconnects while
#   activating.
FREERDP_DIR="${BUILD}/freerdp"
rm -rf "${FREERDP_DIR}"
mkdir -p "${FREERDP_DIR}"
git clone --depth 1 https://gitlab.archlinux.org/archlinux/packaging/packages/freerdp.git \
    "${FREERDP_DIR}/packaging"
(
    cd "${FREERDP_DIR}/packaging"
    PKGVER="$(sed -n 's/^pkgver=//p' PKGBUILD)"
    EPOCH="$(sed -n 's/^epoch=//p' PKGBUILD)"
    BASE_PKGREL="$(sed -n 's/^pkgrel=//p' PKGBUILD)"
    # A sub-release above the repository package, so an upgrade does not replace
    # the patched one until FreeRDP gets a new version.
    PATCHED_PKGREL="${BASE_PKGREL}.3"
    PATCHED_VERSION="${EPOCH:+${EPOCH}:}${PKGVER}-${PATCHED_PKGREL}"
    INSTALLED_VERSION="$(pacman -Q freerdp 2> /dev/null | awk '{print $2}')"
    if [ "${INSTALLED_VERSION}" = "${PATCHED_VERSION}" ]; then
        echo "the patched freerdp ${PATCHED_VERSION} is installed already"
        exit 0
    fi
    cp "${REPO}/nix/freerdp-dsp-ffmpeg-pcm-s16.patch" .
    cp "${REPO}/arch/freerdp-drdynvc-server-start.patch" .
    cat >> PKGBUILD <<EOF

# weaselway: the signed-PCM and drdynvc-server fixes
pkgrel=${PATCHED_PKGREL}
source+=(freerdp-dsp-ffmpeg-pcm-s16.patch freerdp-drdynvc-server-start.patch)
sha512sums+=('SKIP' 'SKIP')
b2sums+=('SKIP' 'SKIP')
prepare() {
  patch -d "\$pkgname" -p1 < freerdp-dsp-ffmpeg-pcm-s16.patch
  patch -d "\$pkgname" -p1 < freerdp-drdynvc-server-start.patch
}
EOF
    # --syncdeps installs FreeRDP's runtime dependencies from the PKGBUILD
    # (libxdamage and libxinerama among them), which a minimal Arch WSL image
    # does not have.
    MAKEFLAGS="-j${BUILD_JOBS}" CMAKE_BUILD_PARALLEL_LEVEL="${BUILD_JOBS}" \
        makepkg --syncdeps --noconfirm --nocheck
    FREERDP_PACKAGE="$(find . -maxdepth 1 -name 'freerdp-*.pkg.tar.*' ! -name '*debug*' -print -quit)"
    [ -n "${FREERDP_PACKAGE}" ] || die "the patched freerdp package was not built"
    sudo pacman -U --noconfirm "${FREERDP_PACKAGE}"
)

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
meson compile -C "${BUILD}/mesa-build" -j "${BUILD_JOBS}"
sudo meson install -C "${BUILD}/mesa-build" --no-rebuild --quiet

# Ahead of the distro's Mesa: ld.so.conf.d is read in name order, and the first
# directory with a library wins. The distro's Mesa stays installed for pacman.
echo "${PREFIX}/mesa/lib" | sudo tee /etc/ld.so.conf.d/00-weaselway-mesa.conf > /dev/null
sudo ldconfig
sudo mkdir -p /etc/vulkan/icd.d
sudo ln -sf "${PREFIX}/mesa/share/vulkan/icd.d/dzn_icd.x86_64.json" /etc/vulkan/icd.d/

# Intel's Windows driver needs libedit.so.2, which Arch ships as libedit.so.0.
# Without it d3d12 falls back to llvmpipe on Intel GPUs.
if [ -e /usr/lib/libedit.so.0 ] && [ ! -e /usr/lib/libedit.so.2 ]; then
    sudo ln -s libedit.so.0 /usr/lib/libedit.so.2
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
# A checkout on the Windows drive has CRLF line endings, and a script installed
# with "#!/usr/bin/env bash\r" does not run.
sudo sed -i 's/\r$//' /usr/local/bin/ww-start-session /usr/local/bin/ww-start-viewer \
    /usr/local/bin/ww-install-viewer-link "${PREFIX}/libexec/prep-session.sh"

# systemd user manager environment: the tools weaselwayd and the session's
# services read, and the default session.
sudo install -d -m 0755 /etc/environment.d
sudo install -Dm644 "${REPO}/environment.d/10-weaselway.conf" /etc/environment.d/10-weaselway.conf
sudo sed -i 's/\r$//' /etc/environment.d/10-weaselway.conf
# VA-API looks for d3d12_drv_video.so in the distro's directory otherwise.
echo "LIBVA_DRIVERS_PATH=${PREFIX}/mesa/lib/dri" | sudo tee /etc/environment.d/11-weaselway-arch.conf > /dev/null
sudo tee /etc/environment.d/12-weaselway-session.conf > /dev/null <<'EOF'
# Installed by weaselway: Plasma is the session ww-start-session starts.
WEASELWAY_DEFAULT_SESSION=plasma
EOF

sudo install -Dm644 "${REPO}/pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf" \
    /etc/pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf
sudo sed -i 's/\r$//' /etc/pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf

# Login shells: WSL points PULSE_SERVER at WSLg's PulseAudio, which would hide
# PipeWire, and ww-start-viewer needs to know where the viewer is.
sudo tee /etc/profile.d/weaselway.sh > /dev/null <<EOF
unset PULSE_SERVER
export WEASELWAY_VIEWER="\${WEASELWAY_VIEWER:-${PREFIX}/viewer/sdl-freerdp.exe}"
export WEASELWAY_DEFAULT_SESSION="\${WEASELWAY_DEFAULT_SESSION:-plasma}"
EOF

# CachyOS's user shell is fish, which does not read /etc/profile.d.
sudo install -d -m 0755 /etc/fish/conf.d
sudo tee /etc/fish/conf.d/weaselway.fish > /dev/null <<EOF
set -gx WEASELWAY_VIEWER ${PREFIX}/viewer/sdl-freerdp.exe
set -gx WEASELWAY_DEFAULT_SESSION plasma
set -e PULSE_SERVER
EOF

# weaselwayd creates the viewer's input devices through uinput, as the user.
sudo tee /etc/udev/rules.d/70-weaselway-uinput.rules > /dev/null <<'EOF'
KERNEL=="uinput", SUBSYSTEM=="misc", GROUP="input", MODE="0660"
EOF
sudo usermod -aG render,video,input "$(id -un)"

# OpenSSL 4 keeps MD4 and RC4 in the legacy provider, and FreeRDP's RDP
# security initialises them even when the session ends up on TLS. Without the
# provider the activation fails and the client disconnects.
sudo tee /etc/ssl/weaselway-openssl.cnf > /dev/null <<'EOF'
# Installed by weaselway: OpenSSL's legacy provider, for FreeRDP's MD4 and RC4.
openssl_conf = openssl_init

[openssl_init]
providers = provider_sect

[provider_sect]
default = default_sect
legacy = legacy_sect

[default_sect]
activate = 1

[legacy_sect]
activate = 1
EOF
sudo install -d -m 0755 /etc/systemd/user/weaselwayd.service.d
sudo tee /etc/systemd/user/weaselwayd.service.d/openssl-legacy.conf > /dev/null <<'EOF'
[Service]
Environment=OPENSSL_CONF=/etc/ssl/weaselway-openssl.cnf
EOF

# Loads dxgdrm, evdev, uinput and vhci_hcd, takes /tmp/.X11-unix back from WSLg
# and mounts WSLg's shared memory, once per boot.
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
ExecStart=-${MODPROBE} -abq -d ${PREFIX}/dxgdrm drm
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
# WSL has no TPM. Arch has no tpm-udev.path, but CachyOS and future packages may.
if systemctl list-unit-files --no-legend tpm-udev.path 2> /dev/null | grep -q tpm-udev; then
    sudo systemctl mask tpm-udev.path
fi

if [ "${WITH_KWIN_PATCH}" = 1 ]; then
    step "Building KWin with the damage-clips patch"
    # KWin tells the kernel that the whole buffer changed in every frame, so
    # weaselwayd reads back 100% of the screen each time. The backport sets
    # FB_DAMAGE_CLIPS, and the readback follows the actual damage (a few
    # percent while the desktop is idle).
    KWIN_DIR="${BUILD}/kwin"
    rm -rf "${KWIN_DIR}"
    mkdir -p "${KWIN_DIR}"
    git clone --depth 1 https://gitlab.archlinux.org/archlinux/packaging/packages/kwin.git \
        "${KWIN_DIR}/packaging"
    (
        cd "${KWIN_DIR}/packaging"
        KWIN_VERSION="$(sed -n 's/^pkgver=//p' PKGBUILD)"
        KWIN_BASE_PKGREL="$(sed -n 's/^pkgrel=//p' PKGBUILD)"
        KWIN_PKGREL=$((KWIN_BASE_PKGREL + 1))
        case "${KWIN_VERSION}" in
        6.6.*) KWIN_PATCH="${REPO}/nix/kwin-6.6-fb-damage-clips.patch" ;;
        6.7.*) KWIN_PATCH="${REPO}/nix/kwin-6.7-fb-damage-clips.patch" ;;
        6.8.*) KWIN_PATCH="${REPO}/nix/kwin-6.8-fb-damage-clips.patch" ;;
        *) die "no FB_DAMAGE_CLIPS backport for KWin ${KWIN_VERSION}; run with --without-kwin-patch or add one to nix/" ;;
        esac
        PATCHED_VERSION="${KWIN_VERSION}-${KWIN_PKGREL}"
        if [ "$(pacman -Q kwin 2> /dev/null | awk '{print $2}')" = "${PATCHED_VERSION}" ]; then
            echo "the patched kwin ${PATCHED_VERSION} is installed already"
            exit 0
        fi
        cp "${KWIN_PATCH}" .
        # Arch's PKGBUILD applies one patch from invent.kde.org. Fetch it
        # locally, so the build does not depend on that host answering today.
        ARCH_PATCH_URL="$(grep -o 'https://invent.kde.org/[^)]*\.patch' PKGBUILD | head -1 || true)"
        ARCH_PATCH=""
        if [ -n "${ARCH_PATCH_URL}" ]; then
            ARCH_PATCH="$(basename "${ARCH_PATCH_URL}")"
            if curl -fsSL -o "${ARCH_PATCH}" "${ARCH_PATCH_URL}"; then
                sed -i "s|${ARCH_PATCH_URL}|${ARCH_PATCH}|" PKGBUILD
                sed -i 's/667d62447fdbc9fe0a760efee7c44e96c988384a5ad86fac49be7576d52010c2/SKIP/' PKGBUILD
            else
                echo "warning: cannot download the Arch kwin patch, building without it" >&2
                ARCH_PATCH=""
                grep -vF "${ARCH_PATCH_URL}" PKGBUILD > PKGBUILD.tmp && mv PKGBUILD.tmp PKGBUILD
                grep -vF '667d62447fdbc9fe0a760efee7c44e96c988384a5ad86fac49be7576d52010c2' PKGBUILD > PKGBUILD.tmp && mv PKGBUILD.tmp PKGBUILD
            fi
        fi
        {
            echo
            echo "# weaselway: report frame damage to the kernel through FB_DAMAGE_CLIPS"
            echo "pkgrel=${KWIN_PKGREL}"
            echo "source+=($(basename "${KWIN_PATCH}"))"
            echo "sha256sums+=('SKIP')"
            echo "prepare() {"
            if [ -n "${ARCH_PATCH}" ]; then
                echo "  patch -d \"\$pkgname-\$pkgver\" -p1 < ${ARCH_PATCH}"
            fi
            echo "  patch -d \"\$pkgname-\$pkgver\" -p1 < $(basename "${KWIN_PATCH}")"
            echo "}"
        } >> PKGBUILD
        # --skippgpcheck skips the KDE tarball's signature, which needs the KDE
        # packagers' keys in the user's keyring. The sha256 sums still check the
        # tarball, and the backport's entry is SKIP because it is a local file.
        MAKEFLAGS="-j${BUILD_JOBS}" CMAKE_BUILD_PARALLEL_LEVEL="${BUILD_JOBS}" \
            makepkg --noconfirm --nocheck --skippgpcheck
        KWIN_PACKAGE="$(find . -maxdepth 1 -name 'kwin-[0-9]*.pkg.tar.*' ! -name '*debug*' -print -quit)"
        [ -n "${KWIN_PACKAGE}" ] || die "the patched kwin package was not built"
        sudo pacman -U --noconfirm "${KWIN_PACKAGE}"
    )
fi

cat <<EOF

Done. Restart the distro so that the module, the groups and the environment
take effect, in PowerShell:

    wsl --terminate ${WSL_DISTRO_NAME}

Then start the distro again from the Windows desktop (not over SSH) and run:

    ww-start-session          # Plasma is the default session
    ww-start-viewer
EOF

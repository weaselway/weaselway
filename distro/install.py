#!/usr/bin/env python3
"""Install weaselway into an Ubuntu or Arch Linux (and CachyOS) WSL distro.

The desktop (GNOME, Plasma or Hyprland) and PipeWire come from the distro,
with weaselway's patches built into the compositor packages. The patched Mesa
and weaselwayd are compiled here; the dxgdrm module, the Windows viewer and the
Bluetooth modules come from GitHub releases. See distro/README.md.

Run it from a checkout of this repository, as your user; it uses sudo where
needed. Options that are not given on the command line are asked for.
Running it again rebuilds and reinstalls, e.g. after a WSL kernel update or
after the distro updated a patched package.
"""

from __future__ import annotations

import argparse
import enum
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from abc import ABC, abstractmethod
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Final, NoReturn

# The versions the NixOS image ships (flake.lock, nix/overlay.nix).
DXGDRM_VERSION: Final = "v1.4.0"
VIEWER_VERSION: Final = "v1.3.0"
MESA_REV: Final = "8aa6a89c3da6bc894f8baff96320be5150169fcf"  # branch mesa-26.2.7-wsl

# The runtime branches the Flatpak GL extension is built for; apps on other
# branches get the runtime's own Mesa.
FLATPAK_BRANCHES: Final = ("25.08", "26.08")
# Intel's Windows driver needs libedit.so.2, which the Flatpak runtimes lack.
LIBEDIT_URL: Final = "https://thrysoee.dk/editline/libedit-20260512-3.1.tar.gz"
LIBEDIT_SHA256: Final = "432d5e7ea8b0116dd39f2eca7bc11d0eed77faa6b77ea526ace89907c23ea4a0"

# Built for every supported kernel by release-image.yml. Not pinned: the
# modules depend on the kernel release only.
BLUETOOTH_URL: Final = "https://github.com/weaselway/weaselway/releases/latest/download/wsl-bluetooth-{kernel}.tar.gz"

PREFIX: Final = Path("/opt/weaselway")
REPO: Final = Path(__file__).resolve().parent.parent
NIX: Final = REPO / "nix"
BUILD: Final = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "weaselway-build"
KERNEL: Final = os.uname().release

# Marks files this script writes, so it does not overwrite a user's own.
MARKER: Final = "Installed by weaselway (distro/install.py)"


class InstallError(Exception):
    pass


def die(message: str) -> NoReturn:
    raise InstallError(message)


def step(message: str) -> None:
    print(f"\n==> {message}", flush=True)


def run(
    *args: str | Path,
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
    check: bool = True,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    argv = [str(arg) for arg in args]
    full_env = {**os.environ, **env} if env else None
    return subprocess.run(argv, cwd=cwd, env=full_env, check=check, text=True, capture_output=capture)


def output(*args: str | Path, cwd: Path | None = None) -> str:
    return run(*args, cwd=cwd, capture=True).stdout.strip()


def sudo(*args: str | Path, env: Mapping[str, str] | None = None) -> None:
    # sudo resets the environment, so variables go on its command line.
    assignments = [f"{key}={value}" for key, value in (env or {}).items()]
    run("sudo", *assignments, *args)


def write_root(path: Path | str, content: str, mode: int = 0o644) -> None:
    with tempfile.NamedTemporaryFile("w", delete=False) as file:
        file.write(content)
    try:
        sudo("install", "-D", f"-m{mode:o}", file.name, path)
    finally:
        os.unlink(file.name)


def install_file(source: Path, destination: Path | str, mode: int = 0o644) -> None:
    # A checkout on the Windows drive has CRLF line endings, and a script with
    # "#!/usr/bin/env bash\r" does not run.
    write_root(destination, source.read_text().replace("\r\n", "\n"), mode)


def download(url: str, destination: Path, missing: str) -> None:
    print(f"downloading {url}", flush=True)
    try:
        with urllib.request.urlopen(url) as response, destination.open("wb") as file:
            shutil.copyfileobj(response, file)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            die(missing)
        raise


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def unit_exists(name: str) -> bool:
    # Exits 1 when nothing matches.
    return bool(run("systemctl", "list-unit-files", "--no-legend", name, check=False, capture=True).stdout.strip())


# --- options ------------------------------------------------------------------


class Desktop(enum.StrEnum):
    GNOME = "gnome"
    KDE = "kde"
    HYPRLAND = "hyprland"

    @property
    def session(self) -> str:
        """The ww-start-session argument that starts this desktop."""
        return {Desktop.GNOME: "gnome", Desktop.KDE: "plasma", Desktop.HYPRLAND: "custom"}[self]


@dataclass(frozen=True)
class Options:
    desktop: Desktop
    flatpak: bool
    bluetooth: bool
    jobs: int
    quick_build: bool
    leave_idle_to_windows: bool


def ask(question: str, choices: Sequence[str], default: str) -> str:
    while True:
        answer = input(f"{question} [{'/'.join(choices)}] (default {default}): ").strip().lower()
        if not answer:
            return default
        matches = [choice for choice in choices if choice.startswith(answer)]
        if len(matches) == 1:
            return matches[0]
        print(f"please answer one of {', '.join(choices)}")


def ask_bool(question: str) -> bool:
    return ask(question, ["yes", "no"], "no") == "yes"


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Install weaselway into an Ubuntu or Arch Linux WSL distro. See distro/README.md.",
        epilog="Options that are not given are asked for interactively.",
    )
    parser.add_argument("--desktop", choices=[d.value for d in Desktop], help="the desktop to install")
    parser.add_argument(
        "--flatpak",
        action=argparse.BooleanOptionalAction,
        help="install Flatpak and build Mesa for Flatpak apps (about 4 GB and 15 minutes more)",
    )
    parser.add_argument(
        "--bluetooth",
        action=argparse.BooleanOptionalAction,
        help="install bluez and Bluetooth modules with rfkill for adapters attached with usbipd",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(os.cpu_count() or 1, 8),
        help="parallel compile jobs (default: CPU threads, at most 8, since a WSL VM with 8 GiB swaps with more)",
    )
    parser.add_argument(
        "--leave-idle-to-windows",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="keep the desktop from locking and turning off the screen when idle, and leave both to Windows (default)",
    )
    parser.add_argument(
        "--quick-build",
        action="store_true",
        help="compile without optimization, for CI: faster to build, slower to run",
    )
    return parser


def complete_options(parser: argparse.ArgumentParser, args: argparse.Namespace) -> Options:
    missing = [name for name in ("desktop", "flatpak", "bluetooth") if getattr(args, name) is None]
    if missing and not sys.stdin.isatty():
        parser.error(f"not interactive, give {', '.join('--' + name for name in missing)}")
    if args.desktop is None:
        args.desktop = ask("Desktop?", [d.value for d in Desktop], Desktop.GNOME.value)
    if args.flatpak is None:
        args.flatpak = ask_bool("Build Mesa for Flatpak apps (about 4 GB and 15 minutes more)?")
    if args.bluetooth is None:
        args.bluetooth = ask_bool("Install Bluetooth support for adapters attached with usbipd?")
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")

    return Options(
        desktop=Desktop(args.desktop),
        flatpak=args.flatpak,
        bluetooth=args.bluetooth,
        jobs=args.jobs,
        quick_build=args.quick_build,
        leave_idle_to_windows=args.leave_idle_to_windows,
    )


# --- patched packages ---------------------------------------------------------

Version = tuple[int, ...]


def upstream_version(version: str) -> Version:
    """The upstream part of a package version.

    >>> upstream_version("4:6.6.6-0ubuntu0.1"), upstream_version("0.53.3+ds-4"), upstream_version("51.1")
    ((6, 6, 6), (0, 53, 3), (51, 1))
    """
    version = version.split(":", 1)[-1]
    match = re.match(r"\d+(\.\d+)*", version)
    if not match:
        die(f"cannot parse the version {version}")
    return tuple(int(part) for part in match.group().split("."))


@dataclass(frozen=True)
class Backport:
    """weaselway patches for the upstream versions since <= v < until."""

    since: Version
    until: Version
    patches: tuple[str, ...]


# Patch files in nix/, the same ones nix/overlay.nix applies. A version outside
# every range stops the script until a backport is added.
BACKPORTS: Final[Mapping[str, Sequence[Backport]]] = {
    # The overview keeps its old size when the viewer resizes the display, and a
    # failed device open closed fd 0, which hung GNOME when WSL's vgem came back.
    # From 50.4 on, each monitor's viewport stays inside its framebuffer, which
    # Intel Gen9 GPUs need with several monitors; older releases lack the helper.
    "mutter": [
        Backport((50,), (50, 4), ("mutter-stage-relayout.patch", "mutter-kms-fd0.patch")),
        Backport(
            (50, 4),
            (52,),
            ("mutter-stage-relayout.patch", "mutter-kms-fd0.patch", "mutter-viewport-in-fb.patch"),
        ),
    ],
    # KWin reports its damage (FB_DAMAGE_CLIPS), so weaselwayd does not read back
    # the whole screen every frame.
    "kwin": [
        Backport((6, 6), (6, 7), ("kwin-6.6-fb-damage-clips.patch",)),
        Backport((6, 7), (6, 8), ("kwin-6.7-fb-damage-clips.patch",)),
        Backport((6, 8), (6, 9), ("kwin-6.8-fb-damage-clips.patch",)),
    ],
    # Hyprland follows the viewer's window size.
    "aquamarine": [
        Backport((0, 10), (0, 11), ("aquamarine-0.10-mode-changes.patch",)),
        Backport((0, 11), (0, 17), ("aquamarine-0.11-mode-changes.patch",)),
    ],
    "hyprland": [
        Backport((0, 53), (0, 56), ("hyprland-0.55-modeset-buffer-size.patch",)),
        Backport((0, 56), (0, 57), ("hyprland-0.56-modeset-buffer-size.patch",)),
        Backport((0, 57), (0, 58), ("hyprland-0.57-modeset-buffer-size.patch",)),
    ],
    # FreeRDP's FFmpeg backend treats 16-bit PCM as unsigned, which garbles audio
    # both ways. Only where FreeRDP links FFmpeg, see Distro.freerdp_uses_ffmpeg.
    "freerdp": [Backport((3,), (4,), ("freerdp-dsp-ffmpeg-pcm-s16.patch",))],
}

COMPOSITOR_PACKAGES: Final[Mapping[Desktop, Sequence[str]]] = {
    Desktop.GNOME: ["mutter"],
    Desktop.KDE: ["kwin"],
    Desktop.HYPRLAND: ["aquamarine", "hyprland"],
}


def backport_patches(package: str, version: str) -> list[Path]:
    """The patches for a version of a package, or an error if there are none.

    >>> [p.name for p in backport_patches("kwin", "4:6.6.6-0ubuntu0.1")]
    ['kwin-6.6-fb-damage-clips.patch']
    >>> [p.name for p in backport_patches("mutter", "50.3-0ubuntu1")]
    ['mutter-stage-relayout.patch', 'mutter-kms-fd0.patch']
    >>> [p.name for p in backport_patches("mutter", "51.0-1")][-1]
    'mutter-viewport-in-fb.patch'
    >>> [p.name for p in backport_patches("aquamarine", "0.16.0")]
    ['aquamarine-0.11-mode-changes.patch']
    >>> backport_patches("kwin", "6.9.0")  # doctest: +IGNORE_EXCEPTION_DETAIL
    Traceback (most recent call last):
    InstallError: weaselway has no patch for kwin 6.9.0 yet
    """
    upstream = upstream_version(version)
    for backport in BACKPORTS[package]:
        if backport.since <= upstream < backport.until:
            return [NIX / name for name in backport.patches]
    die(
        f"weaselway has no patch for {package} {version} yet; "
        f"add a backport to nix/ and to BACKPORTS in {Path(__file__).name}"
    )


def patch_digest(patches: Iterable[Path]) -> str:
    return hashlib.sha256(b"".join(patch.read_bytes() for patch in patches)).hexdigest()[:16]


def source_tree(directory: Path, patch: Path) -> Path:
    """The directory below an unpacked source package the patch applies to."""
    for candidate in sorted(directory.iterdir()):
        if not candidate.is_dir() or candidate.is_symlink():
            continue
        dry_run = run("patch", "-p1", "--dry-run", "-s", "-f", "-i", patch, cwd=candidate, check=False, capture=True)
        if dry_run.returncode == 0:
            return candidate
    die(f"{patch.name} applies to no source tree in {directory}")


# --- distros ------------------------------------------------------------------


class Distro(ABC):
    name: str
    freerdp_uses_ffmpeg: bool
    base_packages: Sequence[str]
    desktop_packages: Mapping[Desktop, Sequence[str]]
    flatpak_packages: Sequence[str] = ("flatpak",)
    bluetooth_packages: Sequence[str]

    @abstractmethod
    def install_packages(self, options: Options) -> None:
        """The desktop, PipeWire and what the builds need."""

    @abstractmethod
    def build_patched(self, package: str, options: Options) -> None:
        """Rebuild a distro package with weaselway's patches and install it."""

    def packages(self, options: Options) -> list[str]:
        packages = [*self.base_packages, *self.desktop_packages[options.desktop]]
        if options.flatpak:
            packages += self.flatpak_packages
        if options.bluetooth:
            packages += self.bluetooth_packages
        return packages

    def patched_packages(self, options: Options) -> list[str]:
        packages = ["freerdp"] if self.freerdp_uses_ffmpeg else []
        return packages + list(COMPOSITOR_PACKAGES[options.desktop])

    @staticmethod
    def stamp(package: str) -> Path:
        """Records the version and the patches of the last patched build."""
        return BUILD / "packages" / f"{package}.stamp"

    def up_to_date(self, package: str, target: str, patches: Iterable[Path], installed: bool) -> bool:
        stamp = self.stamp(package)
        current = installed and stamp.exists() and stamp.read_text() == f"{target} {patch_digest(patches)}"
        if current:
            print(f"the patched {package} {target} is installed already")
        return current

    @staticmethod
    def build_env(options: Options) -> dict[str, str]:
        return {
            "MAKEFLAGS": f"-j{options.jobs}",
            "CMAKE_BUILD_PARALLEL_LEVEL": str(options.jobs),
        }


class Ubuntu(Distro):
    name = "Ubuntu"
    # Ubuntu's freerdp3 is built without FFmpeg, and its PCM is right.
    freerdp_uses_ffmpeg = False
    base_packages = (
        "pipewire-pulse", "wireplumber", "upower", "mesa-utils", "vulkan-tools", "kbd", "curl", "git", "unzip",
        "build-essential", "pkg-config", "freerdp3-dev", "libwinpr3-dev", "libglib2.0-dev",
        "libpng-dev", "libwayland-dev", "wayland-protocols", "libgbm-dev", "libdrm-dev", "libegl-dev",
        "libgles-dev",
    )  # fmt: skip
    desktop_packages = {
        Desktop.GNOME: (
            "gnome-session", "gnome-shell", "gnome-terminal", "nautilus", "gnome-control-center",
            "gnome-text-editor", "gnome-backgrounds", "xdg-desktop-portal-gnome",
        ),
        Desktop.KDE: (
            "plasma-desktop", "plasma-workspace", "kwin-wayland", "konsole", "dolphin", "xdg-desktop-portal-kde",
            # Keeps the apps' sleep and idle inhibitors, which weaselwayd reads.
            "powerdevil",
        ),
        Desktop.HYPRLAND: ("hyprland", "kitty", "xdg-desktop-portal-hyprland"),
    }  # fmt: skip
    bluetooth_packages = ("bluez",)

    # What the desktops recommend that does not work in WSL: a display manager
    # would take the display, NetworkManager the network.
    blocked = (
        "gdm3", "sddm", "network-manager", "modemmanager", "wpasupplicant", "cups", "cups-daemon", "bluez",
        "plymouth", "snapd",
    )  # fmt: skip

    def apt_get(self, *args: str) -> None:
        sudo("apt-get", *args, env={"DEBIAN_FRONTEND": "noninteractive"})

    def install_packages(self, options: Options) -> None:
        blocked = [p for p in self.blocked if not (options.bluetooth and p in self.bluetooth_packages)]
        write_root(
            "/etc/apt/preferences.d/weaselway",
            f"# {MARKER}: what the desktops recommend that does not work in WSL.\n"
            f"Package: {' '.join(blocked)}\nPin: release *\nPin-Priority: -1\n",
        )
        # The WSL image ships snapd, and the pin only stops new installs. Snaps
        # bring their own Mesa without d3d12 on dxgdrm.
        self.apt_get("purge", "-y", "--auto-remove", "snapd")
        # Mesa's and the compositors' build dependencies come from their source
        # packages.
        sudo("sed", "-i", "s/^Types: deb$/Types: deb deb-src/", "/etc/apt/sources.list.d/ubuntu.sources")
        self.apt_get("update")
        self.apt_get("install", "-y", *self.packages(options))
        self.apt_get("build-dep", "-y", "mesa")

    def build_patched(self, package: str, options: Options) -> None:
        work = BUILD / "packages" / package
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        run("apt-get", "source", "-q", package, cwd=work)
        (tree,) = [d for d in work.iterdir() if d.is_dir()]
        version = output("dpkg-parsechangelog", "-S", "Version", cwd=tree)
        patches = backport_patches(package, version)
        target = f"{version}+weaselway1"

        # The binary packages of this source that are installed.
        installed = {}
        query = "${db:Status-Status} ${Package} ${source:Package} ${Version}\n"
        for line in output("dpkg-query", "-W", "-f", query).splitlines():
            status, name, source, installed_version = line.split()
            if status == "installed" and source == package:
                installed[name] = installed_version
        if self.up_to_date(package, target, patches, set(installed.values()) == {target}):
            return

        for patch in patches:
            run("patch", "-p1", "-i", patch, cwd=tree)
        distribution = output("dpkg-parsechangelog", "-S", "Distribution", cwd=tree)
        changelog = tree / "debian" / "changelog"
        date = datetime.now(UTC).strftime("%a, %d %b %Y %H:%M:%S +0000")
        entry = (
            f"{package} ({target}) {distribution}; urgency=medium\n\n"
            f"  * weaselway: {', '.join(p.name for p in patches)}\n\n"
            f" -- weaselway <weaselway@localhost>  {date}\n\n"
        )
        changelog.write_text(entry + changelog.read_text())

        self.apt_get("build-dep", "-y", str(tree))
        build_options = ["nocheck", "nodoc", "noautodbgsym", f"parallel={options.jobs}"]
        if options.quick_build:
            build_options.append("noopt")
        env = {
            "DEB_BUILD_OPTIONS": " ".join(build_options),
            # On a rerun ld.so.conf has weaselway's Mesa first, and dpkg-shlibdeps
            # finds no package for its libgbm. It searches LD_LIBRARY_PATH first.
            "LD_LIBRARY_PATH": "/usr/lib/x86_64-linux-gnu",
        }
        run("dpkg-buildpackage", "-b", "-uc", "-us", cwd=tree, env=env)

        # Hyprland builds against aquamarine, and its -dev package wants the
        # library of the same version.
        def wanted(name: str) -> bool:
            return name in installed or (package == "aquamarine" and name.endswith("-dev"))

        debs = [deb for deb in sorted(work.glob("*.deb")) if wanted(output("dpkg-deb", "-f", deb, "Package"))]
        if not debs:
            die(f"none of the packages built from {package} is installed")
        self.apt_get("install", "-y", "--allow-downgrades", *[str(deb) for deb in debs])
        self.stamp(package).write_text(f"{target} {patch_digest(patches)}")


class Arch(Distro):
    name = "Arch Linux"
    # Arch's FreeRDP links FFmpeg, so it needs the PCM patch.
    freerdp_uses_ffmpeg = True
    # freerdp is not listed: the patched package is built from Arch's PKGBUILD,
    # and asking for the repository's would replace it.
    base_packages = (
        "pipewire", "pipewire-pulse", "wireplumber", "upower", "glib2", "libpng", "libdrm", "libglvnd", "mesa",
        "wayland", "wayland-protocols", "vulkan-icd-loader", "mesa-utils", "vulkan-tools", "libva", "kbd", "kmod",
        "base-devel", "git", "curl", "unzip",
        # Mesa's build dependencies, from Arch's mesa PKGBUILD without what
        # weaselway does not build.
        "cmake", "ninja", "meson", "python-mako", "python-yaml", "python-packaging", "python-ply",
        "python-pycparser", "llvm", "clang", "glslang", "spirv-tools", "directx-headers", "libdisplay-info",
        "libxml2", "libx11", "libxcb", "libxext", "libxrandr", "libxshmfence", "libxxf86vm", "systemd-libs", "zstd",
        "lm_sensors", "expat", "libelf", "xcb-util-keysyms",
    )  # fmt: skip
    # Without a display manager: plasma-meta and the gnome group pull one.
    desktop_packages = {
        Desktop.GNOME: (
            "gnome-session", "gnome-shell", "gnome-terminal", "nautilus", "gnome-control-center",
            "gnome-text-editor", "gnome-backgrounds", "gnome-keyring", "xdg-desktop-portal-gnome",
        ),
        Desktop.KDE: (
            "plasma-desktop", "plasma-workspace", "plasma-integration", "kglobalacceld", "xdg-desktop-portal",
            "xdg-desktop-portal-kde", "konsole", "dolphin", "powerdevil",
        ),
        Desktop.HYPRLAND: ("hyprland", "kitty", "xdg-desktop-portal-hyprland"),
    }  # fmt: skip
    bluetooth_packages = ("bluez", "bluez-utils")

    def install_packages(self, options: Options) -> None:
        # Arch does not support partial upgrades, and a fresh image has no
        # package databases yet.
        sudo("pacman", "-Syu", "--needed", "--noconfirm", "--disable-download-timeout", *self.packages(options))

    def makepkg_conf(self, options: Options) -> Path:
        conf = BUILD / "makepkg.conf"
        extra = "\n# weaselway: no debug packages\nOPTIONS+=(!debug)\n"
        if options.quick_build:
            extra += '# weaselway --quick-build\nCFLAGS="-O0 -pipe"\nCXXFLAGS="$CFLAGS"\nOPTIONS+=(!lto)\n'
        conf.write_text(Path("/etc/makepkg.conf").read_text() + extra)
        return conf

    def build_patched(self, package: str, options: Options) -> None:
        work = BUILD / "packages" / package
        shutil.rmtree(work, ignore_errors=True)
        work.parent.mkdir(parents=True, exist_ok=True)
        # The packaging at the version in the repositories; Arch tags it as
        # [epoch-]pkgver-pkgrel. CachyOS rebuilds have other versions: main then.
        info = output("pacman", "-Si", package)
        repo_version = re.search(r"^Version\s*:\s*(\S+)", info, re.M)
        tag = repo_version.group(1).replace(":", "-") if repo_version else "main"
        url = f"https://gitlab.archlinux.org/archlinux/packaging/packages/{package}.git"
        clone = ["git", "-c", "advice.detachedHead=false", "clone", "-q", "--depth", "1"]
        if run(*clone, "--branch", tag, url, work, check=False).returncode != 0:
            print(f"warning: no tag {tag} in {url}, building main", file=sys.stderr)
            run(*clone, url, work)

        srcinfo: dict[str, str] = {}
        for line in output("makepkg", "--printsrcinfo", cwd=work).splitlines():
            key, _, value = line.strip().partition(" = ")
            # The first value, from pkgbase.
            srcinfo.setdefault(key, value)
        version = srcinfo["pkgver"]
        patches = backport_patches(package, version)
        # A sub-release above the repository's, so an upgrade does not replace
        # the patched package until Arch rebuilds or updates it.
        pkgrel = f"{srcinfo['pkgrel']}.1"
        epoch = f"{srcinfo['epoch']}:" if "epoch" in srcinfo else ""
        target = f"{epoch}{version}-{pkgrel}"
        installed = run("pacman", "-Q", package, check=False, capture=True).stdout.split()
        if self.up_to_date(package, target, patches, installed[1:] == [target]):
            return

        with (work / "PKGBUILD").open("a") as pkgbuild:
            pkgbuild.write(f"\n# weaselway: {', '.join(p.name for p in patches)}\npkgrel={pkgrel}\n")
        conf = self.makepkg_conf(options)
        env = {**self.build_env(options), "PKGDEST": str(work)}
        # --skippgpcheck: the signatures need the packagers' keys in the user's
        # keyring. The checksums still check the sources.
        makepkg = ["makepkg", "--config", str(conf), "--noconfirm", "--skippgpcheck"]
        # Download, extract and Arch's own prepare(), then our patches, then the
        # build on that tree.
        run(*makepkg, "--nobuild", "--syncdeps", cwd=work, env=env)
        tree = source_tree(work / "src", patches[0])
        for patch in patches:
            run("patch", "-p1", "-i", patch, cwd=tree)
        run(*makepkg, "--noextract", "--nocheck", "--force", cwd=work, env=env)

        wanted = set(output("pacman", "-Qq").split()) | {package}
        built = [p for p in sorted(work.glob("*.pkg.tar.*")) if not p.name.endswith(".sig")]
        files = [p for p in built if output("pacman", "-Qqp", p) in wanted]
        sudo("pacman", "-U", "--noconfirm", *files)
        self.stamp(package).write_text(f"{target} {patch_digest(patches)}")


def detect_distro() -> Distro:
    release: dict[str, str] = {}
    for line in Path("/etc/os-release").read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            release[key] = "".join(shlex.split(value))
    ids = {release.get("ID", ""), *release.get("ID_LIKE", "").split()}
    if "ubuntu" in ids:
        return Ubuntu()
    if "arch" in ids:
        return Arch()
    die(f"{release.get('PRETTY_NAME', 'this distro')} is not supported, only Ubuntu and Arch Linux (CachyOS)")


# --- shared steps ---------------------------------------------------------------


def mesa_flags(prefix: str, options: Options) -> list[str]:
    flags = [
        f"--prefix={prefix}",
        "--libdir=lib",
        "--buildtype=plain" if options.quick_build else "--buildtype=release",
        "-Db_ndebug=true",
        "-Dglvnd=enabled",
        "-Dplatforms=x11,wayland",
        "-Dgallium-d3d12-graphics=enabled",
        "-Dgallium-d3d12-video=enabled",
        "-Dgallium-va=enabled",
        "-Dvideo-codecs=all",
        "-Dgallium-rusticl=false",
        "-Dteflon=false",
        "-Dintel-rt=disabled",
        "-Dtools=",
    ]
    return flags


def meson_build(build: Path, source: Path, flags: Sequence[str], jobs: int) -> None:
    if (build / "build.ninja").exists():
        run("meson", "setup", "--reconfigure", build, source, *flags)
    else:
        run("meson", "setup", build, source, *flags)
    run("meson", "compile", "-C", build, "-j", str(jobs))


def fetch_mesa() -> Path:
    source = BUILD / "mesa"
    if not (source / ".git").exists():
        run("git", "init", "-q", source)
        run("git", "-C", source, "remote", "add", "origin", "https://github.com/weaselway/mesa.git")
    run("git", "-C", source, "fetch", "-q", "--depth", "1", "origin", MESA_REV)
    run("git", "-C", source, "checkout", "-q", "--force", "FETCH_HEAD")
    return source


def build_mesa(source: Path, options: Options) -> None:
    step("Building Mesa")
    # The drivers of the NixOS image: d3d12 on the GPU, llvmpipe without one.
    flags = mesa_flags(f"{PREFIX}/mesa", options) + [
        "-Dgallium-drivers=d3d12,llvmpipe,softpipe,zink",
        "-Dvulkan-drivers=microsoft-experimental,swrast",
    ]
    meson_build(BUILD / "mesa-build", source, flags, options.jobs)
    sudo("meson", "install", "-C", BUILD / "mesa-build", "--no-rebuild", "--quiet")

    # Ahead of the distro's Mesa: ld.so.conf.d is read in name order, and the
    # first directory with a library wins. The distro's Mesa stays installed.
    write_root("/etc/ld.so.conf.d/00-weaselway-mesa.conf", f"{PREFIX}/mesa/lib\n")
    sudo("ldconfig")
    sudo("mkdir", "-p", "/etc/vulkan/icd.d")
    sudo("ln", "-sf", f"{PREFIX}/mesa/share/vulkan/icd.d/dzn_icd.x86_64.json", "/etc/vulkan/icd.d/")

    # Intel's Windows driver needs libedit.so.2, which Arch ships as
    # libedit.so.0. Without it d3d12 falls back to llvmpipe on Intel GPUs.
    libraries = output("ldconfig", "-p")
    so0 = re.search(r"libedit\.so\.0 \(libc6,x86-64\) => (\S+)", libraries)
    if "libedit.so.2 " not in libraries and so0:
        sudo("ln", "-sf", "libedit.so.0", Path(so0.group(1)).with_name("libedit.so.2"))
        sudo("ldconfig")


# Runs in the SDK of a runtime branch: Mesa, then libedit for the GL extension.
FLATPAK_BUILD_SCRIPT: Final = r"""
SRC="$1" OUT="$2" GL_PREFIX="$3" JOBS="$4"
shift 4
[ -d "${OUT}/python/yaml" ] || python3 -m pip install -q --target "${OUT}/python" pyyaml
export PYTHONPATH="${OUT}/python"
if [ -f "${OUT}/mesa-build/build.ninja" ]; then
    meson setup --reconfigure "${OUT}/mesa-build" "${SRC}" "$@"
else
    meson setup "${OUT}/mesa-build" "${SRC}" "$@"
fi
meson compile -C "${OUT}/mesa-build" -j "${JOBS}"
DESTDIR="${OUT}/dest" meson install -C "${OUT}/mesa-build" --no-rebuild --quiet

# ldconfig finds libraries by SONAME, and the driver asks for libedit.so.2.
cd "${OUT}/libedit"
./configure -q --prefix="${GL_PREFIX}" --disable-static LDFLAGS="-L${OUT}"
make -s -j"${JOBS}" LT_VERSION=2:0:0
"""


def build_flatpak_extension(source: Path, options: Options) -> None:
    step(f"Building the Flatpak GL extension for {', '.join(FLATPAK_BRANCHES)}")
    # Flatpak apps bring their own Mesa, without d3d12. This builds ours in the
    # SDK of each runtime branch, against its libraries, and installs it as an
    # unmaintained extension, which Flatpak prefers over the runtime's
    # GL.default. Apps see the one for their runtime's branch. prep-session.sh
    # gives it /dev/dxg and WSL's libraries at boot.
    sudo("flatpak", "remote-add", "--if-not-exists", "flathub", "https://dl.flathub.org/repo/flathub.flatpakrepo")
    for branch in FLATPAK_BRANCHES:
        sudo("flatpak", "install", "-y", "--noninteractive", "--or-update", "flathub", f"org.freedesktop.Sdk//{branch}")

    libedit = BUILD / "libedit.tar.gz"
    if not libedit.exists() or sha256(libedit) != LIBEDIT_SHA256:
        download(LIBEDIT_URL, libedit, f"{LIBEDIT_URL} is gone")
        if sha256(libedit) != LIBEDIT_SHA256:
            die(f"{LIBEDIT_URL} does not have the expected sha256 {LIBEDIT_SHA256}")

    # Like the Mesa above, but only the drivers for WSL and without LLVM,
    # which the SDK only has as an extension: softpipe instead of llvmpipe.
    gl_prefix = "/usr/lib/x86_64-linux-gnu/GL/host"
    flags = mesa_flags(gl_prefix, options) + [
        "-Dgallium-drivers=d3d12,softpipe",
        "-Dvulkan-drivers=microsoft-experimental",
        "-Dllvm=disabled",
    ]
    for branch in FLATPAK_BRANCHES:
        out = BUILD / f"flatpak-{branch}"
        for directory in ("libedit", "dest", "stage"):
            shutil.rmtree(out / directory, ignore_errors=True)
        (out / "libedit").mkdir(parents=True)
        run("tar", "-xzf", libedit, "-C", out / "libedit", "--strip-components=1")
        # The SDK has libtinfo but no libncurses.so to link it by.
        (out / "libncurses.so").unlink(missing_ok=True)
        (out / "libncurses.so").symlink_to("/usr/lib/x86_64-linux-gnu/libtinfo.so.6")

        # The SDK lacks PyYAML, hence the network.
        run(
            "flatpak", "run", "--share=network", f"--filesystem={BUILD}", "--command=sh",
            f"org.freedesktop.Sdk//{branch}", "-euc", FLATPAK_BUILD_SCRIPT,
            "sh", source, out, gl_prefix, str(options.jobs), *flags,
        )  # fmt: skip

        # The layout of a GL extension: the vendor and ICD files at the top,
        # where Flatpak merges them with the other extensions'. wsl/ gets its
        # binds at boot.
        stage = out / "stage"
        stage.mkdir()
        dest = out / "dest" / gl_prefix.lstrip("/")
        (dest / "lib").rename(stage / "lib")
        (dest / "share").rename(stage / "share")
        (stage / "share" / "glvnd").rename(stage / "glvnd")
        (stage / "share" / "vulkan").rename(stage / "vulkan")
        shutil.rmtree(stage / "lib" / "pkgconfig", ignore_errors=True)
        for archive in (stage / "lib").glob("*.a"):
            archive.unlink()
        shutil.copy(out / "libedit" / "src" / ".libs" / "libedit.so.2.0.0", stage / "lib" / "libedit.so.2")

        # Never rm -rf the extension itself: wsl/ may hold binds of WSL's files.
        extension = f"/var/lib/flatpak/extension/org.freedesktop.Platform.GL.host/x86_64/{branch}"
        sudo("mkdir", "-p", f"{extension}/wsl/lib", f"{extension}/wsl/drivers")
        for directory in ("lib", "share", "glvnd", "vulkan"):
            sudo("rm", "-rf", f"{extension}/{directory}")
            sudo("cp", "-R", stage / directory, f"{extension}/")

    # Flatpak keeps a library cache per app and never renews it for
    # unmaintained extensions (flatpak#5948).
    for cache in (Path.home() / ".var" / "app").glob("*/.ld.so"):
        shutil.rmtree(cache, ignore_errors=True)


def install_weaselwayd() -> None:
    step("Building weaselwayd")
    run("make", "-C", REPO / "weaselwayd", f"DXGDRM_INCLUDE={BUILD}", "weaselwayd", "check")
    sudo("make", "-C", REPO / "weaselwayd", "PREFIX=/usr/local", "install")


def install_releases() -> None:
    step("Installing dxgdrm and the viewer")
    sudo("rm", "-rf", PREFIX / "dxgdrm" / "lib" / "modules" / KERNEL)
    sudo("mkdir", "-p", PREFIX / "dxgdrm")
    sudo("tar", "-C", PREFIX / "dxgdrm", "-xzf", BUILD / "dxgdrm.tar.gz")
    sudo("install", "-m644", PREFIX / "dxgdrm/lib/udev/rules.d/99-dxgdrm.rules", "/etc/udev/rules.d/")

    viewer = BUILD / "viewer.zip"
    url = f"https://github.com/weaselway/freerdp/releases/download/{VIEWER_VERSION}/freerdp-{VIEWER_VERSION}.zip"
    download(url, viewer, f"the viewer {VIEWER_VERSION} is gone from {url}")
    sudo("rm", "-rf", PREFIX / "viewer")
    sudo("unzip", "-q", viewer, "-d", PREFIX / "viewer")


def install_configuration(options: Options) -> None:
    step("Installing the scripts and the configuration")
    user = output("id", "-un")
    session = options.desktop.session

    for script in ("ww-start-session", "ww-start-viewer", "ww-install-viewer-link"):
        install_file(REPO / "bin" / f"{script}.sh", f"/usr/local/bin/{script}", 0o755)
    install_file(REPO / "libexec" / "prep-session.sh", PREFIX / "libexec" / "prep-session.sh", 0o755)

    # The systemd user manager's environment: what weaselwayd and the session's
    # services read, and the default session.
    install_file(REPO / "environment.d" / "10-weaselway.conf", "/etc/environment.d/10-weaselway.conf")
    write_root(
        "/etc/environment.d/11-weaselway-distro.conf",
        f"# {MARKER}.\n"
        "# VA-API looks for d3d12_drv_video.so in the distro's directory otherwise.\n"
        f"LIBVA_DRIVERS_PATH={PREFIX}/mesa/lib/dri\n"
        f"WEASELWAY_DEFAULT_SESSION={session}\n",
    )
    # From ubuntu/install.sh and arch/install.sh.
    sudo(
        "rm", "-f", "/etc/environment.d/11-weaselway-ubuntu.conf", "/etc/environment.d/11-weaselway-arch.conf",
        "/etc/environment.d/12-weaselway-session.conf",
    )  # fmt: skip
    install_file(
        REPO / "pipewire" / "pipewire.conf.d" / "10-weaselway-rdp-audio.conf",
        "/etc/pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf",
    )

    # Login shells: WSL points PULSE_SERVER at WSLg's PulseAudio, which would
    # hide PipeWire, and ww-start-viewer needs to know where the viewer is.
    viewer = f"{PREFIX}/viewer/sdl-freerdp.exe"
    write_root(
        "/etc/profile.d/weaselway.sh",
        f"# {MARKER}.\nunset PULSE_SERVER\n"
        f'export WEASELWAY_VIEWER="${{WEASELWAY_VIEWER:-{viewer}}}"\n'
        f'export WEASELWAY_DEFAULT_SESSION="${{WEASELWAY_DEFAULT_SESSION:-{session}}}"\n',
    )
    # fish, CachyOS's user shell, does not read /etc/profile.d.
    if shutil.which("fish"):
        write_root(
            "/etc/fish/conf.d/weaselway.fish",
            f"# {MARKER}.\nset -e PULSE_SERVER\n"
            f"set -q WEASELWAY_VIEWER; or set -gx WEASELWAY_VIEWER {viewer}\n"
            f"set -q WEASELWAY_DEFAULT_SESSION; or set -gx WEASELWAY_DEFAULT_SESSION {session}\n",
        )

    # weaselwayd creates the viewer's input devices through uinput, as the user.
    write_root(
        "/etc/udev/rules.d/70-weaselway-uinput.rules",
        'KERNEL=="uinput", SUBSYSTEM=="misc", GROUP="input", MODE="0660"\n',
    )
    sudo("usermod", "-aG", "render,video,input", user)

    # Loads dxgdrm, evdev, uinput and vhci_hcd, takes /tmp/.X11-unix back from
    # WSLg and mounts WSLg's shared memory, once per boot. It also gives the
    # Flatpak GL extension /dev/dri/dxg and WSL's libraries.
    write_root(
        "/etc/systemd/system/weaselway-prep.service",
        "[Unit]\n"
        "Description=Weaselway host preparation for the session\n"
        "Documentation=https://github.com/weaselway/weaselway\n"
        "After=local-fs.target wslg.service systemd-udevd.service\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        "RemainAfterExit=yes\n"
        f"Environment=DXGDRM_ROOT={PREFIX}/dxgdrm\n"
        f"ExecStart={PREFIX}/libexec/prep-session.sh\n\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
    )
    # On kernels without DRM core the tarball has it as a module, and logind
    # only opens DRM devices if it is loaded before logind starts.
    write_root(
        "/etc/systemd/system/modprobe@drm.service.d/weaselway.conf",
        f"[Service]\nExecStart=\nExecStart=-modprobe -abq -d {PREFIX}/dxgdrm drm\n",
    )
    # Lets weaselwayd lower its nice value to -11.
    write_root("/etc/systemd/system/user@.service.d/weaselway.conf", "[Service]\nLimitNICE=-11\n")
    # Windows suspends the machine, not the distro, and acts on a critical
    # battery, which UPower reads from Hyper-V's virtual battery.
    write_root(
        "/etc/systemd/sleep.conf.d/weaselway.conf",
        f"# {MARKER}.\n[Sleep]\nAllowSuspend=no\nAllowHibernation=no\nAllowHybridSleep=no\n"
        "AllowSuspendThenHibernate=no\n",
    )
    write_root(
        "/etc/UPower/UPower.conf.d/weaselway.conf",
        f"# {MARKER}.\n[UPower]\nCriticalPowerAction=Ignore\nAllowRiskyCriticalPowerAction=true\n",
    )
    # Only for this user: a copy in another user's session, e.g. root's after
    # wsl -u root, can't get the vsock port and restarts forever.
    write_root(
        "/etc/systemd/user/weaselwayd.service",
        "[Unit]\n"
        "Description=Weaselway display server for the Windows viewer\n"
        "Documentation=https://github.com/weaselway/weaselway\n"
        f"ConditionUser={user}\n\n"
        "[Service]\n"
        "ExecStart=/usr/local/bin/weaselwayd\n"
        "Restart=on-failure\n"
        "RestartSec=2\n\n"
        "[Install]\n"
        "WantedBy=default.target\n",
    )

    if options.desktop is Desktop.HYPRLAND:
        install_hyprland_session()

    sudo("systemctl", "daemon-reload")
    sudo("udevadm", "control", "--reload")
    sudo("systemctl", "enable", "weaselway-prep.service")
    sudo("systemctl", "--global", "enable", "weaselwayd.service")
    # WSL's unit links pulse/native in the runtime directory to WSLg's
    # PulseAudio, over pipewire-pulse's socket.
    sudo("systemctl", "--global", "mask", "wslg-session.service")
    # WSL has no TPM. The unit triggers until it hits its start limit and
    # leaves the system degraded.
    if unit_exists("tpm-udev.path"):
        sudo("systemctl", "mask", "tpm-udev.path")


def install_hyprland_session() -> None:
    # ww-start-session custom runs custom-weaselway-session. Keep one the user
    # wrote.
    path = Path("/usr/local/bin/custom-weaselway-session")
    if path.exists() and MARKER not in path.read_text(errors="replace"):
        print(f"warning: keeping your {path}; it has to start Hyprland for `ww-start-session custom`", file=sys.stderr)
        return
    write_root(
        path,
        "#!/bin/sh\n"
        f"# {MARKER}: Hyprland for `ww-start-session custom`.\n"
        "export XDG_CURRENT_DESKTOP=Hyprland\n"
        "# dxgdrm, not card0: from kernel 6.18.54.1 on card0 is WSL's vgem.\n"
        "export AQ_DRM_DEVICES=/dev/dri/by-path/platform-dxgdrm-card\n"
        'exec Hyprland "$@"\n',
        0o755,
    )


# Defaults that keep the desktop from locking and turning off the screen when
# idle, as weaselway.idle.leaveToWindows does on NixOS. The desktop's settings
# still override them.
IDLE_DEFAULTS: Final[dict[Desktop, dict[str, str]]] = {
    # Compiled into the schemas, also by later package updates. Without GDM
    # GNOME neither locks nor blanks; this keeps it from going idle at all.
    Desktop.GNOME: {
        "/usr/share/glib-2.0/schemas/90_weaselway.gschema.override": "[org.gnome.desktop.session]\nidle-delay=uint32 0\n",
    },
    Desktop.KDE: {
        "/etc/xdg/powerdevilrc": "\n".join(
            f"[{profile}][Display]\nDimDisplayWhenIdle=false\nTurnOffDisplayWhenIdle=false\n"
            for profile in ("AC", "Battery", "LowBattery")
        ),
        "/etc/xdg/kscreenlockerrc": "[Daemon]\nAutolock=false\n",
    },
}


def install_idle_defaults(options: Options) -> None:
    wanted = IDLE_DEFAULTS.get(options.desktop, {}) if options.leave_idle_to_windows else {}
    for files in IDLE_DEFAULTS.values():
        for name, content in files.items():
            path = Path(name)
            if path.exists() and MARKER not in path.read_text(errors="replace"):
                if name in wanted:
                    print(f"warning: keeping your {path}", file=sys.stderr)
            elif name in wanted:
                write_root(path, f"# {MARKER}.\n{content}")
            elif path.exists():
                sudo("rm", "-f", path)
    if shutil.which("glib-compile-schemas"):
        sudo("glib-compile-schemas", "/usr/share/glib-2.0/schemas")


def install_bluetooth(options: Options) -> None:
    unit = "/etc/systemd/system/weaselway-bluetooth.service"
    if not options.bluetooth:
        if Path(unit).exists():
            sudo("systemctl", "disable", "weaselway-bluetooth.service")
        return

    step("Installing the Bluetooth modules")
    sudo("rm", "-rf", PREFIX / "bluetooth")
    sudo("mkdir", "-p", PREFIX / "bluetooth")
    sudo("tar", "-C", PREFIX / "bluetooth", "-xzf", BUILD / "bluetooth.tar.gz")
    install_file(REPO / "distro" / "bluetooth-modules.sh", PREFIX / "libexec" / "bluetooth-modules.sh", 0o755)
    install_file(REPO / "distro" / "load-firmware.sh", PREFIX / "libexec" / "load-firmware.sh", 0o755)
    # Before anything loads the stock bluetooth.ko, which can't be swapped out
    # once in use: before udev sees a usbip-attached adapter and before bluetoothd.
    write_root(
        unit,
        "[Unit]\n"
        "Description=Load Bluetooth modules with rfkill support\n"
        "Documentation=https://github.com/weaselway/weaselway\n"
        "DefaultDependencies=no\n"
        "Before=sysinit.target systemd-udev-trigger.service bluetooth.service\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        "RemainAfterExit=yes\n"
        f"ExecStart={PREFIX}/libexec/bluetooth-modules.sh\n\n"
        "[Install]\n"
        "WantedBy=sysinit.target\n",
    )
    # The kernel asks udev for firmware, see bluetooth-modules.sh.
    write_root(
        "/etc/udev/rules.d/70-weaselway-firmware.rules",
        f'SUBSYSTEM=="firmware", ACTION=="add", RUN+="{PREFIX}/libexec/load-firmware.sh"\n',
    )
    sudo("systemctl", "daemon-reload")
    sudo("systemctl", "enable", "weaselway-bluetooth.service", "bluetooth.service")


def download_releases(options: Options) -> None:
    # First, so that an unsupported kernel fails before anything is installed.
    step(f"Downloading dxgdrm {DXGDRM_VERSION} for {KERNEL}")
    base = f"https://github.com/weaselway/dxgdrm/releases/download/{DXGDRM_VERSION}"
    download(
        f"{base}/dxgdrm-{KERNEL}.tar.gz",
        BUILD / "dxgdrm.tar.gz",
        f"dxgdrm {DXGDRM_VERSION} has no module for the kernel {KERNEL}",
    )
    download(
        f"https://raw.githubusercontent.com/weaselway/dxgdrm/{DXGDRM_VERSION}/dxgdrm_drm.h",
        BUILD / "dxgdrm_drm.h",
        f"dxgdrm {DXGDRM_VERSION} has no dxgdrm_drm.h",
    )
    if options.bluetooth:
        download(
            BLUETOOTH_URL.format(kernel=KERNEL),
            BUILD / "bluetooth.tar.gz",
            f"the latest weaselway release has no Bluetooth modules for the kernel {KERNEL}",
        )


def check_system() -> None:
    if os.getuid() == 0:
        die("run this as your user, not as root; it uses sudo where needed")
    if not os.environ.get("WSL_DISTRO_NAME"):
        die("this is not a WSL distro")
    if not Path("/run/systemd/system").is_dir():
        die("systemd is not running; set systemd=true under [boot] in /etc/wsl.conf and run wsl --shutdown")
    if not shutil.which("sudo"):
        die("sudo is not installed")


def install(options: Options, distro: Distro) -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    download_releases(options)

    step(f"Installing packages from {distro.name}")
    distro.install_packages(options)
    for package in distro.patched_packages(options):
        step(f"Building {package} with weaselway's patches")
        distro.build_patched(package, options)

    mesa = fetch_mesa()
    build_mesa(mesa, options)
    if options.flatpak:
        build_flatpak_extension(mesa, options)
    install_weaselwayd()
    install_releases()
    install_configuration(options)
    install_idle_defaults(options)
    install_bluetooth(options)

    print(
        "\nDone. Restart WSL so that the module, the groups and the environment take\n"
        "effect, in PowerShell:\n\n"
        "    wsl --shutdown\n\n"
        "Then start the distro again from the Windows desktop (not over SSH) and run:\n\n"
        f"    ww-start-session          # {options.desktop.session} is the default session\n"
        "    ww-start-viewer"
    )


def main(argv: Sequence[str]) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)
    try:
        check_system()
        distro = detect_distro()
        install(complete_options(parser, args), distro)
    except InstallError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as error:
        print(f"error: {shlex.join(error.cmd)} failed with exit status {error.returncode}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

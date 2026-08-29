# Weaselway

Scripts to get a GPU-accelerated GNOME session running on Ubuntu under WSL2.

Four pieces have to be in place:

- a **patched mesa and mutter** in the Ubuntu distro. mutter runs headless and
  serves the session over an RDP connection on a vsock; mesa gets the `d3d12`
  Gallium driver pointed at the GPU Windows exposes.
- a **slimmed WSLg system distro** ([weaselway/wslg]). Its `WSLGd` does not run a
  compositor or an RDP client of its own -- it picks the vsock port and writes
  the connection details to `/mnt/wslg/mutter-rdp.env`, which our mutter reads.
- the patched **FreeRDP client** ([weaselway/freerdp]) on the Windows side, which
  connects to that vsock and shows the session.
- a small kernel module (`dxgdrm`) giving the d3d12 driver a real
  `/dev/dri/renderD128` to find.

## Prerequisites

- **Ubuntu 26.04 (`resolute`)** under WSL2.
- **docker**, inside that distro. Both build steps run in a container.
- `git`, `curl` and `unzip`.

### 0. Get this repo

Create the distro:

```powershell
wsl --install Ubuntu-26.04 --name "Gnome"
```

It must be either the only WSL distro you have, or the first one started
after `wsl --shutdown` -- WSL only wires up `/run/user/1000/` for the distro
that starts first, and everything here depends on that directory being
non-empty. If in doubt, run `wsl --shutdown` and then start this distro
before any other.

```powershell
wsl -d Gnome
```

Install a few dependencies. This repository uses docker to get isolated build
environments. You can remove it once setup is done:

```sh
sudo apt -y update
sudo apt -y install git unzip docker.io gnome-session ubuntu-session winpr-utils
sudo usermod -aG docker $(whoami)
```

`usermod` does not affect your current login session -- open a new shell (or
`su - $(whoami)`) for the new group membership to take effect.

```sh
cd $HOME
git clone https://github.com/weaselway/weaselway.git
cd weaselway
```

Run every step from the repo root. The paths matter: the container mounts the
current directory, so running a script from elsewhere gives it the wrong tree.

### 1. Install FreeRDP

```sh
./install-freerdp.sh
```

Downloads the client release and unpacks it into `C:\Weaselway`
(`/mnt/c/Weaselway`), creating the directory if it does not exist. The piece you
will use is `sdl-freerdp.exe`.

### 2. Install the system distro image

```sh
./install-system-image.sh
```

Downloads and unpacks `system_x64-<version>.vhd` into `C:\Weaselway`. Pointing
WSL at it is a manual step -- the script only prints the stanza below when it is
done. Add it to `.wslconfig` in your Windows home directory
(`C:\Users\<you>\.wslconfig`), under the existing `[wsl2]` section if there is
one:

```ini
[wsl2]
systemDistro=C:\\Weaselway\\system_x64-v1.0.79-1.vhd
```

Note the doubled backslashes. Then, from Windows:

```powershell
wsl --shutdown
```

Nothing picks up the new image until WSL is restarted.

### 3. Build the dxgdrm kernel module

```sh
./install-kernel-module.sh
```

WSL runs Microsoft's kernel, for which no `linux-headers` package exists, so
this clones the matching kernel source and builds enough of it to compile an
out-of-tree module against. It takes a while -- most of it is that kernel build,
which is what produces the symbol versions the module is checked against.

The module lands in `/usr/local/lib/weaselway/modules/$(uname -r)/`, and the
udev rules that open up permissions on the render node in `/etc/udev/rules.d`.
Not `/lib/modules`: WSL mounts that as an overlay of its own and throws the
contents away at every `wsl --shutdown`, so a module installed there is gone by
the time anything wants to load it.

The result is tied to the running kernel (`uname -r`). After a WSL kernel update,
run it again.

### 4. Load the module (optional)

`weaselway-prep.service`, installed in step 7, loads the module by this same
path at every boot, so there is nothing to do here in normal use. This step
is only for confirming, right after building it, that the module from step 3
actually loads before you go any further:

```sh
sudo modprobe /usr/local/lib/weaselway/modules/$(uname -r)/dxgdrm.ko
sudo udevadm trigger --subsystem-match=drm
sudo udevadm settle
```

### 5. Build the patched mesa and mutter packages

```sh
./build-packages.sh
```

Rebuilds Ubuntu's own `mesa` and `mutter` source packages with the fork's
patches on top, inside a container, and drops the `.deb`s in
`ubuntu/resolute/packages/`. See
[ubuntu/resolute/README.md](ubuntu/resolute/README.md) for what it does and how
to add a package.

Building locally takes a while. If you would rather skip it, a PPA carries the
same packages already built -- see below.

### 6. Install the updated packages

```sh
sudo apt install -y ./ubuntu/resolute/packages/*/*.deb
```

The rebuilt packages carry a `+weasel0` version suffix, so they install over the
distro ones and `apt policy mutter` will show which is active.

#### Alternative: install from the PPA

Instead of steps 5 and 6, pull the same packages from a PPA:

```sh
./install-ppa-packages.sh
```

This writes `/etc/apt/preferences.d/weaselway`, pinning that PPA above every
other source (including a later distro update) so `apt upgrade` does not
silently revert `mesa` or `mutter` back to the unpatched build:

```
Package: *
Pin: release o=LP-PPA-oliver-bestmann-weaselway
Pin-Priority: 1001
```

Then it adds the PPA and upgrades:

```sh
sudo add-apt-repository ppa:oliver-bestmann/weaselway
sudo apt update
sudo apt upgrade
```

Sanity check that the installed `mutter` actually came from the PPA rather
than the distro:

```sh
apt info mutter | grep APT-Sources
```

The output should mention `oliver-bestmann/weaselway`.

### 7. Install the session units

```sh
./install-units.sh
```

The session runs as the units GNOME already ships, rather than as a script of
our own. Three things have to be added for those units to work here:

- `weaselway-prep.service`, a system unit doing the root-side setup once per
  boot -- loading `dxgdrm`, taking `/tmp/.X11-unix` back from WSL so mutter can
  create its X socket there, and mounting the shared-memory share. It is ordered
  `After=wslg.service`, the unit WSL generates to bind `/mnt/wslg/.X11-unix`
  read-only over that path; we have to undo that after it happens rather than
  before.
- `~/.config/environment.d/10-weaselway.conf`, the environment the user manager
  hands to every service in the session: the driver variables, and
  `XDG_SESSION_TYPE=wayland`. That last one is not optional --
  `org.gnome.Shell@.service` carries
  `AssertEnvironment=XDG_SESSION_TYPE=wayland`, and with no display manager here
  to establish a graphical logind session, nothing else would ever set it. The
  assert reads the *manager's* environment, so exporting it in a shell does
  nothing.
- a drop-in on `org.gnome.Shell@.service` replacing the stock `ExecStart=` with
  the headless RDP one, and pulling the vsock port in from
  `/mnt/wslg/mutter-rdp.env`. It sits in the template's drop-in directory, not
  an instance's, so it covers every session the distro offers rather than just
  one. The stock `--mode=%i` is kept, which is what lets a single file do that.

The script refuses to run if `~/.config/systemd/user/org.gnome.Shell@.service`
exists: a file of that name shadows the distro unit outright, and the drop-in
would then be layered onto the copy instead of the real one.

### Running it (session)

Then start the session:

```sh
./start-gnome-shell.sh
```

That is a thin wrapper now -- `systemctl --user start gnome-session@ubuntu.target`
is the whole of it, plus clearing failed state from any previous attempt. The
target brings up the shell on the `wayland-0` display, and with it the settings
daemons, the portals and the rest of the session.

Any session the distro ships works; pass its name to get it:

```sh
./start-gnome-shell.sh gnome
```

`ls /usr/share/gnome-session/sessions/` lists them -- `ubuntu`, `gnome` and
`gnome-login` on 26.04, with `ubuntu` the default here.

The shell unit's instance is the `gnome-shell --mode`, which is *not* the
session name: `gnome.session` requires `org.gnome.Shell@user.service` and
`gnome-login.session` requires `@gdm`. The drop-in covers all of them because it
is installed template-wide; the script asks the target which shell unit it wants
rather than assuming, which is why it can report on the right one.

The script returns once the session is up rather than staying in the foreground,
so Ctrl-C is no longer how you end it. To stop:

```sh
systemctl --user start gnome-session-shutdown.target
```

Not `systemctl --user stop gnome-session@ubuntu.target` -- the target carries
`RefuseManualStop=on` and will tell you so. Shutting down takes the session bus
with it, which is why the start wrapper puts `dbus.service` back before trying
again.

### Connect to it

Then connect to it:

```sh
./start-viewer.sh
```

That runs `sdl-freerdp.exe` from `C:\Weaselway`, pointed at the vsock address
and shared memory from the same env file. Edit it to taste -- the resolution
and `/kbd:layout:German` in particular.

Do not install Chromium via snap: the snap bundles its own mesa, which is not
the patched one and will either fail to accelerate or break outright. Use the
PPA instead:

```sh
sudo add-apt-repository ppa:xtradeb/apps
sudo apt update
sudo apt install chromium
```

## Checking each step

If something does not come up, this is roughly where to look:

```sh
ls /mnt/c/Weaselway           # sdl-freerdp.exe and system_x64-*.vhd
ls /usr/local/lib/weaselway/modules/$(uname -r)/  # module built for this kernel
lsmod | grep dxgdrm           # module loaded
ls -l /dev/dri                # renderD128 present
apt policy mutter             # the +weasel0 version is installed
cat /mnt/wslg/mutter-rdp.env  # WSLGd published the transport
```

For the session itself:

```sh
systemctl status weaselway-prep.service              # root-side setup ran
systemctl --user show-environment | grep XDG_SESSION # must say wayland
systemctl --user cat org.gnome.Shell@ubuntu.service  # drop-in applied?
systemctl --user --failed                            # what actually broke
journalctl --user -u org.gnome.Shell@ubuntu.service -b
```

`Starting requested but asserts failed` on the shell means one of the three
`Assert` lines it now carries is false: `XDG_SESSION_TYPE` missing from the
manager environment, no `/dev/dri/renderD128`, or `/mnt/wslg-shared-memory` not
mounted. The last two are `weaselway-prep.service`'s job. Note that a failed
start also stops `dbus.service` on the way down, so clear the wreckage with
`systemctl --user reset-failed` before retrying -- which is what
`start-gnome-shell.sh` does for you.

An empty or missing `mutter-rdp.env` means the system distro is not the one from
step 2 -- check the `.wslconfig` path and that WSL was restarted. You can look
around inside it with `wsl --system`.

If the shell starts but Xwayland cannot bind its display, look at
`/tmp/.X11-unix`: it should be `drwxrwxrwt` (mode 1777), on the same filesystem
as `/tmp`. A root-owned `drwxr-xr-x` there means `weaselway-prep.service` did
not get its turn, and `sudo systemctl restart weaselway-prep.service` fixes it.

That directory is worth understanding, because the obvious diagnostics lie about
it. WSL generates `wslg.service` to bind `/mnt/wslg/.X11-unix` read-only over
`/tmp/.X11-unix`, and `X-mount.mkdir` creates the mountpoint as a root-owned
`0755` directory on the way in. systemd's `tmp.mount` then covers `/tmp` with a
fresh tmpfs, leaving WSL's bind *shadowed* -- still listed in
`/proc/self/mountinfo`, mounted over, affecting nothing. So `mountpoint` calls
the path a mountpoint while `umount` calls it "not mounted", and only `umount`
is right. Comparing `stat -c %d` against `/tmp` is the honest check, which is
what the prep script does.

There is no way to move the socket somewhere WSL does not touch:
`/tmp/.X11-unix` is a compile-time constant on both sides, baked into
`libmutter` (which creates the socket and hands Xwayland the fd via
`-listenfd`) and into `libxcb` (which every X client uses to find it). No
environment variable overrides either.

[weaselway/wslg]: https://github.com/weaselway/wslg
[weaselway/freerdp]: https://github.com/weaselway/freerdp

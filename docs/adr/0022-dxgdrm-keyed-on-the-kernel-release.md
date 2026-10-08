# 0022. dxgdrm is loaded from the store, keyed on the kernel release

Status: Accepted

## Context

dxgdrm is an out-of-tree module, and a module only loads into the exact kernel
release it was built for (see dxgdrm 0003 and 0006). WSL's `/lib/modules` is an
overlay whose writes are discarded at the next `wsl --shutdown` (see dxgdrm
0005). From `6.18.40.1` on the WSL kernel has no DRM core, and dxgdrm's
package brings it along as modules (see dxgdrm 0017).

## Decision

- The `dxgdrm` flake input builds one module per supported kernel release
  (`dxgdrm-all`), currently `6.18.33.2` (WSL 2.7) and `6.18.40.1` (WSL 3.0).
- `weaselway-prep.service` loads it once per boot with
  `modprobe -d "$DXGDRM_ROOT"`, which uses the package's own `modules.dep`.
  On a kernel the package does not cover, the file is missing, the unit logs
  that, and `ww-start-session` refuses to start.
- A drop-in for `modprobe@drm.service` loads `drm.ko` from the package before
  systemd-logind starts. logind's `DeviceAllow=char-drm` covers only device
  classes that are registered at that moment, and otherwise logind refuses the
  compositor `/dev/dri/card0` (`1a4c1a2`).
- `evdev` and `uinput` are loaded from WSL's own `/lib/modules`.

## Consequences

- After `wsl --update` installs a kernel that is not covered, no session
  starts until a release adds it. Supporting a kernel means adding its config
  and source hash to dxgdrm, updating weaselway's `flake.lock`, and a
  weaselway release.
- A custom kernel set with `kernel=` in `.wslconfig` is not supported.
- A changed module needs `wsl --shutdown`, because the old one stays loaded in
  the kernel that all distros share.

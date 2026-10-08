# Decision records (ADR)

Each file records one decision: the context, the decision and its
consequences, including known risks. Most of them were written after the
fact, from ARCHITECTURE.md, the commit messages and the code. Where the reason
was not written down, it was inferred from the code and is marked "Reason
inferred from code".

New decisions get the next number. A changed decision gets a new record, and
the old one gets the status "Superseded by NNNN". Records of the kernel module
are in dxgdrm's `docs/adr`, and are referred to as "dxgdrm NNNN".

| No. | Decision | Status |
|---|---|---|
| [0001](0001-mutter-rdp-backend.md) | mutter's RDP backend serves the desktop | Superseded by 0005 |
| [0002](0002-nixos-wsl-image.md) | Distribute as a NixOS-WSL image | Accepted |
| [0003](0003-rdp-transport-derived-in-the-distro.md) | The RDP transport is derived in the user distro | Accepted |
| [0004](0004-gfxredir-shared-memory-no-codec.md) | Frames go through the wslg shared memory, with no codec fallback | Accepted |
| [0005](0005-virtual-kms-display-and-presenter.md) | The compositor runs unmodified on a virtual KMS display | Accepted |
| [0006](0006-weaselwayd-c-on-glib.md) | weaselwayd is written in C, on one GLib main loop | Accepted |
| [0007](0007-damage-readback-paced-by-the-client.md) | Read back only the damage, asynchronously, and pace the compositor to it | Accepted |
| [0008](0008-input-through-uinput.md) | The client's input becomes uinput devices | Accepted |
| [0009](0009-clipboard-backends.md) | The clipboard has a backend for mutter and one for ext-data-control-v1 | Accepted |
| [0010](0010-audio-through-protocol-simple.md) | Audio goes through PipeWire's protocol-simple servers | Accepted |
| [0011](0011-session-as-transient-unit-on-tty7.md) | The session runs in a transient system unit on tty7, without a display manager | Accepted |
| [0012](0012-session-keeps-the-distro-running.md) | The session keeps the distro running | Accepted |
| [0013](0013-compositor-patches-as-nixpkgs-patches.md) | Compositor patches only for problems of their own, applied to nixpkgs | Accepted |
| [0014](0014-graphics-stack.md) | The patched Mesa is the graphics driver package, found through `LD_LIBRARY_PATH` | Accepted |
| [0015](0015-forks-fetched-with-git.md) | Forks with eol rules are fetched with git, not as GitHub tarballs | Accepted |
| [0016](0016-image-ships-its-own-flake.md) | The image ships its configuration as a flake locked to its own commit | Accepted |
| [0017](0017-binary-cache-on-cachix.md) | CI and installed systems share a binary cache on Cachix | Accepted |
| [0018](0018-viewer-runs-from-the-image.md) | The Windows viewer runs from inside the image | Accepted |
| [0019](0019-two-images-stock-system-distro.md) | Two images, GNOME and Plasma, on WSL's stock system distro | Accepted |
| [0020](0020-wsl-workarounds-in-the-module.md) | Each WSL workaround lives in the module, next to the failure it fixes | Accepted |
| [0021](0021-image-defaults-user-password-sshd.md) | The image user is `nixos` with the password `nixos`, and sshd is off | Accepted |
| [0022](0022-dxgdrm-keyed-on-the-kernel-release.md) | dxgdrm is loaded from the store, keyed on the kernel release | Accepted |
| [0023](0023-session-environment.md) | The user manager's environment selects d3d12 and runs WebKit unsandboxed | Accepted |
| [0024](0024-unauthenticated-rdp-on-vsock.md) | RDP on a Hyper-V socket, with a throwaway certificate and no authentication | Accepted |
| [0025](0025-custom-session-for-other-compositors.md) | Other compositors start through a session script the user provides | Accepted |
| [0026](0026-placeholder-without-a-compositor.md) | A placeholder screen while no compositor owns the display | Accepted |
| [0027](0027-immutable-releases.md) | Releases are immutable and carry both images | Accepted |
| [0028](0028-ww-prefix.md) | Commands carry a `ww-` prefix | Accepted |

## Known risks

Found while writing these records, details in the linked files:

- Nothing authenticates the RDP client. Any process on the Windows host that
  can open a Hyper-V socket to the VM can see the screen and send input
  ([0024](0024-unauthenticated-rdp-on-vsock.md)).
- Every installation starts with the password `nixos` and passwordless `sudo`
  ([0021](0021-image-defaults-user-password-sshd.md)).
- WebKit's web content runs without its sandbox
  ([0023](0023-session-environment.md)).
- The audio ports on 127.0.0.1 are open to every local process, for recording
  and for feeding the microphone ([0010](0010-audio-through-protocol-simple.md)).
- dxgdrm's presenter ioctls are allowed on the render node, and both nodes are
  mode 0666, so anything that can render can read the screen (dxgdrm 0014,
  0016).
- After a WSL kernel update, no session starts until a release adds the kernel
  ([0022](0022-dxgdrm-keyed-on-the-kernel-release.md)).
- The images are close to GitHub's 2 GiB asset limit
  ([0019](0019-two-images-stock-system-distro.md)).
- The transport depends on undocumented WSL internals
  ([0003](0003-rdp-transport-derived-in-the-distro.md),
  [0012](0012-session-keeps-the-distro-running.md)).
- With ext-data-control-v1, clipboard ownership relies on an order of events
  the protocol does not require ([0009](0009-clipboard-backends.md)).

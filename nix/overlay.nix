# The patched mesa and mutter, the Windows viewer, and the scripts from this
# repo, as nixpkgs packages. Takes the fork sources and the freerdp flake as
# arguments, see ../flake.nix.
{
  mesa-src,
  mutter-src,
  freerdp,
  dxgdrm,
}:

final: prev:
let
  inherit (prev) lib;

  # Everything but the flags that are only about what gets built -- those are
  # replaced below.
  mesaDrivers = {
    # d3d12 is the one that matters, llvmpipe the fallback when no GPU is
    # exposed. softpipe matches the Ubuntu build, zink comes nearly free.
    galliumDrivers = [
      "d3d12"
      "llvmpipe"
      "softpipe"
      "zink"
    ];
    vulkanDrivers = [
      "microsoft-experimental"
      "swrast"
    ];
  };
in
{
  # Not a replacement for pkgs.mesa: that would rebuild everything linking
  # libgbm or libglvnd. The NixOS module hands this to hardware.graphics
  # instead, which is where the drivers are loaded from.
  weaselway-mesa = (prev.mesa.override mesaDrivers).overrideAttrs (old: {
    version = "${lib.fileContents "${mesa-src}/VERSION"}-weaselway";
    src = mesa-src;

    # nixpkgs' check that the GL headers match mesa-gl-headers is about its
    # own release; drivers built from the fork do not install those headers
    # anyway, postFixup removes them.
    postPatch = ''
      patchShebangs .
    '';

    mesonFlags = old.mesonFlags ++ [
      # Rusticl pulls in crates pinned to nixpkgs' mesa release (wraps.json),
      # teflon and intel-rt are for drivers we do not build.
      (lib.mesonBool "gallium-rusticl" false)
      (lib.mesonBool "teflon" false)
      (lib.mesonEnable "intel-rt" false)
      (lib.mesonOption "tools" "")
    ];

    # Without rusticl there is no libRusticlOpenCL.so to add an rpath to.
    postFixup = builtins.replaceStrings [ " $opencl/lib/libRusticlOpenCL.so" ] [ "" ] old.postFixup;
  });

  # FreeRDP for mutter's RDP server. nixpkgs builds it with FFmpeg, whose DSP
  # backend maps 16-bit PCM to FFmpeg's *unsigned* PCM codec, so every sample
  # mutter sends to the client (and every microphone sample it receives) comes
  # out shifted by 32768 -- unintelligible. Only mutter links this one, so
  # nothing else rebuilds.
  weaselway-freerdp = prev.freerdp.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [ ./freerdp-dsp-ffmpeg-pcm-s16.patch ];
  });

  # Replaced outright, so gnome-shell links the RDP-enabled build.
  mutter = prev.mutter.overrideAttrs (old: {
    version = "${old.version}-weaselway";
    src = mutter-src;

    mesonFlags = old.mesonFlags ++ [
      (lib.mesonEnable "rdp" true)
    ];

    # The release tarball nixpkgs builds vendors gvdb; a git checkout only
    # has the wrap for it. Same revision as subprojects/gvdb.wrap.
    postPatch = ''
      cp -r --no-preserve=mode ${
        final.fetchFromGitLab {
          domain = "gitlab.gnome.org";
          owner = "GNOME";
          repo = "gvdb";
          rev = "b54bc5da25127ef416858a3ad92e57159ff565b3";
          hash = "sha256-c56yOepnKPEYFcU1B1TrDl8ydU0JU+z6R8siAQP4d2A=";
        }
      } subprojects/gvdb
    ''
    + old.postPatch;

    # freerdp3, freerdp-server3, winpr3, and libcrypto for the session's TLS
    # certificate. The gfxredir channel is built from the tree, see
    # src/backends/rdp/gfxredir.
    buildInputs = old.buildInputs ++ [
      final.weaselway-freerdp
      final.openssl
    ];
  });

  # kms-wsl spike: KWin that tells the kernel what changed in a frame
  # (FB_DAMAGE_CLIPS), so the presenter does not read back the whole screen
  # for every frame. The patches are backports (branches weaselway-6.6.6 and
  # weaselway-6.7.5) of the commit on master of github.com/weaselway/kde-kwin;
  # which one depends on the Plasma release in nixpkgs. Replaced in the scope,
  # so Plasma runs it; what links KWin is rebuilt.
  kdePackages = prev.kdePackages.overrideScope (
    kfinal: kprev: {
      kwin = kprev.kwin.overrideAttrs (old: {
        # builtins.path, so that the patch is a store path of its own: as
        # ./file it would be a path into this flake's source, and every
        # change to the repo would rebuild KWin and Plasma.
        patches = (old.patches or [ ]) ++ [
          (builtins.path {
            name = "kwin-fb-damage-clips.patch";
            path =
              if lib.versionOlder old.version "6.7" then
                ./kwin-6.6-fb-damage-clips.patch
              else
                ./kwin-6.7-fb-damage-clips.patch;
          })
        ];
      });
    }
  );

  # kms-wsl spike: the userspace half of dxgdrm's virtual display. Built
  # against nixpkgs' libglvnd and libgbm; at run time those load the patched
  # mesa from /run/opengl-driver like everything else.
  weaselway-presenter = final.stdenv.mkDerivation {
    pname = "weaselway-presenter";
    version = "0-spike";
    src = ../presenter;

    nativeBuildInputs = [ final.pkg-config ];
    buildInputs = [
      final.libglvnd
      final.libgbm
      final.libdrm
      final.libjpeg
      # The RDP server; gfxredir, which nixpkgs' FreeRDP is built without, is
      # compiled in from presenter/gfxredir.
      final.freerdp
      final.openssl
    ];

    # dxgdrm_drm.h, the uapi header, lives with the module.
    makeFlags = [
      "DXGDRM_INCLUDE=${dxgdrm}"
      "PREFIX=${placeholder "out"}"
    ];
  };

  # kms-wsl spike: gnome-shell as nixpkgs builds it, against unpatched mutter.
  # The KMS session runs this one, so what it proves holds for stock GNOME.
  # Same derivation as nixpkgs', so it comes from cache.nixos.org.
  weaselway-stock-gnome-shell = prev.gnome-shell.override { mutter = prev.mutter; };

  # sdl-freerdp.exe with its SDL DLLs, cross-compiled by the freerdp flake.
  # Windows binaries, so whichever machine builds them is fine.
  weaselway-viewer = freerdp.packages.${final.stdenv.buildPlatform.system}.sdl-freerdp;

  # The session scripts, runnable from PATH. They are the same files the
  # Ubuntu setup runs from a checkout.
  weaselway-scripts =
    let
      script =
        name: runtimeInputs:
        final.writeShellApplication {
          inherit name runtimeInputs;
          text = builtins.readFile (../. + "/${name}.sh");
          # Lint in this repo's own flake check, not here.
          checkPhase = "";
          bashOptions = [ ];
        };
    in
    final.symlinkJoin {
      name = "weaselway-scripts";
      paths = [
        (script "start-gnome-shell" [
          final.coreutils
          final.gnugrep
          final.gnused
          final.systemd
        ])
        (final.writeShellApplication {
          name = "start-viewer";
          runtimeInputs = [
            final.gnused
            final.systemd
          ];
          # Run the viewer from the store rather than C:\Weaselway, so it is
          # updated with the flake. WSL interop starts it over
          # \\wsl.localhost, and Windows loads the DLLs next to it from there.
          text = ''
            : "''${WEASELWAY_VIEWER:=${final.weaselway-viewer}/bin/sdl-freerdp.exe}"
            export WEASELWAY_VIEWER
          ''
          + builtins.readFile ../start-viewer.sh;
          checkPhase = "";
          bashOptions = [ ];
        })
        (final.writeShellApplication {
          name = "start-kms-session";
          runtimeInputs = [
            final.coreutils
            final.gnugrep
            final.systemd
          ];
          text = ''
            : "''${WEASELWAY_KMS_GNOME_SHELL:=${final.weaselway-stock-gnome-shell}/bin/gnome-shell}"
            export WEASELWAY_KMS_GNOME_SHELL
          ''
          + builtins.readFile ../start-kms-session.sh;
          checkPhase = "";
          bashOptions = [ ];
        })
        (script "install-system-image" [
          final.coreutils
          final.curl
          final.gzip
        ])
      ];
    };
}

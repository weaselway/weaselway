# The patched mesa, weaselwayd, the compositors' patches, the Windows viewer,
# and the scripts from this repo, as nixpkgs packages. Takes the mesa fork's
# source and the freerdp and dxgdrm flakes as arguments, see ../flake.nix.
{
  mesa-src,
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
    # exposed. softpipe and zink come nearly free.
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

  # FreeRDP for weaselwayd's RDP server. nixpkgs builds it with FFmpeg, whose
  # DSP backend maps 16-bit PCM to FFmpeg's *unsigned* PCM codec, so every
  # sample sent to the client (and every microphone sample received) comes
  # out shifted by 32768 -- unintelligible. Only weaselwayd links this one, so
  # nothing else rebuilds.
  weaselway-freerdp = prev.freerdp.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [ ./freerdp-dsp-ffmpeg-pcm-s16.patch ];
  });

  # nixpkgs' mutter with one fix, which is on main of github.com/weaselway/mutter:
  # the overview keeps its old size when the stage is resized, which the
  # viewer's window does to it all the time. Replaced outright, so gnome-shell
  # and whatever else links mutter are rebuilt against it.
  mutter = prev.mutter.overrideAttrs (old: {
    # builtins.path, so that the patch is a store path of its own: as ./file
    # it would be a path into this flake's source, and every change to the
    # repo would rebuild mutter and gnome-shell.
    patches = (old.patches or [ ]) ++ [
      (builtins.path {
        name = "mutter-stage-relayout.patch";
        path = ./mutter-stage-relayout.patch;
      })
    ];
  });

  # KWin that tells the kernel what changed in a frame (FB_DAMAGE_CLIPS), so
  # weaselwayd does not read back the whole screen for every frame. The
  # patches are backports (branches weaselway-6.6.6 and weaselway-6.7.5) of
  # the commit on master of github.com/weaselway/kde-kwin; which one depends
  # on the Plasma release in nixpkgs. Replaced in the scope, so Plasma runs
  # it; what links KWin is rebuilt. Only built with weaselway.plasma.enable.
  kdePackages = prev.kdePackages.overrideScope (
    kfinal: kprev: {
      kwin = kprev.kwin.overrideAttrs (old: {
        # builtins.path for the same reason as mutter's patch above.
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

  # The userspace half of dxgdrm's virtual display. Built
  # against nixpkgs' libglvnd and libgbm; at run time those load the patched
  # mesa from /run/opengl-driver like everything else.
  weaselwayd = final.stdenv.mkDerivation {
    pname = "weaselwayd";
    version = "0";
    src = ../weaselwayd;

    nativeBuildInputs = [ final.pkg-config ];
    buildInputs = [
      final.libglvnd
      final.libgbm
      final.libdrm
      # The RDP server, with the PCM fix: nixpkgs' distorts the sound.
      # gfxredir, which it is built without, is compiled in from
      # weaselwayd/gfxredir.
      final.weaselway-freerdp
      final.openssl
      # The main loop, D-Bus for the clipboard, and its images.
      final.glib
      final.libpng
    ];

    # The clipboard's conversions.
    doCheck = true;
    checkTarget = "check";

    # dxgdrm_drm.h, the uapi header, lives with the module.
    makeFlags = [
      "DXGDRM_INCLUDE=${dxgdrm}"
      "PREFIX=${placeholder "out"}"
    ];
  };

  # sdl-freerdp.exe with its SDL DLLs, cross-compiled by the freerdp flake.
  # Windows binaries, so whichever machine builds them is fine.
  weaselway-viewer = freerdp.packages.${final.stdenv.buildPlatform.system}.sdl-freerdp;

  # The session scripts, runnable from PATH.
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
        (script "start-session" [
          final.coreutils
          final.gnugrep
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
        (script "install-viewer-link" [
          final.bash
          final.coreutils
        ])
        (script "install-system-image" [
          final.coreutils
          final.curl
          final.gzip
        ])
      ];
    };
}

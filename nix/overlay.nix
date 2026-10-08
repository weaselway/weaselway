# weaselway's packages and patches to nixpkgs.
{
  mesa-src,
  freerdp,
  dxgdrm,
}:

final: prev:
let
  inherit (prev) lib;

  mesaDrivers = {
    # d3d12 does the work, llvmpipe is the fallback without a GPU.
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
  # Not a replacement for pkgs.mesa, which would rebuild everything linking
  # libgbm. The module uses it as hardware.graphics.package.
  weaselway-mesa = (prev.mesa.override mesaDrivers).overrideAttrs (old: {
    version = "${lib.fileContents "${mesa-src}/VERSION"}-weaselway";
    src = mesa-src;

    # Drops nixpkgs' check that the GL headers match its own mesa release.
    postPatch = ''
      patchShebangs .
    '';

    mesonFlags = old.mesonFlags ++ [
      # Rusticl's crates are pinned to nixpkgs' mesa release; the rest is for
      # drivers we do not build.
      (lib.mesonBool "gallium-rusticl" false)
      (lib.mesonBool "teflon" false)
      (lib.mesonEnable "intel-rt" false)
      (lib.mesonOption "tools" "")
    ];

    # Without rusticl there is no libRusticlOpenCL.so to add an rpath to.
    postFixup = builtins.replaceStrings [ " $opencl/lib/libRusticlOpenCL.so" ] [ "" ] old.postFixup;
  });

  # FreeRDP's FFmpeg backend treats 16-bit PCM as unsigned, which garbles
  # audio both ways. Only weaselwayd links this one.
  weaselway-freerdp = prev.freerdp.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [ ./freerdp-dsp-ffmpeg-pcm-s16.patch ];
  });

  # The overview keeps its old size when the stage is resized, which the
  # viewer does all the time. From main of github.com/weaselway/mutter.
  mutter = prev.mutter.overrideAttrs (old: {
    # builtins.path, or every change to this repo would rebuild mutter.
    patches = (old.patches or [ ]) ++ [
      (builtins.path {
        name = "mutter-stage-relayout.patch";
        path = ./mutter-stage-relayout.patch;
      })
    ];
  });

  # KWin reports its damage (FB_DAMAGE_CLIPS), so weaselwayd does not read
  # back the whole screen. Backports of master of github.com/weaselway/kde-kwin,
  # picked by KWin's version.
  kdePackages = prev.kdePackages.overrideScope (
    kfinal: kprev: {
      kwin = kprev.kwin.overrideAttrs (old: {
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

  # The userspace half of dxgdrm's virtual display.
  weaselwayd = final.stdenv.mkDerivation {
    pname = "weaselwayd";
    version = "0";
    src = ../weaselwayd;

    nativeBuildInputs = [
      final.pkg-config
      final.wayland-scanner
    ];
    buildInputs = [
      final.libglvnd
      final.libgbm
      final.libdrm
      final.weaselway-freerdp
      final.openssl
      final.glib
      final.libpng
      final.wayland
      final.wayland-protocols
    ];

    doCheck = true;
    checkTarget = "check";

    # dxgdrm_drm.h lives with the module.
    makeFlags = [
      "DXGDRM_INCLUDE=${dxgdrm}"
      "PREFIX=${placeholder "out"}"
    ];
  };

  # sdl-freerdp.exe and its DLLs, cross-compiled by the freerdp flake.
  weaselway-viewer = freerdp.packages.${final.stdenv.buildPlatform.system}.sdl-freerdp;

  weaselway-scripts =
    let
      script =
        name: runtimeInputs:
        final.writeShellApplication {
          inherit name runtimeInputs;
          text = builtins.readFile (../. + "/${name}.sh");
          # Linted by this repo's flake check.
          checkPhase = "";
          bashOptions = [ ];
        };
    in
    final.symlinkJoin {
      name = "weaselway-scripts";
      paths = [
        (script "ww-start-session" [
          final.coreutils
          final.gnugrep
          final.kbd
          final.systemd
        ])
        (final.writeShellApplication {
          name = "ww-start-viewer";
          runtimeInputs = [
            final.gnused
            final.systemd
            final.util-linux
          ];
          # The viewer runs from the store over \\wsl.localhost, so it updates
          # with the flake.
          text = ''
            : "''${WEASELWAY_VIEWER:=${final.weaselway-viewer}/bin/sdl-freerdp.exe}"
            export WEASELWAY_VIEWER
          ''
          + builtins.readFile ../ww-start-viewer.sh;
          checkPhase = "";
          bashOptions = [ ];
        })
        (script "ww-install-viewer-link" [
          final.bash
          final.coreutils
        ])
      ];
    };
}

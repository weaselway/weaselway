# weaselway's packages and patches to nixpkgs.
{
  freerdp,
  dxgdrm,
}:

final: prev:
let
  inherit (prev) lib;

  # The mesa fork. nixpkgs' Mesa package only builds the Mesa major it was
  # written for, so each nixpkgs Mesa gets the fork branch that matches it.
  # To update one: set rev to the branch head and hash to "", build, and take
  # the hash from the error. version is the branch's VERSION file; reading it
  # from src would be import-from-derivation.
  mesaFork =
    if lib.versions.majorMinor prev.mesa.version == "26.3" then
      # branch mesa-26.3.0-wsl
      {
        version = "26.3.0";
        rev = "2517e97bef0d73155e4caaf3153a50a1b21df2ca";
        hash = "sha256-6mbnhrJ59a4zgec0xP4VeupzVFBCvGeQ0waCQMaxfRc=";
      }
    else if lib.versionOlder prev.mesa.version "27" then
      # branch mesa-26.2.7-wsl
      {
        version = "26.2.7";
        rev = "45d061df2d597e0c037616013cc29bacece29395";
        hash = "sha256-HU4r4Ajj83UaLPjZaJQImJfe1W15mDGqpRPxcq2IRnA=";
      }
    else
      # branch main
      {
        version = "27.0.0-devel";
        rev = "326919afc3e728ecd84505765ab4d32fd18bc645";
        hash = "sha256-G4GLA5pT7SKVz9+uRZQuubnC1ZKymNgj9kkztcqaipY=";
      };

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
    version = "${mesaFork.version}-weaselway";
    src = final.fetchFromGitHub {
      owner = "weaselway";
      repo = "mesa";
      inherit (mesaFork) rev hash;
    };

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
      # The Intel driver's shader compiler brings its own libLLVM-14. glibc
      # binds GNU unique symbols process-wide, ignoring versions, so a shared
      # libLLVM loaded first hands it foreign FoldingSet tables and it crashes
      # (Firefox on startup). Linked statically, LLVM's symbols stay local.
      (lib.mesonEnable "shared-llvm" false)
    ];
    # Static LLVM links its system libraries itself.
    buildInputs = old.buildInputs ++ [ final.libxml2 ];

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
      # A failed device open closed fd 0, which can be the KMS fd: GNOME hung
      # when WSL's vgem came back through udev. Also from that main.
      (builtins.path {
        name = "mutter-kms-fd0.patch";
        path = ./mutter-kms-fd0.patch;
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
              else if lib.versionOlder old.version "6.8" then
                ./kwin-6.7-fb-damage-clips.patch
              else
                ./kwin-6.8-fb-damage-clips.patch;
          })
        ];
      });
    }
  );

  # Hyprland's backend ignores new modes of a connector that stays connected,
  # so Hyprland does not follow the viewer's window size. Backport of main of
  # github.com/weaselway/aquamarine, picked by aquamarine's version.
  aquamarine = prev.aquamarine.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [
      (builtins.path {
        name = "aquamarine-mode-changes.patch";
        path =
          if lib.versionOlder old.version "0.11" then
            ./aquamarine-0.10-mode-changes.patch
          else
            ./aquamarine-0.11-mode-changes.patch;
      })
    ];
  });

  # Hyprland tests a larger mode with the last frame's smaller buffer, which
  # dxgdrm rejects, so it could not grow. Backport of main of
  # github.com/weaselway/Hyprland, picked by Hyprland's version.
  hyprland = prev.hyprland.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [
      (builtins.path {
        name = "hyprland-modeset-buffer-size.patch";
        path =
          if lib.versionOlder old.version "0.56" then
            ./hyprland-0.55-modeset-buffer-size.patch
          else if lib.versionOlder old.version "0.57" then
            ./hyprland-0.56-modeset-buffer-size.patch
          else
            ./hyprland-0.57-modeset-buffer-size.patch;
      })
    ];
  });

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
          text = builtins.readFile (../bin + "/${name}.sh");
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
          + builtins.readFile ../bin/ww-start-viewer.sh;
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

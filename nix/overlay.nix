# The patched mesa and mutter, and the scripts from this repo, as nixpkgs
# packages. Takes the fork sources as arguments, see ../flake.nix.
{ mesa-src, mutter-src }:

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

    # freerdp3, freerdp-server3, winpr3. The gfxredir channel is built from
    # the tree, see src/backends/rdp/gfxredir.
    buildInputs = old.buildInputs ++ [ final.freerdp ];
  });

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
        (script "start-viewer" [ ])
        (script "install-freerdp" [
          final.coreutils
          final.curl
          final.unzip
        ])
        (script "install-system-image" [
          final.coreutils
          final.curl
          final.gzip
        ])
      ];
    };
}

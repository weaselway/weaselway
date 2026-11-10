{
  description = "weaselway: a GPU-accelerated Wayland desktop on WSL2, as a NixOS-WSL image";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

    # For images that track nixos-unstable, see nixosConfigurations below.
    # The viewer and NixOS-WSL stay on the stable nixpkgs either way.
    nixpkgs-unstable.url = "github:NixOS/nixpkgs/nixos-unstable";

    nixos-wsl = {
      url = "github:nix-community/NixOS-WSL";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    # Not following nixpkgs: its kernel trees (from wsl-kernel) pin their
    # own, so nixpkgs updates don't rebuild them.
    dxgdrm.url = "github:weaselway/dxgdrm";

    # The Windows viewer. git, not a tarball, for the reason given at mesa-src.
    freerdp = {
      url = "git+https://github.com/weaselway/freerdp?ref=main&shallow=1";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    # The mesa fork. Fetched with git: its .gitattributes has eol=crlf rules,
    # which Nix versions apply differently to GitHub tarballs, breaking the hash.
    mesa-src = {
      url = "git+https://github.com/weaselway/mesa?ref=mesa-26.2.6-wsl&shallow=1";
      flake = false;
    };
    # Used instead once nixpkgs' Mesa is 27, whose package fails on 26.x.
    mesa-main-src = {
      url = "git+https://github.com/weaselway/mesa?ref=main&shallow=1";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      nixpkgs-unstable,
      nixos-wsl,
      dxgdrm,
      freerdp,
      mesa-src,
      mesa-main-src,
    }:
    let
      inherit (nixpkgs) lib;

      forAllSystems =
        f: lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (system: f nixpkgs.legacyPackages.${system});

      # The packages are the same in every image.
      wsl = self.nixosConfigurations.wsl-gnome;

      # The same modules the image's own flake.nix imports, so nixos-rebuild
      # inside the distro rebuilds this system.
      image =
        pkgsFlake: configPath:
        pkgsFlake.lib.nixosSystem {
          modules = [
            nixos-wsl.nixosModules.default
            self.nixosModules.weaselway
            self.nixosModules.image
            (configPath + "/configuration.nix")
            { wsl.tarball.configPath = configPath; }
          ];
        };
    in
    {
      overlays.default = import ./nix/overlay.nix {
        inherit
          mesa-src
          mesa-main-src
          freerdp
          dxgdrm
          ;
      };

      nixosModules.weaselway = {
        imports = [
          (import ./nix/module.nix { inherit dxgdrm; })
          (import ./nix/bluetooth.nix { inherit (dxgdrm.inputs) wsl-kernel; })
        ];
        nixpkgs.overlays = [ self.overlays.default ];
      };
      nixosModules.default = self.nixosModules.weaselway;

      # Pins /etc/nixos/flake.lock to this commit, see nix/image-lock.nix.
      nixosModules.image = import ./nix/image-lock.nix { inherit self; };

      nixosConfigurations.wsl-gnome = image nixpkgs ./nix/image-gnome;
      nixosConfigurations.wsl-plasma = image nixpkgs ./nix/image-plasma;
      # CI builds these for the cache only; there are no unstable images.
      nixosConfigurations.wsl-gnome-unstable = image nixpkgs-unstable ./nix/image-gnome;
      nixosConfigurations.wsl-plasma-unstable = image nixpkgs-unstable ./nix/image-plasma;

      # x86_64 builds on every system, since the distro is x86_64.
      packages = forAllSystems (pkgs: {
        weaselway-mesa = wsl.pkgs.weaselway-mesa;
        mutter = wsl.pkgs.mutter;
        weaselway-scripts = wsl.pkgs.weaselway-scripts;
        weaselway-viewer = wsl.pkgs.weaselway-viewer;
        weaselwayd = wsl.pkgs.weaselwayd;
        # The rest of what cache.nixos.org lacks, for CI to build in jobs of
        # their own before the images.
        dxgdrm-all = dxgdrm.packages.x86_64-linux.dxgdrm-all;
        gnome-shell = wsl.pkgs.gnome-shell;
        gnome-control-center = wsl.pkgs.gnome-control-center;
        kwin = wsl.pkgs.kdePackages.kwin;
        plasma-workspace = wsl.pkgs.kdePackages.plasma-workspace;
        plasma-desktop = wsl.pkgs.kdePackages.plasma-desktop;
        # sudo nix run .#tarballBuilder-gnome -> nixos.wsl
        tarballBuilder-gnome = self.nixosConfigurations.wsl-gnome.config.system.build.tarballBuilder;
        tarballBuilder-plasma = self.nixosConfigurations.wsl-plasma.config.system.build.tarballBuilder;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ (pkgs.extend self.overlays.default).weaselwayd ];
          packages = with pkgs; [
            bash
            shellcheck
            git
          ];

          # dxgdrm_drm.h, for weaselwayd's Makefile.
          DXGDRM_INCLUDE = "${dxgdrm}";

          shellHook = ''
            echo "lint with: shellcheck --shell=bash \$(git ls-files '*.sh')"
            echo "build weaselwayd with: make -C weaselwayd"
          '';
        };
      });

      checks = forAllSystems (pkgs: {
        shellcheck = pkgs.runCommand "weaselway-shellcheck" { nativeBuildInputs = [ pkgs.shellcheck ]; } ''
          cd ${self}
          find . -name '*.sh' -print0 | xargs -0 shellcheck --shell=bash --severity=error
          touch $out
        '';
        usbipd-auto-attach = pkgs.runCommand "weaselway-usbipd-auto-attach" { } ''
          ${pkgs.python3}/bin/python3 -m doctest ${./libexec/usbipd-auto-attach.py}
          touch $out
        '';
      });
    };
}

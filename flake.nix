{
  description = "weaselway: a GPU-accelerated GNOME or Plasma desktop on WSL2, as a NixOS-WSL image";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

    nixos-wsl = {
      url = "github:nix-community/NixOS-WSL";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    dxgdrm = {
      url = "github:weaselway/dxgdrm";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    # The Windows viewer, sdl-freerdp.exe, cross-compiled by its own flake.
    # Fetched with git for the same reason as mesa below (eol rules in
    # .gitattributes).
    freerdp = {
      url = "git+https://github.com/weaselway/freerdp?ref=main&shallow=1";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    # The mesa fork, on the branch that matches the mesa release nixpkgs has.
    # It carries no flake.nix, so only its source is used. Fetched with git
    # rather than as a GitHub tarball: mesa's .gitattributes has eol=crlf
    # rules, and whether those are applied to the tarball differs between Nix
    # versions, which breaks the locked hash.
    mesa-src = {
      url = "git+https://github.com/weaselway/mesa?ref=mesa-26.2.1-wsl&shallow=1";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      nixos-wsl,
      dxgdrm,
      freerdp,
      mesa-src,
    }:
    let
      inherit (nixpkgs) lib;

      forAllSystems =
        f: lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (system: f nixpkgs.legacyPackages.${system});

      wsl = self.nixosConfigurations.wsl;
    in
    {
      overlays.default = import ./nix/overlay.nix {
        inherit
          mesa-src
          freerdp
          dxgdrm
          ;
      };

      nixosModules.weaselway = {
        imports = [ (import ./nix/module.nix { inherit dxgdrm; }) ];
        nixpkgs.overlays = [ self.overlays.default ];
      };
      nixosModules.default = self.nixosModules.weaselway;

      # Gives the image's /etc/nixos a flake.lock that pins weaselway to the
      # commit the system was built from, so the first nixos-rebuild stays on
      # it rather than jumping to whatever main is by then. Imported by both
      # nixosConfigurations.wsl and nix/image/flake.nix, so the two still build
      # the same system.
      nixosModules.image = import ./nix/image-lock.nix { inherit self; };

      # A NixOS-WSL distro running the session. See NIXOS.md. Built from
      # the same configuration.nix the image ships in /etc/nixos, together with
      # nix/image/flake.nix, so a nixos-rebuild inside the distro rebuilds this
      # system rather than NixOS-WSL's generic default.
      nixosConfigurations.wsl = lib.nixosSystem {
        modules = [
          nixos-wsl.nixosModules.default
          self.nixosModules.weaselway
          self.nixosModules.image
          ./nix/image/configuration.nix
          { wsl.tarball.configPath = ./nix/image; }
        ];
      };

      # The distro is x86_64 whatever the build machine is, so these are the
      # x86_64 builds on every system; elsewhere they need an x86_64 builder,
      # see NIXOS.md.
      packages = forAllSystems (pkgs: {
        weaselway-mesa = wsl.pkgs.weaselway-mesa;
        mutter = wsl.pkgs.mutter;
        weaselway-scripts = wsl.pkgs.weaselway-scripts;
        weaselway-viewer = wsl.pkgs.weaselway-viewer;
        weaselwayd = wsl.pkgs.weaselwayd;
        # sudo nix run .#tarballBuilder -> nixos.wsl
        tarballBuilder = wsl.config.system.build.tarballBuilder;
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.tarballBuilder;
      });

      # The scripts run inside the target distro; weaselwayd is what compiles
      # here, with what its package is built from.
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ wsl.pkgs.weaselwayd ];
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
      });
    };
}

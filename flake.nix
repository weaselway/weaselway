{
  description = "weaselway: GPU-accelerated GNOME on WSL2, and a NixOS-WSL image of it";

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

    # The fork branches the Ubuntu packages build too (ubuntu/resolute), picked
    # to match the mesa and mutter releases nixpkgs has. They carry no
    # flake.nix, so only their source is used.
    mesa-src = {
      url = "github:weaselway/mesa/mesa-26.2.1-wsl";
      flake = false;
    };
    mutter-src = {
      url = "github:weaselway/mutter/50.4-wslg";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      nixos-wsl,
      dxgdrm,
      mesa-src,
      mutter-src,
    }:
    let
      inherit (nixpkgs) lib;

      forAllSystems =
        f: lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (system: f nixpkgs.legacyPackages.${system});

      wsl = self.nixosConfigurations.wsl;
    in
    {
      overlays.default = import ./nix/overlay.nix { inherit mesa-src mutter-src; };

      nixosModules.weaselway = {
        imports = [ (import ./nix/module.nix { inherit dxgdrm; }) ];
        nixpkgs.overlays = [ self.overlays.default ];
      };
      nixosModules.default = self.nixosModules.weaselway;

      # A NixOS-WSL distro running the session. See WEASELWAY.md.
      nixosConfigurations.wsl = lib.nixosSystem {
        modules = [
          nixos-wsl.nixosModules.default
          self.nixosModules.weaselway
          {
            nixpkgs.hostPlatform = "x86_64-linux";

            wsl.enable = true;
            # uid 1000: WSL only wires up /run/user/1000.
            wsl.defaultUser = "nixos";

            weaselway.enable = true;

            nix.settings.experimental-features = [
              "nix-command"
              "flakes"
            ];

            system.stateVersion = "26.05";
          }
        ];
      };

      # The distro is x86_64 whatever the build machine is, so these are the
      # x86_64 builds on every system; elsewhere they need an x86_64 builder
      # or binfmt emulation (boot.binfmt.emulatedSystems).
      packages = forAllSystems (pkgs: {
        weaselway-mesa = wsl.pkgs.weaselway-mesa;
        mutter = wsl.pkgs.mutter;
        weaselway-scripts = wsl.pkgs.weaselway-scripts;
        # sudo nix run .#tarballBuilder -> nixos.wsl
        tarballBuilder = wsl.config.system.build.tarballBuilder;
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.tarballBuilder;
      });

      # Nothing else here compiles; the scripts run inside the target distro.
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            bash
            shellcheck
            git
          ];

          shellHook = ''
            echo "lint with: shellcheck --shell=bash \$(git ls-files '*.sh')"
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

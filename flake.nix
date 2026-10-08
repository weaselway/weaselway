{
  description = "weaselway: a GPU-accelerated Wayland desktop on WSL2, as a NixOS-WSL image";

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

    # The Windows viewer. git, not a tarball, for the reason given at mesa-src.
    freerdp = {
      url = "git+https://github.com/weaselway/freerdp?ref=main&shallow=1";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    # The mesa fork. Fetched with git: its .gitattributes has eol=crlf rules,
    # which Nix versions apply differently to GitHub tarballs, breaking the hash.
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

      # The packages are the same in every image.
      wsl = self.nixosConfigurations.wsl-gnome;

      # The same modules the image's own flake.nix imports, so nixos-rebuild
      # inside the distro rebuilds this system.
      image =
        configPath:
        lib.nixosSystem {
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
          freerdp
          dxgdrm
          ;
      };

      nixosModules.weaselway = {
        imports = [ (import ./nix/module.nix { inherit dxgdrm; }) ];
        nixpkgs.overlays = [ self.overlays.default ];
      };
      nixosModules.default = self.nixosModules.weaselway;

      # Pins /etc/nixos/flake.lock to this commit, see nix/image-lock.nix.
      nixosModules.image = import ./nix/image-lock.nix { inherit self; };

      nixosConfigurations.wsl-gnome = image ./nix/image-gnome;
      nixosConfigurations.wsl-plasma = image ./nix/image-plasma;

      # x86_64 builds on every system, since the distro is x86_64.
      packages = forAllSystems (pkgs: {
        weaselway-mesa = wsl.pkgs.weaselway-mesa;
        mutter = wsl.pkgs.mutter;
        weaselway-scripts = wsl.pkgs.weaselway-scripts;
        weaselway-viewer = wsl.pkgs.weaselway-viewer;
        weaselwayd = wsl.pkgs.weaselwayd;
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
      });
    };
}

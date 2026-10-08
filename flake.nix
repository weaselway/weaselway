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

    # The Windows viewer, sdl-freerdp.exe, cross-compiled by its own flake.
    # Fetched with git for the same reason as mesa below (eol rules in
    # .gitattributes).
    freerdp = {
      url = "git+https://github.com/weaselway/freerdp?ref=main&shallow=1";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    # The mesa fork, on the release branch the image is built from. It carries
    # no flake.nix, so only its source is used. Fetched with git
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

      # The packages are the same in every image.
      wsl = self.nixosConfigurations.wsl-gnome;

      # An image, built from the configuration.nix it ships in /etc/nixos and
      # the modules that the flake.nix next to it imports, so a nixos-rebuild
      # inside the distro rebuilds this system rather than NixOS-WSL's generic
      # default.
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

      # Gives the image's /etc/nixos a flake.lock that pins weaselway to the
      # commit the system was built from, so the first nixos-rebuild stays on
      # it rather than jumping to whatever main is by then. Imported by both
      # the nixosConfigurations below and the flake.nix of each image, so the
      # two still build the same system.
      nixosModules.image = import ./nix/image-lock.nix { inherit self; };

      # The NixOS-WSL distros running the session, one per desktop. See
      # ARCHITECTURE.md.
      nixosConfigurations.wsl-gnome = image ./nix/image-gnome;
      nixosConfigurations.wsl-plasma = image ./nix/image-plasma;

      # The distro is x86_64 whatever the build machine is, so these are the
      # x86_64 builds on every system; elsewhere they need an x86_64 builder,
      # see ARCHITECTURE.md.
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

      # The scripts run inside the target distro; weaselwayd is what compiles
      # here, with what its package is built from.
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

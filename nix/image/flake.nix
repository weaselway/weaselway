# /etc/nixos/flake.nix of the weaselway NixOS-WSL image. nixos-rebuild picks
# nixosConfigurations.nixos because the distro's hostname is "nixos" (NixOS-WSL
# writes networking.hostName into wsl.conf).
#
# nixpkgs and NixOS-WSL come from weaselway's own lock, so the patched mesa and
# mutter are always built against the nixpkgs they were tested with.
# `sudo nix flake update --flake /etc/nixos` moves to the newest weaselway.
{
  inputs.weaselway.url = "github:weaselway/weaselway";

  outputs =
    { weaselway, ... }:
    {
      nixosConfigurations.nixos = weaselway.inputs.nixpkgs.lib.nixosSystem {
        modules = [
          weaselway.inputs.nixos-wsl.nixosModules.default
          weaselway.nixosModules.weaselway
          ./configuration.nix
        ];
      };
    };
}

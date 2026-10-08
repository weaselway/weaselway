# The weaselway image's system flake. nixpkgs and NixOS-WSL come from
# weaselway's lock, so the patched packages match the binary cache.
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
          weaselway.nixosModules.image
          ./configuration.nix
        ];
      };
    };
}

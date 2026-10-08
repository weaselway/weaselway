# Writes /etc/nixos/flake.lock on activation, pinned to the weaselway commit
# this system is built from. Without it the first nixos-rebuild locks whatever
# main is then. nix/image-* can't carry the lock itself (it would pin its own
# commit), and wsl.tarball.configPath can't take a generated directory without
# import-from-derivation. An existing lock is left alone.
{ self }:

{ lib, pkgs, ... }:

let
  # weaselway's own lock nested under a "weaselway" node. follows paths are
  # relative to the lock's root, so they gain a "weaselway" prefix.
  own = lib.importJSON ../flake.lock;

  nest =
    node:
    node
    // lib.optionalAttrs (node ? inputs) {
      inputs = lib.mapAttrs (_: i: if builtins.isList i then [ "weaselway" ] ++ i else i) node.inputs;
    };

  github = {
    type = "github";
    owner = "weaselway";
    repo = "weaselway";
  };

  lock = {
    version = own.version;
    root = "root";
    nodes = lib.mapAttrs (_: nest) (removeAttrs own.nodes [ own.root ]) // {
      root.inputs.weaselway = "weaselway";
      weaselway = {
        inputs = own.nodes.${own.root}.inputs;
        # Matches GitHub's tarball: weaselway has no .gitattributes.
        locked = github // {
          inherit (self) rev narHash lastModified;
        };
        original = github;
      };
    };
  };

  lockFile = builtins.toFile "flake.lock" (builtins.toJSON lock + "\n");
in
{
  # A dirty tree has no commit to pin.
  config = lib.mkIf (self ? rev) {
    system.activationScripts.weaselway-flake-lock = ''
      # Only for the image's own flake.nix.
      if [ ! -e /etc/nixos/flake.lock ] \
        && ${pkgs.gnugrep}/bin/grep -q 'github:weaselway/weaselway' /etc/nixos/flake.nix 2>/dev/null; then
        ${pkgs.coreutils}/bin/install -m 0644 ${lockFile} /etc/nixos/flake.lock
      fi
    '';
  };
}

# A flake.lock for the image's /etc/nixos, written on activation when there is
# none.
#
# An image ships its nix/image-* as /etc/nixos, and that directory can't carry a
# lock of its own: it would have to pin the weaselway commit it is part of.
# Without one, the first nixos-rebuild locks whatever weaselway main is at that
# moment, and a one-line config change turns into a rebuild of mesa, mutter and
# gnome-shell. So the lock is made here instead, from the commit this system is
# being built from.
#
# It can't go into the tarball directly: wsl.tarball.configPath is read through
# lib.cleanSource, and a generated directory there would be an
# import-from-derivation. Activation writes it instead, only when the file is
# missing, so a lock the user has since updated is left alone.
{ self }:

{ lib, pkgs, ... }:

let
  # weaselway's own inputs, nested under a "weaselway" node the way nix writes
  # a lock for a flake that depends on weaselway. follows paths are relative
  # to the root of the lock they are in, so the ones copied from weaselway's
  # lock gain a "weaselway" prefix.
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
        # The same narHash GitHub's tarball of this commit has: weaselway has
        # no .gitattributes, so a git checkout and the tarball agree.
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
  # A build from a dirty tree has no commit to pin. Its image ships without a
  # lock, as before.
  config = lib.mkIf (self ? rev) {
    system.activationScripts.weaselway-flake-lock = ''
      # Only for the image's own flake: a user-written flake.nix without the
      # weaselway input would get a lock for inputs it doesn't have.
      if [ ! -e /etc/nixos/flake.lock ] \
        && ${pkgs.gnugrep}/bin/grep -q 'github:weaselway/weaselway' /etc/nixos/flake.nix 2>/dev/null; then
        ${pkgs.coreutils}/bin/install -m 0644 ${lockFile} /etc/nixos/flake.lock
      fi
    '';
  };
}

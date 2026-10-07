# The weaselway NixOS-WSL system. This file is both what the image is built
# from and what it ships in /etc/nixos, next to flake.nix: edit it there and
# `sudo nixos-rebuild switch` to change the running system.
{ pkgs, ... }:

{
  nixpkgs.hostPlatform = "x86_64-linux";

  wsl.enable = true;
  # uid 1000: WSL only wires up /run/user/1000.
  wsl.defaultUser = "nixos";

  weaselway.enable = true;
  # Pin the GPU d3d12 renders on, for machines with more than one. Matched
  # against a substring of the adapter description.
  # weaselway.adapter = "nvidia";

  # The desktop: Plasma, with the patched KWin, and what `ww-start-session`
  # starts when given no session. No display manager, since ww-start-session
  # does what one would do.
  weaselway.plasma.enable = true;
  weaselway.session = "plasma";
  services.displayManager.sddm.enable = false;

  # Left out to keep the image under the 2 GiB that a GitHub release asset may
  # have: the music player, the X11 KWin and the RDP server, none of which the
  # session uses, the wallpapers other than the default one, the PIM runtime
  # with its MariaDB, and Discover, which fwupd brings in. Delete a line and
  # rebuild to get that part back.
  environment.plasma6.excludePackages = with pkgs.kdePackages; [
    elisa
    kwin-x11
    krdp
    plasma-workspace-wallpapers
  ];
  programs.kde-pim.enable = false;
  services.fwupd.enable = false;

  # No speech-dispatcher: its voices are ~650 MB of the image. To get them
  # back, delete the line and rebuild.
  services.speechd.enable = false;

  # SSH in, for debugging. There is no password in the image: set one with
  # `passwd` first, or add a key to users.users.nixos.openssh.authorizedKeys.
  # services.openssh.enable = true;

  # The system is a flake (/etc/nixos/flake.nix); no channels. Its inputs
  # include git repositories, which nix fetches with git.
  nix.channel.enable = false;
  programs.git.enable = true;
  nix.settings.experimental-features = [
    "nix-command"
    "flakes"
  ];

  # Leave this at the release the distro was first installed with.
  system.stateVersion = "26.05";
}

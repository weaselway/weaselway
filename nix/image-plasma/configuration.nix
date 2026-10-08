# The weaselway NixOS-WSL system. Edit it and run `sudo nixos-rebuild switch`.
{ pkgs, ... }:

{
  nixpkgs.hostPlatform = "x86_64-linux";

  wsl.enable = true;
  # uid 1000: WSL only wires up /run/user/1000.
  wsl.defaultUser = "nixos";
  # For the lock screen. Change it with `passwd`.
  users.users.nixos.initialPassword = "nixos";

  weaselway.enable = true;
  # The GPU to render on, by a substring of its name.
  # weaselway.adapter = "nvidia";

  # Plasma, without a display manager: ww-start-session does its job.
  services.desktopManager.plasma6.enable = true;
  weaselway.session = "plasma";
  services.displayManager.sddm.enable = false;

  # Left out to keep the image under GitHub's 2 GiB limit. Delete a line and
  # rebuild to get it back.
  environment.plasma6.excludePackages = with pkgs.kdePackages; [
    elisa
    kwin-x11
    krdp
    plasma-workspace-wallpapers
  ];
  programs.kde-pim.enable = false;
  services.fwupd.enable = false;

  # No screen reader: orca needs speech-dispatcher, whose voices are ~650 MB of
  # the image. To get them back, delete both lines and rebuild.
  services.orca.enable = false;
  services.speechd.enable = false;

  # SSH, for debugging. Change the password first, or add a key to
  # users.users.nixos.openssh.authorizedKeys.
  # services.openssh.enable = true;

  # The system is a flake; git is needed for its git inputs.
  nix.channel.enable = false;
  programs.git.enable = true;
  nix.settings.experimental-features = [
    "nix-command"
    "flakes"
  ];

  # Leave this at the release the distro was first installed with.
  system.stateVersion = "26.05";
}

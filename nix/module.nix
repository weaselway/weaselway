# The weaselway session on NixOS-WSL: the same pieces install-units.sh and
# install-audio.sh put on Ubuntu, declared instead of installed. Needs the
# NixOS-WSL module and the overlay from ../flake.nix.
{ dxgdrm }:

{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.weaselway;

  # Every WSL kernel the dxgdrm flake knows, as
  # lib/modules/<release>/extra/dxgdrm.ko, plus the udev rules.
  dxgdrm-all = dxgdrm.packages.${pkgs.stdenv.hostPlatform.system}.dxgdrm-all;

  prep-session = pkgs.writeShellApplication {
    name = "weaselway-prep-session";
    runtimeInputs = with pkgs; [
      coreutils
      gnugrep
      gnused
      kmod
      systemd # udevadm
      util-linux # mount, umount, mountpoint
    ];
    # The script loads the module by path, keyed on the running kernel; point
    # that path into the store instead of /usr/local.
    text = ''
      DXGDRM_KO="${dxgdrm-all}/lib/modules/$(uname -r)/extra/dxgdrm.ko"
      export DXGDRM_KO
      exec ${pkgs.bash}/bin/bash ${../libexec/prep-session.sh}
    '';
  };

  # The Ubuntu drop-in verbatim, but for where gnome-shell lives.
  shellDropIn = pkgs.writeTextDir "lib/systemd/user/org.gnome.Shell@.service.d/weaselway.conf" (
    builtins.replaceStrings [ "/usr/bin/gnome-shell" ] [ "${pkgs.gnome-shell}/bin/gnome-shell" ] (
      builtins.readFile (../units + "/org.gnome.Shell@.service.d/weaselway.conf")
    )
  );

  # Libraries the Windows GPU drivers link against by their Debian/Ubuntu
  # names, which nothing on NixOS provides. Intel's WSL driver pulls in a
  # libLLVM-9.so that needs libedit.so.2; without it, loading the driver fails
  # and d3d12 cannot create a device, so mesa falls back to software. nixpkgs'
  # libedit is the same library under its upstream soname (.0).
  wslDriverCompat = pkgs.runCommand "weaselway-wsl-driver-compat" { } ''
    mkdir -p $out/lib
    ln -s ${lib.getLib pkgs.libedit}/lib/libedit.so.0 $out/lib/libedit.so.2
  '';

  audioConfig = pkgs.writeTextDir "share/pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf" (
    builtins.readFile ../pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf
  );
in
{
  options.weaselway = {
    enable = lib.mkEnableOption "the weaselway GNOME session served over RDP";

    adapter = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = "nvidia";
      description = ''
        GPU d3d12 renders on, matched against a substring of the adapter
        description. Null leaves the choice to d3d12, which takes the first
        adapter Windows lists. `start-gnome-shell --adapter` still overrides
        it for a single session.
      '';
    };

    session = lib.mkOption {
      type = lib.types.str;
      default = "gnome";
      description = "gnome-session that `start-gnome-shell` starts when given none.";
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = config.wsl.enable;
        message = "weaselway needs NixOS-WSL (wsl.enable)";
      }
    ];

    # Graphics: the Windows driver libraries (libd3d12, libdxcore), what they
    # need, and the patched mesa, all through /run/opengl-driver.
    wsl.useWindowsDriver = true;
    hardware.graphics = {
      enable = true;
      package = pkgs.weaselway-mesa;
      extraPackages = [ wslDriverCompat ];
    };

    # mesa dlopens libd3d12.so and libdxcore.so by name, and the Windows
    # drivers resolve their own dependencies by name too. On Ubuntu both are
    # found through /etc/ld.so.conf.d/ld.wsl.conf; NixOS has no such search
    # path, so name the directory outright -- for shells here, and for the
    # user manager the session runs under in environment.d below.
    environment.sessionVariables.LD_LIBRARY_PATH = [ "/run/opengl-driver/lib" ];

    # WSL configures the network itself. GNOME turns NetworkManager on, and
    # with it wpa_supplicant, which fails to start in WSL and so fails every
    # nixos-rebuild switch.
    networking.networkmanager.enable = false;
    networking.wireless.enable = false;

    # GNOME without a display manager: the session is a set of user units,
    # started by start-gnome-shell.
    services.desktopManager.gnome.enable = true;
    services.displayManager.gdm.enable = false;

    # NixOS-WSL turns udev off; the render node needs it for its permissions.
    services.udev.enable = true;
    services.udev.packages = [ dxgdrm-all ];

    # dxgdrm's udev rule makes its nodes 0666, but until udev has applied it
    # the render node is root:render 0660, and gnome-shell could not open it.
    # Group membership works whatever the mode is.
    users.users.${config.wsl.defaultUser}.extraGroups = [
      "render"
      "video"
    ];

    # NixOS-WSL bind-mounts WSLg's X0 socket into /tmp/.X11-unix. Our mutter
    # creates its own X socket there, which weaselway-prep.service makes room
    # for; this mount would sit on top of it.
    systemd.units."tmp-.X11\\x2dunix-X0.mount".enable = lib.mkForce false;

    # See units/weaselway-prep.service.
    systemd.services.weaselway-prep = {
      description = "Weaselway host preparation for the GNOME session";
      documentation = [ "https://github.com/weaselway/weaselway" ];
      after = [
        "local-fs.target"
        "wslg.service"
        "systemd-udevd.service"
      ];
      wantedBy = [ "multi-user.target" ];
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
        ExecStart = lib.getExe prep-session;
      };
    };

    # Environment for the user manager, read by systemd's environment.d
    # generator from /etc as well as from ~/.config.
    environment.etc = {
      "environment.d/05-weaselway-nixos.conf".text = ''
        LD_LIBRARY_PATH=/run/opengl-driver/lib
      '';
      "environment.d/10-weaselway.conf".source = ../environment.d/10-weaselway.conf;
    }
    // lib.optionalAttrs (cfg.adapter != null) {
      "environment.d/20-weaselway-adapter.conf".text = ''
        MESA_D3D12_DEFAULT_ADAPTER_NAME=${cfg.adapter}
      '';
    };

    # The headless RDP ExecStart for every gnome-shell mode.
    systemd.packages = [ shellDropIn ];

    # Audio: PipeWire end to end, with the two protocol-simple servers mutter
    # connects to. See install-audio.sh.
    services.pulseaudio.enable = false;
    services.pipewire = {
      enable = true;
      pulse.enable = true;
      wireplumber.enable = true;
      configPackages = [ audioConfig ];
    };

    environment.systemPackages = [ pkgs.weaselway-scripts ];
    environment.sessionVariables.WEASELWAY_DEFAULT_SESSION = cfg.session;
  };
}

# The weaselway session on NixOS-WSL. Needs the NixOS-WSL module and the
# overlay from ../flake.nix.
{ dxgdrm }:

{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.weaselway;

  # Every WSL kernel the dxgdrm flake knows, as a module root with
  # lib/modules/<release>/extra/dxgdrm.ko, plus the udev rules.
  dxgdrm-all = dxgdrm.packages.${pkgs.stdenv.hostPlatform.system}.dxgdrm-all;

  prep-session = pkgs.writeShellApplication {
    name = "weaselway-prep-session";
    runtimeInputs = with pkgs; [
      coreutils
      findutils
      gnugrep
      gnused
      kmod
      systemd # udevadm
      util-linux # mount, umount, mountpoint
    ];
    # The script loads the module from here, keyed on the running kernel.
    text = ''
      export DXGDRM_ROOT="${dxgdrm-all}"
      exec ${pkgs.bash}/bin/bash ${../libexec/prep-session.sh}
    '';
  };

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
    enable = lib.mkEnableOption "the weaselway desktop session, shown on Windows by weaselwayd";

    adapter = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = "nvidia";
      description = ''
        GPU d3d12 renders on, matched against a substring of the adapter
        description. Null leaves the choice to d3d12, which takes the first
        adapter Windows lists. `ww-start-session --adapter` still overrides
        it for a single session.
      '';
    };

    session = lib.mkOption {
      type = lib.types.enum [
        "gnome"
        "gnome-shell"
        "plasma"
        "kwin"
        "custom"
      ];
      default = "gnome";
      description = "Session that `ww-start-session` starts when given none.";
    };

    plasma.enable = lib.mkEnableOption ''
      Plasma next to GNOME, for `ww-start-session plasma`. KWin is patched (see
      the overlay), so it and what links it are built or come from
      weaselway.cachix.org'';
  };

  config = lib.mkIf cfg.enable (
    lib.mkMerge [
      {
        assertions = [
          {
            assertion = config.wsl.enable;
            message = "weaselway needs NixOS-WSL (wsl.enable)";
          }
          {
            assertion =
              cfg.plasma.enable
              || !(lib.elem cfg.session [
                "plasma"
                "kwin"
              ]);
            message = "weaselway.session = \"${cfg.session}\" needs weaselway.plasma.enable";
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
        # drivers resolve their own dependencies by name too. NixOS has no
        # search path they would be found on, so LD_LIBRARY_PATH names the
        # directory outright: for shells in the session variables at the end
        # (ww-start-session hands it on to the session), and for the user manager
        # in environment.d below.

        # WSL configures the network itself. GNOME turns NetworkManager on, and
        # with it wpa_supplicant, which fails to start in WSL and so fails every
        # nixos-rebuild switch.
        networking.networkmanager.enable = false;
        networking.wireless.enable = false;

        # NixOS-WSL turns udev off; the render node needs it for its permissions.
        services.udev.enable = true;
        services.udev.packages = [ dxgdrm-all ];

        # dxgdrm's udev rule makes its nodes 0666, but until udev has applied it
        # the render node is root:render 0660. Group membership works whatever
        # the mode is. input is for uinput, see below.
        users.users.${config.wsl.defaultUser}.extraGroups = [
          "render"
          "video"
          "input"
        ];

        # weaselwayd creates the viewer's keyboard, pointer and touchpad through
        # uinput, as the user. The compositor needs no such rule: logind hands it
        # the evdev devices and the KMS node.
        services.udev.extraRules = ''
          KERNEL=="uinput", SUBSYSTEM=="misc", GROUP="input", MODE="0660"
        '';

        # WSL generates a wslg-session user unit that symlinks pulse/native,
        # wayland-0 and wayland-0.lock in $XDG_RUNTIME_DIR into /mnt/wslg, for the
        # PulseAudio and Weston the stock system distro runs. Ours runs neither,
        # and the pulse/native link replaces pipewire-pulse's socket, so no
        # PulseAudio client -- GNOME's sound settings among them -- finds a server.
        systemd.user.units."wslg-session.service".enable = false;

        # WSL points every shell it starts at the stock system distro's PulseAudio.
        # Unset, libpulse clients find pipewire-pulse's socket on their own. The
        # session's unit starts with a clean environment and never has it.
        environment.extraInit = ''
          unset PULSE_SERVER
        '';

        # systemd mounts its own binfmt_misc at boot, after WSL registered its
        # handler for Windows executables, and the handler is gone. Without it
        # ww-start-viewer cannot run sdl-freerdp.exe. Register it again after the
        # mount.
        wsl.interop.register = true;

        # NixOS-WSL bind-mounts WSLg's X0 socket into /tmp/.X11-unix. The
        # compositor creates its own X socket there, which weaselway-prep.service
        # makes room for; this mount would sit on top of it.
        systemd.units."tmp-.X11\\x2dunix-X0.mount".enable = lib.mkForce false;

        # logind may only open the device classes that exist when it starts
        # (DeviceAllow=char-drm), and orders itself after modprobe@drm.service
        # for that reason. On kernels where DRM core is a module it is in the
        # dxgdrm package, not where that unit looks; loaded any later, logind
        # refuses the compositor /dev/dri/card0. Fails quietly on kernels that
        # have DRM built in.
        systemd.services."modprobe@drm" = {
          overrideStrategy = "asDropin";
          serviceConfig.ExecStart = [
            ""
            "-${pkgs.kmod}/bin/modprobe -abq -d ${dxgdrm-all} drm"
          ];
        };

        # Loads dxgdrm, evdev and uinput, takes /tmp/.X11-unix back, and mounts
        # the WSLg shared-memory share. The last fails the unit when the weaselway
        # system distro is not the one in use. After wslg.service, which WSL
        # generates at every boot and which mounts over /tmp/.X11-unix.
        systemd.services.weaselway-prep = {
          description = "Weaselway host preparation for the session";
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

        # The userspace half of the display: reads the compositor's frames back
        # and serves them, with input and audio, to the viewer. It waits for a
        # compositor and outlives it, so it simply runs with the user manager.
        # Its environment (GALLIUM_DRIVER, LD_LIBRARY_PATH, WEASELWAY_VSOCK_PORT)
        # is the user manager's, from environment.d above.
        systemd.user.services.weaselwayd = {
          description = "Weaselway display server for the Windows viewer";
          documentation = [ "https://github.com/weaselway/weaselway" ];
          wantedBy = [ "default.target" ];
          # PipeWire is socket activated; the audio threads retry until it is up.
          serviceConfig = {
            ExecStart = lib.getExe' pkgs.weaselwayd "weaselwayd";
            Restart = "on-failure";
            RestartSec = 2;
          };
        };

        # Audio: PipeWire end to end, with the two protocol-simple servers
        # weaselwayd connects to.
        services.pulseaudio.enable = false;
        services.pipewire = {
          enable = true;
          pulse.enable = true;
          wireplumber.enable = true;
          configPackages = [ audioConfig ];
        };

        # CI pushes everything it builds here: dxgdrm's kernel tree, mesa, mutter,
        # gnome-shell, weaselwayd and the viewer, none of which cache.nixos.org has. Without
        # it, every nixos-rebuild after an update compiles them on the WSL machine.
        # cache.nixos.org stays; NixOS adds it to whatever is listed.
        nix.settings = {
          substituters = [ "https://weaselway.cachix.org" ];
          trusted-public-keys = [ "weaselway.cachix.org-1:aN6jpdbl2M5QNsR3U8zx1G/R0jHIkYkvX15G9jxPiHU=" ];
        };

        environment.systemPackages = [
          pkgs.weaselway-scripts
          pkgs.weaselwayd
        ];
        environment.sessionVariables = {
          LD_LIBRARY_PATH = [ "/run/opengl-driver/lib" ];
          WEASELWAY_DEFAULT_SESSION = cfg.session;
        }
        // lib.optionalAttrs (cfg.adapter != null) {
          # For ww-start-session, which hands it to the session's unit.
          MESA_D3D12_DEFAULT_ADAPTER_NAME = cfg.adapter;
        };
      }

      (lib.mkIf cfg.plasma.enable {
        services.desktopManager.plasma6.enable = true;
        # Both desktops want to be the one that asks for ssh passphrases.
        programs.ssh.askPassword = lib.mkForce "${pkgs.seahorse}/libexec/seahorse/ssh-askpass";
        # And they disagree about the screen reader.
        services.orca.enable = lib.mkForce false;
        # Plasma runs KWin through a setcap wrapper, for CAP_SYS_NICE. NixOS's
        # wrappers drop LD_LIBRARY_PATH from the environment (and with file
        # capabilities the loader would too), which is how the d3d12 driver is
        # found (see environment.d/05-weaselway-nixos.conf): KWin then cannot
        # create its gbm device. Realtime scheduling is not worth that here.
        security.wrappers.kwin_wayland.enable = lib.mkForce false;
        # For the bare KWin, which looks for a cursor theme called "default" and
        # shows no pointer without one.
        xdg.icons.fallbackCursorThemes = [ "Adwaita" ];

        environment.systemPackages = [
          pkgs.kdePackages.kwin
          pkgs.kdePackages.konsole
        ];
      })
    ]
  );
}

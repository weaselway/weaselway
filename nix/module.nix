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

  # dxgdrm.ko for every WSL kernel the dxgdrm flake knows, plus its udev rules.
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
    text = ''
      export DXGDRM_ROOT="${dxgdrm-all}"
      exec ${pkgs.bash}/bin/bash ${../libexec/prep-session.sh}
    '';
  };

  # Libraries the Windows GPU drivers expect under their Debian names. Intel's
  # driver needs libedit.so.2; without it d3d12 fails and mesa falls back to
  # software. nixpkgs ships the same library as libedit.so.0.
  wslDriverCompat = pkgs.runCommand "weaselway-wsl-driver-compat" { } ''
    mkdir -p $out/lib
    ln -s ${lib.getLib pkgs.libedit}/lib/libedit.so.0 $out/lib/libedit.so.2
  '';

  audioConfig = pkgs.writeTextDir "share/pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf" (
    builtins.readFile ../pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf
  );
in
{
  imports = [
    (lib.mkRenamedOptionModule
      [ "weaselway" "plasma" "enable" ]
      [ "services" "desktopManager" "plasma6" "enable" ]
    )
  ];

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
              lib.elem cfg.session [
                "gnome"
                "gnome-shell"
              ]
              -> config.services.desktopManager.gnome.enable;
            message = "weaselway.session = \"${cfg.session}\" needs services.desktopManager.gnome.enable";
          }
          {
            assertion =
              lib.elem cfg.session [
                "plasma"
                "kwin"
              ]
              -> config.services.desktopManager.plasma6.enable;
            message = "weaselway.session = \"${cfg.session}\" needs services.desktopManager.plasma6.enable";
          }
        ];

        # The Windows driver libraries and the patched mesa, in /run/opengl-driver.
        wsl.useWindowsDriver = true;
        hardware.graphics = {
          enable = true;
          package = pkgs.weaselway-mesa;
          extraPackages = [ wslDriverCompat ];
        };

        # NixOS-WSL turns udev off; the render node needs it for permissions.
        services.udev.enable = true;
        services.udev.packages = [ dxgdrm-all ];

        # Until udev applies dxgdrm's 0666 rule the render node is root:render
        # 0660; the groups work either way. input is for uinput.
        users.users.${config.wsl.defaultUser}.extraGroups = [
          "render"
          "video"
          "input"
        ];

        # weaselwayd creates the viewer's input devices through uinput, as the
        # user. The compositor gets its devices from logind.
        services.udev.extraRules = ''
          KERNEL=="uinput", SUBSYSTEM=="misc", GROUP="input", MODE="0660"
        '';

        # WSL's wslg-session unit links pulse/native and wayland-0 in
        # $XDG_RUNTIME_DIR to WSLg's. The pulse link replaces pipewire-pulse's
        # socket, and no PulseAudio client finds PipeWire.
        systemd.user.units."wslg-session.service".enable = false;

        # WSL points every shell at WSLg's PulseAudio; unset, libpulse finds
        # PipeWire. Not in extraInit: NixOS-WSL's shell wrapper runs that before
        # the shell, and the unset would not reach it.
        environment.loginShellInit = ''
          unset PULSE_SERVER
        '';

        # systemd mounts a fresh binfmt_misc over WSL's, which drops the handler
        # for Windows executables that ww-start-viewer needs.
        wsl.interop.register = true;

        # WSL makes binfmt_misc's status file read-only. systemd-binfmt flushes
        # through it on start and stop, fails, and nixos-rebuild switch reports
        # it. Given a file to apply, it does not flush.
        systemd.services.systemd-binfmt = {
          overrideStrategy = "asDropin";
          serviceConfig = {
            ExecStart = [
              ""
              "${config.systemd.package}/lib/systemd/systemd-binfmt /etc/binfmt.d/nixos.conf"
            ];
            ExecStop = [ "" ];
          };
        };

        # NixOS-WSL mounts WSLg's X0 socket where the compositor puts its own.
        systemd.units."tmp-.X11\\x2dunix-X0.mount".enable = lib.mkForce false;

        # logind only allows device classes that exist when it starts
        # (DeviceAllow=char-drm). Where DRM core is a module, it is in the dxgdrm
        # package, so load it from there before logind. Quiet if DRM is built in.
        systemd.services."modprobe@drm" = {
          overrideStrategy = "asDropin";
          serviceConfig.ExecStart = [
            ""
            "-${pkgs.kmod}/bin/modprobe -abq -d ${dxgdrm-all} drm"
          ];
        };

        # Loads dxgdrm, evdev and uinput, takes /tmp/.X11-unix back from
        # wslg.service, and mounts WSLg's shared memory (fails without WSLg).
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

        # The user manager's environment. mesa dlopens the Windows drivers by
        # name, and NixOS has no search path for them, hence LD_LIBRARY_PATH.
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

        # Serves the compositor's frames, input and audio to the viewer. Waits
        # for a compositor and outlives it, so it runs with the user manager.
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

        # PipeWire, with the protocol-simple servers weaselwayd connects to.
        services.pipewire = {
          enable = true;
          pulse.enable = true;
          configPackages = [ audioConfig ];
        };

        # CI pushes the patched packages here, so nixos-rebuild does not compile
        # them. cache.nixos.org stays in the list.
        nix.settings = {
          substituters = [ "https://weaselway.cachix.org" ];
          trusted-public-keys = [ "weaselway.cachix.org-1:aN6jpdbl2M5QNsR3U8zx1G/R0jHIkYkvX15G9jxPiHU=" ];
        };

        environment.systemPackages = [
          pkgs.weaselway-scripts
          pkgs.weaselwayd
        ];

        # Shells get the same environment; ww-start-session hands it on.
        environment.sessionVariables = {
          LD_LIBRARY_PATH = [ "/run/opengl-driver/lib" ];
          WEASELWAY_DEFAULT_SESSION = cfg.session;
        }
        // lib.optionalAttrs (cfg.adapter != null) {
          MESA_D3D12_DEFAULT_ADAPTER_NAME = cfg.adapter;
        };
      }

      (lib.mkIf config.services.desktopManager.plasma6.enable {
        # KWin's setcap wrapper (for realtime scheduling) drops LD_LIBRARY_PATH,
        # so KWin would not find the d3d12 driver.
        security.wrappers.kwin_wayland.enable = lib.mkForce false;

        # The bare KWin shows no pointer without a "default" cursor theme.
        xdg.icons.fallbackCursorThemes = [ "Adwaita" ];

        environment.systemPackages = [
          pkgs.adwaita-icon-theme
        ];
      })
    ]
  );
}

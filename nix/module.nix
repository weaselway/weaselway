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
  # software. nixpkgs ships the same library as libedit.so.0. It also needs
  # libz.so.1, which GL processes happen to load already, but Vulkan (dzn)
  # processes do not.
  wslDriverCompat = pkgs.runCommand "weaselway-wsl-driver-compat" { } ''
    mkdir -p $out/lib
    ln -s ${lib.getLib pkgs.libedit}/lib/libedit.so.0 $out/lib/libedit.so.2
    ln -s ${lib.getLib pkgs.zlib}/lib/libz.so.1 $out/lib/libz.so.1
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

    viewer.copyTo = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = "/mnt/c/work";
      description = ''
        Windows directory, as a path in the distro, that `ww-start-viewer`
        copies the viewer to before it runs it, into a subdirectory named
        after the store hash of the viewer build. A copy that is there is
        never replaced. Null runs the viewer from the store over
        \\wsl.localhost.
      '';
    };

    idle.leaveToWindows = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Whether the desktop leaves locking and turning off the screen on idle
        to Windows, which still does both, and which the session's
        inhibitors keep awake. True sets system defaults that the user can
        change in the desktop's settings:

        - Plasma: PowerDevil neither dims nor turns off the screen, and the
          screen locker does not lock on idle.
        - GNOME: the session never goes idle (`idle-delay` 0). Without GDM,
          GNOME has no lock screen and does not blank either way.

        False keeps the desktops' own defaults: Plasma locks after 5 minutes
        and turns the virtual display off, which the viewer shows as a black
        frame.
      '';
    };

    usbip.devices = lib.mkOption {
      type = lib.types.listOf (lib.types.strMatching "[0-9a-fA-F]{4}:[0-9a-fA-F]{4}");
      default = [ ];
      example = [ "2357:0604" ];
      description = ''
        USB devices, by VID:PID, that a service keeps attached to the distro
        with usbipd-win's `usbipd attach --wsl`, also after they are plugged
        in again. Share each one once on Windows with `usbipd bind`. Needs
        usbipd-win in its default location.
      '';
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

        # WSL's generated unit bind-mounts WSLg's read-only .X11-unix again
        # whenever X0 is missing. switch-to-configuration starts it, and without
        # a running session the next compositor gets no X socket. weaselway-prep
        # removes the mount that WSL makes at boot.
        systemd.units."wslg.service".enable = false;

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

        # Loads dxgdrm, evdev, uinput and vhci_hcd, sets full preemption, takes
        # /tmp/.X11-unix back from wslg.service, and mounts WSLg's shared
        # memory (fails without WSLg).
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
          # A copy in another user's session, e.g. root's after wsl -u root,
          # can't get the vsock port and restarts forever.
          unitConfig.ConditionUser = config.wsl.defaultUser;
          # PipeWire is socket activated; the audio threads retry until it is up.
          serviceConfig = {
            ExecStart = lib.getExe' pkgs.weaselwayd "weaselwayd";
            Restart = "on-failure";
            RestartSec = 2;
          };
        };

        # Lets weaselwayd lower its nice value to -11, like PipeWire gets via rtkit.
        # A unit limit, not rtkit, because rtkit raises single threads only.
        systemd.services."user@".serviceConfig.LimitNICE = "-11";

        # Hyper-V passes the host's battery in over ACPI (BAT1, AC1). The desktops
        # read it through UPower, which they only enable with powerManagement,
        # and NixOS-WSL turns that off. Forced: they set it to that value.
        services.upower = {
          enable = lib.mkForce true;
          # Windows acts on a critical battery. UPower's default, HybridSleep or
          # else PowerOff, would stop the distro.
          criticalPowerAction = "Ignore";
          allowRiskyCriticalPowerAction = true;
        };

        # Windows suspends the machine, not the distro. Without this, logind lets
        # GNOME and Plasma suspend the WSL kernel when the session is idle.
        systemd.sleep.settings.Sleep = {
          AllowSuspend = "no";
          AllowHibernation = "no";
          AllowHybridSleep = "no";
          AllowSuspendThenHibernate = "no";
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
        }
        // lib.optionalAttrs (cfg.viewer.copyTo != null) {
          WEASELWAY_VIEWER_COPY_TO = cfg.viewer.copyTo;
        };
      }

      (lib.mkIf (cfg.usbip.devices != [ ]) {
        # Runs usbipd.exe through WSL interop; systemd services don't get the
        # Windows PATH, so the script calls it by its full path.
        systemd.services.weaselway-usbipd-auto-attach = {
          description = "Attach USB devices from Windows with usbipd";
          documentation = [ "https://github.com/weaselway/weaselway" ];
          # weaselway-prep loads vhci_hcd.
          after = [ "weaselway-prep.service" ];
          wantedBy = [ "multi-user.target" ];
          serviceConfig = {
            ExecStart = lib.escapeShellArgs [
              (lib.getExe pkgs.python3)
              "${../libexec/usbipd-auto-attach.py}"
              "--hardware-ids"
              (lib.concatStringsSep "," cfg.usbip.devices)
              "--usbipd"
              "${config.wsl.wslConf.automount.root}/c/Program Files/usbipd-win/usbipd.exe"
            ];
            Restart = "always";
            RestartSec = 5;
          };
        };
      })

      (lib.mkIf config.services.desktopManager.plasma6.enable {
        # KWin's setcap wrapper (for realtime scheduling) drops LD_LIBRARY_PATH,
        # so KWin would not find the d3d12 driver.
        security.wrappers.kwin_wayland.enable = lib.mkForce false;

        # The bare KWin shows no pointer without a "default" cursor theme.
        xdg.icons.fallbackCursorThemes = [ "Adwaita" ];

        environment.systemPackages = [
          pkgs.adwaita-icon-theme
          # Plasma adds it only with powerManagement, which NixOS-WSL turns off.
          # It keeps the apps' sleep and idle inhibitors, which weaselwayd reads.
          pkgs.kdePackages.powerdevil
        ];
      })

      (lib.mkIf (config.services.desktopManager.plasma6.enable && cfg.idle.leaveToWindows) {
        environment.etc = {
          "xdg/powerdevilrc".text =
            lib.concatMapStrings
              (profile: ''
                [${profile}][Display]
                DimDisplayWhenIdle=false
                TurnOffDisplayWhenIdle=false

              '')
              [
                "AC"
                "Battery"
                "LowBattery"
              ];
          "xdg/kscreenlockerrc".text = ''
            [Daemon]
            Autolock=false
          '';
        };
      })

      (lib.mkIf (config.services.desktopManager.gnome.enable && cfg.idle.leaveToWindows) {
        # Never idle: no blank screen, and so no lock either.
        programs.dconf.profiles.user.databases = [
          { settings."org/gnome/desktop/session".idle-delay = lib.gvariant.mkUint32 0; }
        ];
      })
    ]
  );
}

{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.services.plasma-nm-ts;
in
{
  options.services.plasma-nm-ts = {
    enable = lib.mkEnableOption "Tailscale profiles as NetworkManager VPN connections";
    package = lib.mkOption {
      type = lib.types.package;
      # Built from the system's nixpkgs, so the plasma-nm plugin matches the installed plasma-nm
      default = pkgs.kdePackages.callPackage ./package.nix { };
      defaultText = lib.literalExpression "pkgs.kdePackages.callPackage ./package.nix { }";
      description = "The plasma-nm-ts package to use.";
    };
    sync = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Run a service activating the NetworkManager connection of the profile Tailscale runs, when Tailscale
        was started outside NetworkManager (`tailscale up`, `tailscale switch`, at boot).
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = config.networking.networkmanager.enable && config.services.tailscale.enable;
        message = "services.plasma-nm-ts requires NetworkManager and Tailscale to be enabled";
      }
    ];

    networking.networkmanager.plugins = [ cfg.package ];

    # NetworkManager only discovers VPN plugins at startup
    systemd.services.NetworkManager.restartTriggers = [ cfg.package ];

    # The plasma-nm plugin is found through /run/current-system/sw/lib/qt-6/plugins
    environment.systemPackages = [ cfg.package ];

    systemd.packages = lib.mkIf cfg.sync [ cfg.package ];
    systemd.services.nm-tailscale-sync = lib.mkIf cfg.sync {
      wantedBy = [ "multi-user.target" ];
    };
  };
}

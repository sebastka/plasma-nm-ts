self:
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
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "plasma-nm-ts.packages.\${system}.default";
      description = "The plasma-nm-ts package to use.";
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
  };
}

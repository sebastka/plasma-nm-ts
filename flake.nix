{
  description = "Tailscale integration for NetworkManager and the KDE Plasma network applet";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (
          system: f nixpkgs.legacyPackages.${system}
        );
    in
    {
      packages = forAllSystems (pkgs: rec {
        plasma-nm-ts = pkgs.kdePackages.callPackage ./nix/package.nix { };
        default = plasma-nm-ts;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
        };
      });

      nixosModules.default = import ./nix/module.nix;
    };
}

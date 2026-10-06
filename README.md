# plasma-nm-ts

> [!NOTE]
> This project was created with [Anthropic Claude Opus 5.5](https://www.anthropic.com/claude).

Tailscale profiles as NetworkManager VPN connections, so they show up in the
KDE Plasma network applet (plasma-nm), one connection per Tailscale profile.

tailscaled keeps owning the tunnel, routes and DNS. The NetworkManager service
plugin only switches profiles and toggles `WantRunning` through the tailscaled
LocalAPI, then reports the `tailscale0` addresses to NetworkManager, with no
routes, no DNS and `never-default`.

Status: prototype. Only the NetworkManager service plugin exists so far. The
plasma-nm editor plugin is not written yet.

## NixOS

```nix
# flake.nix
inputs.plasma-nm-ts = {
  url = "github:sebastka/plasma-nm-ts";
  inputs.nixpkgs.follows = "nixpkgs";
};

# configuration
imports = [ inputs.plasma-nm-ts.nixosModules.default ];
services.plasma-nm-ts.enable = true;
```

## Creating connections

Until the plasma-nm editor exists, create connections with `nmcli`. `profile`
accepts a profile ID, a profile name, a login name or a tailnet name (see
`tailscale switch --list`):

```sh
nmcli connection add type vpn vpn-type tailscale con-name "Tailscale (private)" \
  vpn.data profile=karlsen.fr vpn.persistent yes
nmcli connection up "Tailscale (private)"
```

Optional `vpn.data` key: `interface` (default `tailscale0`).

## Development

```sh
nix develop
cmake -B build && cmake --build build

# Exercise the LocalAPI path without NetworkManager (needs root or --operator)
./build/service/nm-tailscale-service --test-up <profile>
./build/service/nm-tailscale-service --test-down

# Plugin logs
journalctl -u NetworkManager -f
```

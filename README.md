# plasma-nm-ts

> [!NOTE]
> This project was created with [Anthropic Claude Opus 5.5](https://www.anthropic.com/claude).

Tailscale profiles as NetworkManager VPN connections, so they show up in the
KDE Plasma network applet (plasma-nm), one connection per Tailscale profile.

tailscaled keeps owning the tunnel, routes and DNS. The NetworkManager service
plugin only switches profiles and toggles `WantRunning` through the tailscaled
LocalAPI, then reports the `tailscale0` addresses to NetworkManager, with no
routes, no DNS and `never-default`.

Two parts:

- `service/`: the NetworkManager VPN service plugin.
- `ui/`: the plasma-nm VPN plugin. The applet refuses to connect VPN types it
  has no plugin for, and the connection editor uses it to pick the account.

Status: prototype.

The plasma-nm plugin uses plasma-nm's private editor library (plasma-nm
installs no headers for it), so it must be built against the plasma-nm source
of the exact installed version. The NixOS module takes care of this by building
the package from the system's nixpkgs.

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

In the Plasma network settings: add a connection, pick *Tailscale* under VPN
and select the account. Accounts are added with `tailscale login`.

Or with `nmcli`, where `profile` accepts a profile ID, a profile name, a login
name or a tailnet name (see `tailscale switch --list`):

```sh
nmcli connection add type vpn vpn-type tailscale con-name "Tailscale (private)" \
  vpn.data profile=karlsen.fr vpn.persistent yes
nmcli connection up "Tailscale (private)"
```

Optional `vpn.data` key: `interface` (default `tailscale0`).

## Development

```sh
nix build   # or, with the distro's packages:
scripts/fetch-plasma-nm-source.sh /tmp/plasma-nm
cmake -B build -DPLASMA_NM_SOURCE_DIR=/tmp/plasma-nm && cmake --build build

# Service only, without the plasma-nm plugin
cmake -B build -DBUILD_PLASMA_NM_PLUGIN=OFF

# Exercise the LocalAPI path without NetworkManager (needs root or --operator)
./build/service/nm-tailscale-service --test-up <profile>
./build/service/nm-tailscale-service --test-down

# Plugin logs
journalctl -u NetworkManager -f
```

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

tailscaled runs one profile at a time. Connecting another Tailscale connection
switches tailscaled to its profile, and the previous connection disconnects
itself without stopping Tailscale. Changes made outside NetworkManager
(`tailscale down`, `tailscale switch`, an expired login) also disconnect the
connection in NetworkManager.

The `nm-tailscale-sync` service covers the other direction: when Tailscale
runs a profile whose connection is not active (`tailscale up`,
`tailscale switch`, at boot), it activates that connection. The NixOS module
enables it; set `services.plasma-nm-ts.sync = false` to disable it.

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
./build/service/nm-tailscale-service --test-watch <profile>   # reports when a connection would drop

# Plugin logs
journalctl -u NetworkManager -f
journalctl -u nm-tailscale-sync -f
```

## Releases

GitHub releases carry `.deb` packages for the current Debian stable and Ubuntu
LTS (Kubuntu) releases, and `.rpm` packages for the current Fedora release, for
x86_64 and aarch64. The plasma-nm plugin is built against that release's
plasma-nm, so each package only fits the release named in its file name.
Releases still on Plasma 5 are not supported.

Every published file has a signed build provenance attestation. To check that
a package was built by this repository's Release workflow:

```sh
gh attestation verify FILE -R sebastka/plasma-nm-ts
```

To release: bump the version everywhere `scripts/check-version.sh` looks (and
add a `%changelog` entry to `packaging/plasma-nm-ts.spec`) in a pull request,
then run the *Release* workflow on `master` with that version. It validates the
commit, builds every package, and only then tags `vX.Y.Z` and publishes the
release. Its `dry_run` option stops before tagging.

Packages can also be built locally in a container:

```sh
docker run --rm -v "$PWD:/src" -w /src debian:stable packaging/build-deb.sh
docker run --rm -v "$PWD:/src" -w /src ubuntu:latest packaging/build-deb.sh
docker run --rm -v "$PWD:/src" -w /src fedora:latest packaging/build-rpm.sh
```

## License

GPL-2.0-or-later, like plasma-nm. See [LICENSE](LICENSE).

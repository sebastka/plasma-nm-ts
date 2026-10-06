#!/bin/sh
# Build the .deb for the Debian release and architecture it runs on. Meant for
# a clean container:
#
#   docker run --rm -v "$PWD:/src" -w /src debian:trixie packaging/build-deb.sh
#
# Installs the build dependencies (needs root), fetches the source of the
# installed plasma-nm (checksum verified), builds with CMake, and packages the
# result with dpkg-deb. The .deb ends up in packaging/out/.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
cd "$top"

version=$(scripts/check-version.sh)
revision=${DEB_REVISION:-1}
arch=$(dpkg --print-architecture)

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config curl xz-utils ca-certificates file dpkg-dev \
    libglib2.0-dev libjson-glib-dev libsoup-3.0-dev libnm-dev \
    extra-cmake-modules qt6-base-dev libkf6coreaddons-dev libkf6i18n-dev \
    libkf6widgetsaddons-dev libkf6networkmanagerqt-dev plasma-nm >/dev/null

work=$(mktemp -d)
scripts/fetch-plasma-nm-source.sh "$work/plasma-nm"
# 4:6.3.6-1 -> 4:6.3.6: any later plasma-nm, including Debian revisions
plasma_nm_version=$(dpkg-query -W -f '${Version}' plasma-nm | sed 's/-[^-]*$//')

build="$work/build"
cmake -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DPLASMA_NM_SOURCE_DIR="$work/plasma-nm"
cmake --build "$build"
stage="$work/stage"
DESTDIR="$stage" cmake --install "$build" >/dev/null
service="$stage/usr/libexec/nm-tailscale-service"
plugin=$(find "$stage" -name plasmanetworkmanagement_tailscaleui.so)
strip --strip-unneeded "$service" "$plugin"
install -Dm644 LICENSE "$stage/usr/share/doc/plasma-nm-ts/copyright"
install -Dm644 README.md "$stage/usr/share/doc/plasma-nm-ts/README.md"

# Shared-library dependencies, the Debian way (dpkg-shlibdeps wants a
# debian/control next to it). plasma-nm ships libplasmanm_editor without
# dependency information, hence --ignore-missing-info and the explicit
# plasma-nm dependency below.
mkdir -p "$work/debian"
printf 'Source: plasma-nm-ts\n\nPackage: plasma-nm-ts\nArchitecture: any\n' > "$work/debian/control"
shlibs=$(cd "$work" && dpkg-shlibdeps --ignore-missing-info -O "$service" "$plugin" | sed -n 's/^shlibs:Depends=//p')
if [ -z "$shlibs" ]; then
    echo "$0: dpkg-shlibdeps found no dependencies" >&2
    exit 1
fi

mkdir -p "$stage/DEBIAN"
cat > "$stage/DEBIAN/control" <<EOF
Package: plasma-nm-ts
Version: $version-$revision
Architecture: $arch
Maintainer: Sebastian Karlsen <sebastian@karlsen.fr>
Installed-Size: $(du -sk "$stage" | cut -f1)
Depends: $shlibs, network-manager, plasma-nm (>= $plasma_nm_version)
Recommends: tailscale
Section: net
Priority: optional
Homepage: https://github.com/sebastka/plasma-nm-ts
Description: Tailscale profiles as NetworkManager VPN connections in KDE Plasma
 Each Tailscale profile (account) becomes a VPN connection in NetworkManager
 and the KDE Plasma network applet. tailscaled keeps managing the tunnel,
 routes and DNS; connecting switches tailscaled to the profile, and changes
 made with the tailscale command are reflected in NetworkManager.
EOF

# NetworkManager and dbus pick up the plugin by themselves; only the sync
# service needs starting.
cat > "$stage/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ] && [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
    systemctl enable nm-tailscale-sync.service || true
    systemctl restart nm-tailscale-sync.service || true
fi
EOF
cat > "$stage/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = remove ] && [ -d /run/systemd/system ]; then
    systemctl disable --now nm-tailscale-sync.service || true
fi
EOF
cat > "$stage/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
fi
EOF
chmod 755 "$stage/DEBIAN/postinst" "$stage/DEBIAN/prerm" "$stage/DEBIAN/postrm"

mkdir -p packaging/out
deb="packaging/out/plasma-nm-ts_${version}-${revision}_${arch}.deb"
dpkg-deb --root-owner-group --build "$stage" "$deb" >/dev/null
rm -rf "$work"
dpkg-deb --info "$deb" | sed -n 's/^ \(Depends\|Recommends\):/\1:/p'
dpkg-deb --contents "$deb" | awk '{print $NF}' | grep -v '/$'

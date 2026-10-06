#!/bin/sh
# Downloads the plasma-nm source matching the installed plasma-nm package, for
# -DPLASMA_NM_SOURCE_DIR. The plasma-nm plugin uses plasma-nm's private ABI,
# so the versions must match.
set -eu

dest=${1:?usage: $0 DEST}

if command -v rpm >/dev/null 2>&1 && rpm -q plasma-nm >/dev/null 2>&1; then
    version=$(rpm -q --qf '%{VERSION}' plasma-nm)
elif command -v dpkg-query >/dev/null 2>&1 && dpkg-query -W plasma-nm >/dev/null 2>&1; then
    # 4:6.3.6-1 -> 6.3.6
    version=$(dpkg-query -W -f '${Version}' plasma-nm | sed -E 's/^[0-9]+://; s/-[^-]+$//')
else
    echo "$0: plasma-nm is not installed" >&2
    exit 1
fi

tarball="plasma-nm-$version.tar.xz"
url="https://download.kde.org/stable/plasma/$version/$tarball"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The tarball is redirected to third-party mirrors; its checksum is served by
# download.kde.org itself, so it is the trust anchor. Refuse it from elsewhere.
checksum_url=$(curl -fsSL --proto '=https' -o "$tmp/$tarball.sha256" -w '%{url_effective}' "$url.sha256")
case "$checksum_url" in
    https://download.kde.org/*) ;;
    *)
        echo "$0: checksum served from $checksum_url, expected download.kde.org" >&2
        exit 1
        ;;
esac

curl -fsSL --proto '=https' -o "$tmp/$tarball" "$url"
(cd "$tmp" && sha256sum -c --quiet "$tarball.sha256")

mkdir -p "$dest"
tar -xJ -f "$tmp/$tarball" -C "$dest" --strip-components=1
echo "plasma-nm $version source in $dest (sha256 verified)"

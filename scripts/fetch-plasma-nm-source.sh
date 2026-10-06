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

mkdir -p "$dest"
curl -fsSL "https://download.kde.org/stable/plasma/$version/plasma-nm-$version.tar.xz" \
    | tar -xJ -C "$dest" --strip-components=1
echo "plasma-nm $version source in $dest"

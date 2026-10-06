#!/bin/sh
# Prints the project version after checking that every place declaring it
# agrees. With an argument, also checks that it is that version.
set -eu
cd "$(dirname "$0")/.."

cmake=$(sed -n 's/^project(plasma-nm-ts VERSION \([^ ]*\) .*/\1/p' CMakeLists.txt)
nix=$(sed -n 's/^  version = "\(.*\)";/\1/p' nix/package.nix)
spec=$(sed -n 's/^Version: *//p' packaging/plasma-nm-ts.spec)
json=$(sed -n 's/^ *"Version": "\(.*\)",/\1/p' ui/plasmanetworkmanagement_tailscaleui.json)
changelog=$(sed -n '/^%changelog/{n;s/.* - \([^-]*\)-[^-]*$/\1/p;}' packaging/plasma-nm-ts.spec)

status=0
for pair in "nix/package.nix:$nix" "packaging/plasma-nm-ts.spec:$spec" \
    "packaging/plasma-nm-ts.spec %changelog:$changelog" \
    "ui/plasmanetworkmanagement_tailscaleui.json:$json" ${1:+"requested:$1"}; do
    if [ "${pair##*:}" != "$cmake" ]; then
        echo "$0: ${pair%:*} has '${pair##*:}', CMakeLists.txt has '$cmake'" >&2
        status=1
    fi
done
[ "$status" -eq 0 ] && echo "$cmake"
exit "$status"

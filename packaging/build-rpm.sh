#!/bin/sh
# Build the RPM for the Fedora release and architecture it runs on. Meant for
# a clean container:
#
#   docker run --rm -v "$PWD:/src" -w /src fedora:44 packaging/build-rpm.sh
#
# Installs the build dependencies (needs root), fetches the source of the
# installed plasma-nm (checksum verified), and builds from the working tree.
# The RPM ends up in packaging/out/<arch>/.
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
cd "$top"

version=$(scripts/check-version.sh)

dnf -y -q install rpm-build dnf-plugins-core curl xz tar gzip
dnf -y -q builddep packaging/plasma-nm-ts.spec

work=$(mktemp -d)
scripts/fetch-plasma-nm-source.sh "$work/plasma-nm"
plasma_nm_version=$(rpm -q --qf '%{VERSION}' plasma-nm)

tar -czf "$work/plasma-nm-ts-$version.tar.gz" \
    --exclude=./.git --exclude=./result --exclude='./build*' --exclude=./packaging/out \
    --transform "s,^\.,plasma-nm-ts-$version," .

rpmbuild -bb packaging/plasma-nm-ts.spec \
    --define "_sourcedir $work" \
    --define "_rpmdir $top/packaging/out" \
    --define "plasma_nm_source $work/plasma-nm" \
    --define "plasma_nm_version $plasma_nm_version"
rm -rf "$work"
ls -l packaging/out/*/plasma-nm-ts-"$version"-*.rpm

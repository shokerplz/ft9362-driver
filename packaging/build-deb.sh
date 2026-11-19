#!/usr/bin/env bash
# Rebuild the distro's libfprint source package with the focaltech0752 driver.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "${1:-$repo/dist}" && cd "${1:-$repo/dist}" && pwd)
work=$(mktemp -d)
export DEBIAN_FRONTEND=noninteractive

sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/*.sources
apt-get update
apt-get install -y --no-install-recommends build-essential devscripts dpkg-dev fakeroot
cd "$work"
apt-get source libfprint
apt-get build-dep -y libfprint

cd libfprint-*/
"$repo/packaging/apply-driver.sh" .
DEBFULLNAME="ft9362-driver" DEBEMAIL="ft9362-driver@localhost" \
  dch --local +ft9362. "Add FocalTech FT9362 (2808:0752) driver."
DEB_BUILD_OPTIONS=nocheck dpkg-buildpackage -b -us -uc

deb=$(ls "$work"/libfprint-2-2_*.deb)
dpkg-deb -x "$deb" "$work/check"
grep -q focaltech0752 "$work"/check/usr/lib/*/libfprint-2.so.2 \
  || { echo "focaltech0752 missing from $deb" >&2; exit 1; }
. /etc/os-release
cp "$deb" "$out/${ID}-${VERSION_ID}_$(basename "$deb")"
chown --reference="$repo" "$out" "$out/${ID}-${VERSION_ID}_$(basename "$deb")"
echo "Built $out/${ID}-${VERSION_ID}_$(basename "$deb")"

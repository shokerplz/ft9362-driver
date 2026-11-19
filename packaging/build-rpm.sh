#!/usr/bin/env bash
# Rebuild the distro's libfprint source RPM with the focaltech0752 driver.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "${1:-$repo/dist}" && cd "${1:-$repo/dist}" && pwd)
top=$(mktemp -d)

dnf -y install rpm-build diffutils
cd "$top"
dnf download --source libfprint
rpm -i --define "_topdir $top" libfprint-*.src.rpm
spec=$top/SPECS/libfprint.spec

# Turn the driver into a patch that %autosetup applies.
ver=$(rpmspec -q --srpm --qf '%{version}' "$spec")
rel=$(rpmspec -q --srpm --qf '%{release}' "$spec")
mkdir tree && tar -xf "SOURCES/libfprint-v$ver.tar.gz" -C tree
cd tree
cp -a "libfprint-v$ver" a
mv "libfprint-v$ver" b
"$repo/packaging/apply-driver.sh" b
diff -urN a b > "$top/SOURCES/focaltech0752.patch" || [ $? -eq 1 ]
cd "$top"

sed -i \
  -e 's/^Source0:.*/&\nPatch9999:      focaltech0752.patch/' \
  -e "s/^Release:.*/Release:        $rel.ft9362/" \
  "$spec"

dnf -y builddep "$spec"
rpmbuild --define "_topdir $top" --nocheck -bb "$spec"

rpm=$(ls "$top"/RPMS/*/libfprint-"$ver"-*.rpm)
mkdir check && (cd check && rpm2cpio "$rpm" | cpio -idm --quiet)
grep -q focaltech0752 check/usr/lib64/libfprint-2.so.2 \
  || { echo "focaltech0752 missing from $rpm" >&2; exit 1; }
cp "$rpm" "$out/"
chown --reference="$repo" "$out" "$out/$(basename "$rpm")"
echo "Built $out/$(basename "$rpm")"

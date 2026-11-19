#!/usr/bin/env bash
# Add the focaltech0752 driver to an unpacked libfprint source tree.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
src=${1:?usage: apply-driver.sh <libfprint-source-dir>}

cp "$repo"/shared/focaltech_nn_* "$src/libfprint/drivers/"
cp "$repo/driver/focaltech-0752.c" "$src/libfprint/drivers/focaltech0752.c"

# Register the driver next to focaltech_moc. libfprint <= 1.94.10 lists
# drivers as `'focaltech_moc',`; newer releases use `'focaltech_moc': {},`.
sed -i \
  -e "s/^\(\s*\)'focaltech_moc',/\1'focaltech0752',\n&/" \
  -e "s/^\(\s*\)'focaltech_moc': {},/\1'focaltech0752': {},\n&/" \
  "$src/meson.build"
sed -i \
  -e "s|^\(\s*\)'focaltech_moc'\s*:|\1'focaltech0752' : [ 'drivers/focaltech0752.c', 'drivers/focaltech_nn_match.c', 'drivers/focaltech_nn_infer.c' ],\n&|" \
  "$src/libfprint/meson.build"

for f in meson.build libfprint/meson.build; do
  grep -q "'focaltech0752'" "$src/$f" || {
    echo "apply-driver.sh: failed to register driver in $f" >&2
    exit 1
  }
done

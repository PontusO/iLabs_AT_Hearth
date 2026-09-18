#!/usr/bin/env bash
#
# sdk-prepare.sh - copy the pinned Silicon Labs Matter extension out of the
# Conan cache into $MATTER_EXT_ROOT, apply Hearth's SDK patches, and stamp the
# copy with the patch revision the build gate checks.
#
# Why a copy at all: slc writes build output beside the project it is given and
# the extension's own slc/build.sh writes out/ beside the .slcp, which would
# land inside ~/.silabs/slt/installs/ where an `slt install` or `slt update` can
# drop it without warning. The Conan cache is SLT's, not ours; the copy is ours.
#
# Why a stamp and not a "did the patch apply" check: the patch DEFAULTS TO
# STOCK BEHAVIOUR when its configuration macro is unset, which is what makes it
# upstreamable and is exactly what makes its absence invisible. An unpatched
# tree compiles, links and runs; it just quietly gives the RAM back. Worse, a
# tree carrying an OLDER cut of the same patch passes any presence check while
# running code nobody reviewed. So the stamp carries the REVISION, toolchain.env
# refuses to finish without it, and the two numbers are bumped together.
#
# Re-run this after any `slt install` or `slt update`: the cache is not ours,
# and a package revision that moves takes the prepared copy with it.
#
# THE COPY IS DISPOSABLE AND THIS SCRIPT REPLACES IT WHOLE. Anything written
# inside $MATTER_EXT_ROOT goes with it, including an out/ tree from building a
# stock example (README, "Building a stock example", is the recipe that makes
# one again). Hearth's own build directory is deliberately somewhere else.
#
# Usage:
#
#   source platform/silabs/toolchain.env      # see the note below
#   platform/silabs/fw/sdk-prepare.sh
#
# On a machine with no prepared tree yet, sourcing toolchain.env prints the
# "run platform/silabs/fw/sdk-prepare.sh" line and returns 1. That is the gate
# doing its job: every variable this script needs is exported before the gate
# runs, so the source is still good and this script runs straight afterwards.
# Source it again when the prepare is done and it returns 0.

set -euo pipefail

: "${MATTER_EXT_INSTALL:?source platform/silabs/toolchain.env first}"
: "${MATTER_EXT_ROOT:?source platform/silabs/toolchain.env first}"
: "${SISDK_ROOT:?source platform/silabs/toolchain.env first}"
: "${HEARTH_EEM_PATCH_REV:?source platform/silabs/toolchain.env first}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # platform/silabs
PATCHES="$HERE/sdk-patches/matter_sdk"
REV="$HEARTH_EEM_PATCH_REV"
MARKER_FILE="$MATTER_EXT_ROOT/third_party/matter_sdk/src/app/clusters/electrical-energy-measurement-server/ElectricalEnergyMeasurementCluster.cpp"

if [ ! -d "$MATTER_EXT_INSTALL/third_party/matter_sdk" ]; then
    echo "sdk-prepare.sh: $MATTER_EXT_INSTALL does not look like the matter extension" >&2
    exit 1
fi

echo "copying $MATTER_EXT_INSTALL"
echo "     to $MATTER_EXT_ROOT (replacing whatever is there)"
rm -rf "$MATTER_EXT_ROOT"
mkdir -p "$(dirname "$MATTER_EXT_ROOT")"
cp -a "$MATTER_EXT_INSTALL" "$MATTER_EXT_ROOT"

# The extension expects both SDKs under third_party/. SISDK_ROOT is honoured by
# slc/build.sh directly so the first link is belt and braces, but build.sh
# passes the wifi_sdk path to slc unconditionally, so the second one is not.
# Symlinks rather than copies: these are read-only trees an order of magnitude
# larger than the extension.
ln -sfn "$SISDK_ROOT" "$MATTER_EXT_ROOT/third_party/simplicity_sdk"
if command -v slt >/dev/null 2>&1; then
    wifi_sdk="$(slt where wiseconnect | tail -1)"
    if [ -n "$wifi_sdk" ] && [ -d "$wifi_sdk" ]; then
        ln -sfn "$wifi_sdk" "$MATTER_EXT_ROOT/third_party/wifi_sdk"
    fi
fi

shopt -s nullglob
patches=("$PATCHES"/*.patch)
shopt -u nullglob
if [ ${#patches[@]} -eq 0 ]; then
    echo "sdk-prepare.sh: no patches found in $PATCHES" >&2
    exit 1
fi

for p in "${patches[@]}"; do
    name="$(basename "$p")"
    # sha256sum -c resolves the name in the checksum file against the working
    # directory, so check from the directory the patch lives in. A patch edited
    # without refreshing its hash fails here rather than applying something
    # nobody reviewed.
    ( cd "$PATCHES" && sha256sum -c "$name.sha256" )
    echo "applying $name"
    patch -p1 --no-backup-if-mismatch -d "$MATTER_EXT_ROOT/third_party/matter_sdk" < "$p"
done

# The revision gate's positive control: the marker really is in the tree that
# was just patched, at the number toolchain.env will look for.
if ! grep -q "HEARTH_EEM_POOL_PATCH_REV $REV" "$MARKER_FILE"; then
    echo "sdk-prepare.sh: HEARTH_EEM_POOL_PATCH_REV $REV not found in the patched tree" >&2
    echo "  $MARKER_FILE" >&2
    exit 1
fi

printf 'HEARTH_EEM_POOL_PATCH_REV %s\n' "$REV" > "$MATTER_EXT_ROOT/.hearth-stamp"
echo "prepared $MATTER_EXT_ROOT at patch revision $REV"

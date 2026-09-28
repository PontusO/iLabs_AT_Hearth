#!/usr/bin/env bash
#
# sdk-prepare.sh - copy the pinned Silicon Labs Matter extension out of the
# Conan cache into $MATTER_EXT_ROOT, apply Hearth's SDK patches (against BOTH
# trees this tree carries patches for, see below), and stamp the copy with
# the patch revision the build gate checks.
#
# Why a copy at all: slc writes build output beside the project it is given and
# the extension's own slc/build.sh writes out/ beside the .slcp, which would
# land inside ~/.silabs/slt/installs/ where an `slt install` or `slt update` can
# drop it without warning. The Conan cache is SLT's, not ours; the copy is ours.
#
# Why a stamp and not a "did the patch apply" check: the matter_sdk patch
# DEFAULTS TO STOCK BEHAVIOUR when its configuration macro is unset, which is
# what makes it upstreamable and is exactly what makes its absence invisible.
# An unpatched tree compiles, links and runs; it just quietly gives the RAM
# back. Worse, a tree carrying an OLDER cut of the same patch passes any
# presence check while running code nobody reviewed. So the stamp carries the
# REVISION, toolchain.env refuses to finish without it, and the numbers are
# bumped together whenever any patch in either tree changes.
#
# HEARTH_SDK_PATCH_REV stamps the WHOLE prepared tree, both patch directories
# together, not one patch. It was HEARTH_EEM_PATCH_REV through the
# ElectricalEnergyMeasurement instance-pool patch alone; renamed when the
# extension/ tree (below) joined it, because a name naming one patch while
# gating both would mislead the next person to add a third.
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
: "${HEARTH_SDK_PATCH_REV:?source platform/silabs/toolchain.env first}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # platform/silabs
PATCHES_MATTER_SDK="$HERE/sdk-patches/matter_sdk"
PATCHES_EXTENSION="$HERE/sdk-patches/extension"
REV="$HEARTH_SDK_PATCH_REV"
EEM_MARKER_FILE="$MATTER_EXT_ROOT/third_party/matter_sdk/src/app/clusters/electrical-energy-measurement-server/ElectricalEnergyMeasurementCluster.cpp"
BOOLSTATE_TEMPLATE="$MATTER_EXT_ROOT/src/app/zap-templates/templates/app/slc_compatibility/static-cluster-config-BooleanState.zapt"
EXTENSION_TEMPLATES_JSON="$MATTER_EXT_ROOT/src/app/zap-templates/app-templates.json"

if [ ! -d "$MATTER_EXT_INSTALL/third_party/matter_sdk" ]; then
    echo "sdk-prepare.sh: $MATTER_EXT_INSTALL does not look like the matter extension" >&2
    exit 1
fi

# The copy is disposable, but only if what is there IS the copy. The next line
# is an rm -rf of a path that arrives in a variable, from toolchain.env, which a
# mistyped edit or a stray export in the shell can point anywhere. So the path
# has to look like a prepared tree before it is deleted: either it does not
# exist yet, or it carries this script's own stamp, or the extension's
# third_party/matter_sdk is inside it. Anything else is refused by name, because
# a wrong MATTER_EXT_ROOT is a lost directory and not a failed build.
if [ -e "$MATTER_EXT_ROOT" ] \
   && [ ! -e "$MATTER_EXT_ROOT/.hearth-stamp" ] \
   && [ ! -d "$MATTER_EXT_ROOT/third_party/matter_sdk" ]; then
    echo "sdk-prepare.sh: refusing to delete $MATTER_EXT_ROOT" >&2
    echo "  it exists and carries neither .hearth-stamp nor third_party/matter_sdk," >&2
    echo "  so it is not a tree this script prepared. Check MATTER_EXT_ROOT in" >&2
    echo "  platform/silabs/toolchain.env, or move that directory aside yourself." >&2
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

apply_patches() {
    # $1: the patch directory (sdk-patches/matter_sdk or sdk-patches/extension)
    # $2: the -d given to `patch`, i.e. the tree the patch's own a/ b/ paths
    #     are relative to (third_party/matter_sdk for the first, the
    #     extension root itself for the second: see sdk-patches/README.md,
    #     "Why the extension tree and not matter_sdk")
    local dir="$1" target="$2"
    shopt -s nullglob
    local patches=("$dir"/*.patch)
    shopt -u nullglob
    if [ ${#patches[@]} -eq 0 ]; then
        echo "sdk-prepare.sh: no patches found in $dir" >&2
        exit 1
    fi
    for p in "${patches[@]}"; do
        name="$(basename "$p")"
        # sha256sum -c resolves the name in the checksum file against the
        # working directory, so check from the directory the patch lives in.
        # A patch edited without refreshing its hash fails here rather than
        # applying something nobody reviewed.
        ( cd "$dir" && sha256sum -c "$name.sha256" )
        echo "applying $name"
        patch -p1 --no-backup-if-mismatch -d "$target" < "$p"
    done
}

apply_patches "$PATCHES_MATTER_SDK" "$MATTER_EXT_ROOT/third_party/matter_sdk"
apply_patches "$PATCHES_EXTENSION" "$MATTER_EXT_ROOT"

# The revision gate's positive control: each patch's evidence really is in the
# tree that was just patched. The matter_sdk patch carries its own
# HEARTH_EEM_POOL_PATCH_REV define (a macro, so the check is a specific
# revision, matching that patch's own re-cut history). The extension patch
# adds a file rather than editing one behind a guarded macro, so its evidence
# is presence: the new template exists, and app-templates.json's own entry
# for it is in the tree that was just patched.
if ! grep -q "HEARTH_EEM_POOL_PATCH_REV 2" "$EEM_MARKER_FILE"; then
    echo "sdk-prepare.sh: HEARTH_EEM_POOL_PATCH_REV 2 not found in the patched tree" >&2
    echo "  $EEM_MARKER_FILE" >&2
    exit 1
fi
if [ ! -f "$BOOLSTATE_TEMPLATE" ]; then
    echo "sdk-prepare.sh: the BooleanState static-cluster-config template is missing" >&2
    echo "  $BOOLSTATE_TEMPLATE" >&2
    exit 1
fi
if ! grep -q '"BooleanState static cluster configuration"' "$EXTENSION_TEMPLATES_JSON"; then
    echo "sdk-prepare.sh: app-templates.json has no BooleanState static cluster configuration entry" >&2
    echo "  $EXTENSION_TEMPLATES_JSON" >&2
    exit 1
fi

printf 'HEARTH_SDK_PATCH_REV %s\n' "$REV" > "$MATTER_EXT_ROOT/.hearth-stamp"
echo "prepared $MATTER_EXT_ROOT at patch revision $REV"

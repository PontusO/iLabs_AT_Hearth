#!/usr/bin/env bash
#
# zap-regen.sh - the only producer of platform/silabs/data_model/zap-generated/.
#
# The data model is one file, data_model/hearth.zap, and one command: this one.
# Nothing else in this repository writes into data_model/zap-generated/, and no
# file under it is ever hand-edited. A data-model change is a .zap edit and a
# run of this script, in one commit.
#
# THERE IS A SECOND GENERATOR, and it is not a second SOURCE. slc-cli runs ZAP
# itself for any project carrying a `config_file:` with `file_id: zap_config`,
# which hearth.slcp does, and writes the result into the build directory's
# autogen/zap-generated/. That is the tree the build actually compiles. It is
# the same binary (the SLT zap package, reached through
# STUDIO_ADAPTER_PACK_PATH), the same templates and the same .zap file, and the
# two trees were compared file by file on 2026-09-18.
#
# They agreed everywhere but one file, and the reason is worth knowing before
# anyone chases it: zap keeps a persistent sqlite state directory, and on this
# bench that directory is the SLT zap package itself, because toolchain.env
# exports ZAP_DIR for slt and zap reads ZAP_DIR as its own state directory. A
# state database that has other ZCL packages loaded from earlier runs emits
# three extra privilege rows in access.h, for an attribute
# (BasicInformation::LocalConfigDisabled) this data model does not carry and
# cannot reach. A FRESH state directory does not, whether it is temporary or
# not: verified both ways.
#
# So this script runs with --tempState, and the committed tree is the one that
# regenerates identically on any machine. The build's own copy may carry those
# three inert rows; that is a property of zap's state, not of the data model.
#
# What the committed tree is for, then: review and record. A .zap is 380 KB of
# JSON nobody can read a change out of, while endpoint_config.h says in one
# diff what a data-model change did to the wire surface, and FIXED_ENDPOINT_COUNT
# and ATTRIBUTE_LARGEST are read out of it by later tasks.
#
# Usage:
#
#   source platform/silabs/toolchain.env
#   platform/silabs/fw/zap-regen.sh            # write data_model/zap-generated
#   platform/silabs/fw/zap-regen.sh --check    # regenerate to a temp dir and
#                                              # diff; exit 1 on any difference

set -euo pipefail

: "${MATTER_EXT_ROOT:?source platform/silabs/toolchain.env first}"
: "${HEARTH_ZAP_CLI:?source platform/silabs/toolchain.env first}"
: "${HEARTH_ZAP_ZCL:?source platform/silabs/toolchain.env first}"
: "${HEARTH_ZAP_TEMPLATES:?source platform/silabs/toolchain.env first}"
: "${HEARTH_ZAP_VERSION:?source platform/silabs/toolchain.env first}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # platform/silabs
ZAP_FILE="$HERE/data_model/hearth.zap"
OUT="$HERE/data_model/zap-generated"

CHECK=0
if [ "${1:-}" = "--check" ]; then
    CHECK=1
    shift
fi
if [ $# -ne 0 ]; then
    echo "zap-regen.sh: unexpected argument '$1'" >&2
    exit 2
fi

for f in "$HEARTH_ZAP_CLI" "$HEARTH_ZAP_ZCL" "$HEARTH_ZAP_TEMPLATES" "$ZAP_FILE"; do
    if [ ! -e "$f" ]; then
        echo "zap-regen.sh: missing $f" >&2
        exit 1
    fi
done

generate() {
    # --tempState: a fresh state database per run. It is what makes this output
    # reproducible (see the note at the top), and it also keeps the run from
    # writing into the SLT zap package inside the Conan cache, which is where
    # zap would otherwise put its sqlite and its log.
    # -z and -g override the package paths recorded inside the .zap, which are
    # absolute and therefore machine-specific; see the README's "Data model".
    "$HEARTH_ZAP_CLI" generate \
        --noUi --noServer --tempState \
        -z "$HEARTH_ZAP_ZCL" \
        -g "$HEARTH_ZAP_TEMPLATES" \
        -i "$ZAP_FILE" \
        -o "$1"
}

if [ "$CHECK" -eq 1 ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    generate "$tmp" > "$tmp.log" 2>&1 || { cat "$tmp.log"; exit 1; }
    if diff -r -q "$OUT" "$tmp" > /dev/null; then
        echo "zap-generated is current (zap $HEARTH_ZAP_VERSION)"
    else
        echo "zap-generated is STALE against $ZAP_FILE (zap $HEARTH_ZAP_VERSION):" >&2
        diff -r -q "$OUT" "$tmp" >&2 || true
        echo "run platform/silabs/fw/zap-regen.sh and commit the result" >&2
        exit 1
    fi
else
    rm -rf "$OUT"
    mkdir -p "$OUT"
    generate "$OUT"
    echo "regenerated $OUT with zap $HEARTH_ZAP_VERSION"
fi

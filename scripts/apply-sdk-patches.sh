#!/usr/bin/env bash
# Applies the Hearth esp-matter patchset to the pinned SDK checkout.
# Refuses on base-commit drift so an SDK bump re-evaluates the patches
# deliberately (design spec 2026-08-01, section 3).
set -euo pipefail
ESP_MATTER="${ESP_MATTER_PATH:-$HOME/esp/esp-matter}"
PINNED=21aa3d1
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PATCHES="$HERE/sdk-patches/esp-matter"

head=$(git -C "$ESP_MATTER" rev-parse --short HEAD)
if [ "$head" != "$PINNED" ]; then
    echo "refuse: esp-matter at $head, patchset pinned to $PINNED" >&2
    exit 1
fi
case "${1:-apply}" in
  --check)
    for p in "$PATCHES"/*.patch; do
        if git -C "$ESP_MATTER" apply --check --reverse "$p" 2>/dev/null; then
            echo "applied: $(basename "$p")"
        elif git -C "$ESP_MATTER" apply --check "$p" 2>/dev/null; then
            echo "not applied: $(basename "$p")"
        else
            echo "DRIFT: $(basename "$p") fits neither way" >&2; exit 1
        fi
    done ;;
  --revert)
    for p in "$PATCHES"/*.patch; do
        git -C "$ESP_MATTER" apply --reverse "$p" && echo "reverted: $(basename "$p")"
    done ;;
  apply)
    for p in "$PATCHES"/*.patch; do
        if git -C "$ESP_MATTER" apply --check --reverse "$p" 2>/dev/null; then
            echo "already applied: $(basename "$p")"
        else
            git -C "$ESP_MATTER" apply "$p" && echo "applied: $(basename "$p")"
        fi
    done ;;
  *) echo "usage: $0 [apply|--check|--revert]" >&2; exit 2 ;;
esac

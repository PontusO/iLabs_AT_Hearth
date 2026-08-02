#!/usr/bin/env bash
# Applies the Hearth SDK patchset to the pinned SDK checkouts: esp-matter
# itself, and the CHIP core tree nested inside it. Refuses on base-commit
# drift (either repo) so an SDK bump re-evaluates the patches deliberately
# (design spec 2026-08-01, section 3). B83 widened the patchset from
# esp-matter-only to a second, CHIP-core repo.
set -euo pipefail
ESP_MATTER="${ESP_MATTER_PATH:-$HOME/esp/esp-matter}"
ESP_MATTER_PINNED=21aa3d1
CHIP="${CHIP_PATH:-$HOME/esp/esp-matter/connectedhomeip/connectedhomeip}"
CHIP_PINNED=b87051a9
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ESP_MATTER_PATCHES="$HERE/sdk-patches/esp-matter"
CHIP_PATCHES="$HERE/sdk-patches/connectedhomeip"

esp_matter_head=$(git -C "$ESP_MATTER" rev-parse --short HEAD)
if [ "$esp_matter_head" != "$ESP_MATTER_PINNED" ]; then
    echo "refuse: esp-matter at $esp_matter_head, patchset pinned to $ESP_MATTER_PINNED" >&2
    exit 1
fi
chip_head=$(git -C "$CHIP" rev-parse --short HEAD)
if [ "$chip_head" != "$CHIP_PINNED" ]; then
    echo "refuse: connectedhomeip at $chip_head, patchset pinned to $CHIP_PINNED" >&2
    exit 1
fi
case "${1:-apply}" in
  --check)
    for p in "$ESP_MATTER_PATCHES"/*.patch; do
        if git -C "$ESP_MATTER" apply --check --reverse "$p" 2>/dev/null; then
            echo "applied: $(basename "$p")"
        elif git -C "$ESP_MATTER" apply --check "$p" 2>/dev/null; then
            echo "not applied: $(basename "$p")"
        else
            echo "DRIFT: $(basename "$p") fits neither way" >&2; exit 1
        fi
    done
    for p in "$CHIP_PATCHES"/*.patch; do
        if git -C "$CHIP" apply --check --reverse "$p" 2>/dev/null; then
            echo "applied: $(basename "$p")"
        elif git -C "$CHIP" apply --check "$p" 2>/dev/null; then
            echo "not applied: $(basename "$p")"
        else
            echo "DRIFT: $(basename "$p") fits neither way" >&2; exit 1
        fi
    done ;;
  --revert)
    for p in "$ESP_MATTER_PATCHES"/*.patch; do
        git -C "$ESP_MATTER" apply --reverse "$p" && echo "reverted: $(basename "$p")"
    done
    for p in "$CHIP_PATCHES"/*.patch; do
        git -C "$CHIP" apply --reverse "$p" && echo "reverted: $(basename "$p")"
    done ;;
  apply)
    for p in "$ESP_MATTER_PATCHES"/*.patch; do
        if git -C "$ESP_MATTER" apply --check --reverse "$p" 2>/dev/null; then
            echo "already applied: $(basename "$p")"
        else
            git -C "$ESP_MATTER" apply "$p" && echo "applied: $(basename "$p")"
        fi
    done
    for p in "$CHIP_PATCHES"/*.patch; do
        if git -C "$CHIP" apply --check --reverse "$p" 2>/dev/null; then
            echo "already applied: $(basename "$p")"
        else
            git -C "$CHIP" apply "$p" && echo "applied: $(basename "$p")"
        fi
    done ;;
  *) echo "usage: $0 [apply|--check|--revert]" >&2; exit 2 ;;
esac

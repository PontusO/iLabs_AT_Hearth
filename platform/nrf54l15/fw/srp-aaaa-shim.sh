#!/bin/bash
#
# srp-aaaa-shim.sh - bench workaround for an upstream OTBR defect.
#
# OT-core's native mDNS module (ot-br-posix built with
# OTBR_MDNS=openthread; verified present in both the 2025 snapshot
# c244ca2 and release v2026.08.0) mishandles the SRP host update that
# follows a device's fast-attach registration race: a Thread device
# registers its SRP host before it holds an OMR address and updates
# roughly a second later with the address, and the mDNS responder keeps
# answering AAAA queries for that host with an NSEC denial even after
# the update (packet-verified 2026-08-28: SRV answered, AAAA answered
# with an=0 plus NSEC). Same-host resolvers (avahi, chip-tool) then
# fail operational discovery with "Avahi resolve failed" while
# off-host resolvers that cached the announcement burst may succeed.
#
# This shim mirrors the SRP server's registered host address into
# avahi for the duration of a commissioning or control session:
# avahi then answers the AAAA authoritatively with a record that is
# true by construction (it is read from the SRP server itself).
# Run it alongside any chip-tool session on the bench host:
#
#   ./srp-aaaa-shim.sh &        # 3 minute lifetime, self-cleaning
#   ./srp-aaaa-shim.sh 1200 &   # or give it a lifetime in seconds
#
# A harness Phase 2 run commissions three times over about ten minutes,
# so it wants a lifetime that covers the whole run: a shim that expires
# mid-run takes its avahi record down with it and the next commissioning
# fails exactly as it would with no shim at all.
#
# Remove once the upstream mDNS host-update defect is fixed and the
# border router is upgraded past it.
#
# TWO ROUTES TO THE SAME PAIR (host name, address), 2026-09-21.
# `ot-ctl srp server host` is the direct one and is tried first. It fails
# on a bench where /run/openthread-wpan0.sock is root-owned and the
# session runs as an ordinary user: every ot-ctl answers "connect session
# failed: Permission denied" (graph F474/F503, the same wall that made
# the harness's Phase 2 gate read the border router over D-Bus instead).
# There is no D-Bus property for the SRP host table, so the fallback here
# is otbr-agent's own journal, which logs every accepted SRP update:
#
#   SrpServer-----:     Host:<16 hex>.default.service.arpa.
#   SrpServer-----:     1 host address(es):
#   SrpServer-----:       fd..:..
#
# and logs "No host address" for the de-registration half of the very
# race this shim exists for. The journal is read-only and needs no
# privilege on a systemd-journal-readable host. If neither route yields a
# pair the shim publishes nothing, which is what it did before.

OTCTL=/mnt/f86c891c-33c6-4bb7-afe1-2c8846257177/src/git/ot-br-posix/build/otbr/third_party/openthread/repo/src/posix/ot-ctl
JOURNAL_UNIT=otbr-agent
LIFETIME=${1:-180}
PUBPID=""
LAST=""

# Last (host, address) pair otbr-agent's journal shows as REGISTERED in
# the recent past: an address line is only taken while a Host: line is in
# scope, and "No host address" takes that host back out of scope, so a
# de-registered host cannot be published from a stale line.
journal_pair() {
  journalctl -u "$JOURNAL_UNIT" --since "-2min" --no-pager 2>/dev/null \
    | grep -E "SrpServer" \
    | awk '
        /Host:[0-9A-F]{16}\.default/ {
          i = index($0, "Host:"); host = substr($0, i + 5, 16); next
        }
        /No host address/ { host = ""; next }
        /^.*SrpServer[-]*: +fd[0-9a-f:]+$/ {
          if (host != "") { h = host; a = $NF }
          next
        }
        END { if (h != "") print h, a }'
}

trap '[ -n "$PUBPID" ] && kill $PUBPID 2>/dev/null' EXIT
for i in $(seq "$LIFETIME"); do
  OUT=$($OTCTL srp server host 2>/dev/null)
  HOST=$(echo "$OUT" | grep -oE '^[0-9A-F]{16}\.default' | head -1 | cut -d. -f1)
  ADDR=$(echo "$OUT" | grep -oE 'fd[0-9a-f:]+' | head -1)
  if [ -z "$HOST" ] || [ -z "$ADDR" ]; then
    PAIR=$(journal_pair)
    if [ -n "$PAIR" ]; then
      HOST=${PAIR%% *}
      ADDR=${PAIR##* }
    fi
  fi
  if [ -n "$HOST" ] && [ -n "$ADDR" ]; then
    KEY="$HOST/$ADDR"
    if [ "$KEY" != "$LAST" ]; then
      [ -n "$PUBPID" ] && kill $PUBPID 2>/dev/null
      avahi-publish -a -R "$HOST.local" "$ADDR" >/dev/null 2>&1 &
      PUBPID=$!
      echo "shim: publishing $HOST.local" >&2
      LAST="$KEY"
    fi
  fi
  sleep 1
done

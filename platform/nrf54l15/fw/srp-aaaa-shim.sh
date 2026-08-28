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
#
# Remove once the upstream mDNS host-update defect is fixed and the
# border router is upgraded past it.

OTCTL=/mnt/f86c891c-33c6-4bb7-afe1-2c8846257177/src/git/ot-br-posix/build/otbr/third_party/openthread/repo/src/posix/ot-ctl
PUBPID=""
LAST=""
trap '[ -n "$PUBPID" ] && kill $PUBPID 2>/dev/null' EXIT
for i in $(seq 180); do
  OUT=$($OTCTL srp server host 2>/dev/null)
  HOST=$(echo "$OUT" | grep -oE '^[0-9A-F]{16}\.default' | head -1 | cut -d. -f1)
  ADDR=$(echo "$OUT" | grep -oE 'fd[0-9a-f:]+' | head -1)
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

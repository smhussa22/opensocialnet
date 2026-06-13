#!/usr/bin/env bash
# Two-client ICE test: alice + bob authenticate against a signaling
# gateway, exchange ICE SDPs over the chat path, and stream voice
# DIRECTLY peer-to-peer over the libnice-nominated candidate pair — the
# media relay is never touched. Works against the local gateway
# (default) or the EC2 one (SIGNALING_IP=<ec2-ip>).
#
# usage:
#   OPENSOCIALNET_AUTH_SECRET=devsecret123 scripts/run-ice-test.sh [seconds]
# env:
#   SIGNALING_IP   gateway host                  (default 127.0.0.1)
#   OSN_ROOM       voice room to join            (default general)
#   OSN_STUN_HOST / OSN_TURN_* forwarded to both clients (optional —
#   host candidates alone connect two clients on one machine)
set -uo pipefail

SECS="${1:-20}"
SIGNALING_IP="${SIGNALING_IP:-127.0.0.1}"
ROOM="${OSN_ROOM:-general}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"

: "${OPENSOCIALNET_AUTH_SECRET:?set OPENSOCIALNET_AUTH_SECRET (same hmac secret the gateway runs with)}"

pkill -f "$REPO/src/network/build/network" 2>/dev/null || true
sleep 0.5

PIDS=()
for NAME in alice bob; do
  TOKEN="$(printf %s "$NAME" | openssl dgst -sha256 -hmac "$OPENSOCIALNET_AUTH_SECRET" | awk '{print $NF}')"
  echo "=== starting $NAME (signaling ws://$SIGNALING_IP:9001/gateway, ICE on) ==="
  OSN_ICE=1 OSN_SIGNALING_HOST="$SIGNALING_IP" OSN_ROOM="$ROOM" OSN_USER="$NAME" OSN_AUTH_TOKEN="$TOKEN" \
    OSN_STUN_HOST="${OSN_STUN_HOST:-}" OSN_STUN_PORT="${OSN_STUN_PORT:-3478}" \
    OSN_TURN_HOST="${OSN_TURN_HOST:-}" OSN_TURN_PORT="${OSN_TURN_PORT:-3478}" \
    OSN_TURN_USER="${OSN_TURN_USER:-}" OSN_TURN_PASS="${OSN_TURN_PASS:-}" \
    OSN_LOCAL_PORT=0 \
    stdbuf -oL -eL "$REPO/src/network/build/network" >"/tmp/$NAME-ice.log" 2>&1 &
  PIDS+=($!)
  sleep 1
done

echo "=== running for ${SECS}s (logs: /tmp/alice-ice.log /tmp/bob-ice.log) ==="
sleep "$SECS"

kill "${PIDS[@]}" 2>/dev/null
sleep 1

for NAME in alice bob; do
  echo
  echo "=== $NAME: ICE negotiation ==="
  grep -E '\[ice\]|ICE' "/tmp/$NAME-ice.log" | head -20
  echo "=== $NAME: last net-stats ==="
  grep 'net-stats' "/tmp/$NAME-ice.log" | tail -2
done

#!/usr/bin/env bash
# End-to-end voice test against the EC2 deployment: N clients each do the
# real signaling handshake (Hello -> Ready -> JoinVoice) on the gateway,
# get told where the relay lives + their ssrc, then stream mic/opus
# Packets through the EC2 relay to each other.
#
# usage:
#   OPENSOCIALNET_AUTH_SECRET=... scripts/run-ec2-voice-test.sh [seconds] [n_clients]
# env:
#   EC2_IP   gateway + relay host (default 3.144.229.204)
#   OSN_ROOM voice room to join   (default general)
set -uo pipefail

SECS="${1:-20}"
N="${2:-2}"
EC2_IP="${EC2_IP:-3.144.229.204}"
ROOM="${OSN_ROOM:-general}"
REPO="/home/faraz/udptcp"
NAMES=(alice bob carol dave erin frank grace heidi ivan judy)

: "${OPENSOCIALNET_AUTH_SECRET:?set OPENSOCIALNET_AUTH_SECRET (same hmac secret the gateway runs with)}"

pkill -f "$REPO/src/network/build/network" 2>/dev/null || true
sleep 0.5

PIDS=()
for i in $(seq 0 $((N - 1))); do
  NAME="${NAMES[$i]}"
  PORT=$((50301 + i))
  TOKEN="$(printf %s "$NAME" | openssl dgst -sha256 -hmac "$OPENSOCIALNET_AUTH_SECRET" | awk '{print $NF}')"
  echo "=== starting $NAME (local :$PORT, signaling ws://$EC2_IP:9001/gateway) ==="
  OSN_SIGNALING_HOST="$EC2_IP" OSN_ROOM="$ROOM" OSN_USER="$NAME" OSN_AUTH_TOKEN="$TOKEN" OSN_LOCAL_PORT="$PORT" \
    stdbuf -oL -eL "$REPO/src/network/build/network" >"/tmp/$NAME-ec2.log" 2>&1 &
  PIDS+=($!)
  sleep 1
done

echo "=== running for ${SECS}s ==="
sleep "$SECS"

echo "=== shutting down ==="
kill "${PIDS[@]}" 2>/dev/null
sleep 1

for i in $(seq 0 $((N - 1))); do
  NAME="${NAMES[$i]}"
  echo
  echo "=== /tmp/$NAME-ec2.log (signaling + mixer + last 5 stats) ==="
  grep -E '\[signaling\]|\[mixer\]|routing identity' "/tmp/$NAME-ec2.log" | head -15
  echo "  ..."
  grep '\[net-stats\]' "/tmp/$NAME-ec2.log" | tail -5
done

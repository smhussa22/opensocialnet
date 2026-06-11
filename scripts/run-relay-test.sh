#!/usr/bin/env bash
# Helper for orchestrating the local relay smoke test: starts relay + N
# clients in the background, captures their stdout/stderr into /tmp/*.log,
# sleeps a configurable amount of time so the relay can accumulate
# [relay-stats] lines, then kills everything.
#
# usage: scripts/run-relay-test.sh [seconds] [n_clients] (default 20s, 3)
# env passthrough: SIM_LOSS_PCT / SIM_JITTER_MS / SIM_OOO_PCT exercise the
# per-source PLC + jitter buffers under adversarial conditions.
set -uo pipefail

SECS="${1:-20}"
N="${2:-3}"
REPO="/home/faraz/udptcp"
NAMES=(alice bob carol dave erin frank grace heidi ivan judy)

# Reap any leftover binaries from a previous run.
pkill -f "$REPO/src/relay/build/relay"   2>/dev/null || true
pkill -f "$REPO/src/network/build/network" 2>/dev/null || true
sleep 0.5

echo "=== starting relay ==="
"$REPO/src/relay/build/relay" >/tmp/relay.log 2>&1 &
RELAY_PID=$!
sleep 1

PIDS=()
for i in $(seq 0 $((N - 1))); do
  NAME="${NAMES[$i]}"
  PORT=$((50201 + i))
  echo "=== starting $NAME (local :$PORT) ==="
  OSN_ROOM=general OSN_USER="$NAME" OSN_RELAY_HOST=127.0.0.1 OSN_LOCAL_PORT="$PORT" \
    SIM_LOSS_PCT="${SIM_LOSS_PCT:-0}" SIM_JITTER_MS="${SIM_JITTER_MS:-0}" SIM_OOO_PCT="${SIM_OOO_PCT:-0}" \
    OSN_ADAPTIVE_JB="${OSN_ADAPTIVE_JB:-0}" \
    "$REPO/src/network/build/network" >"/tmp/$NAME.log" 2>&1 &
  PIDS+=($!)
  sleep 0.5
done

echo "=== running for ${SECS}s ==="
sleep "$SECS"

echo "=== shutting down ==="
kill "${PIDS[@]}" 2>/dev/null
sleep 1
kill "$RELAY_PID" 2>/dev/null
sleep 1

echo
echo "=== /tmp/relay.log (last 8 lines) ==="
tail -8 /tmp/relay.log

for i in $(seq 0 $((N - 1))); do
  NAME="${NAMES[$i]}"
  echo
  echo "=== /tmp/$NAME.log ([mixer] lines + last 6) ==="
  grep '\[mixer\]' "/tmp/$NAME.log" | head -12
  echo "  ..."
  tail -6 "/tmp/$NAME.log"
done

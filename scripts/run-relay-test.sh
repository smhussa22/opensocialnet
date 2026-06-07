#!/usr/bin/env bash
# Helper for orchestrating the local relay smoke test: starts relay + two
# clients in the background, captures their stdout/stderr into /tmp/*.log,
# sleeps a configurable amount of time so the relay can accumulate
# [relay-stats] lines, then kills everything.
#
# usage: scripts/run-relay-test.sh [seconds] (default 20)
set -uo pipefail

SECS="${1:-20}"
REPO="/home/faraz/udptcp"

# Reap any leftover binaries from a previous run.
pkill -f "$REPO/src/relay/build/relay"   2>/dev/null || true
pkill -f "$REPO/src/network/build/network" 2>/dev/null || true
sleep 0.5

echo "=== starting relay ==="
"$REPO/src/relay/build/relay" >/tmp/relay.log 2>&1 &
RELAY_PID=$!
sleep 1

echo "=== starting alice ==="
OSN_ROOM=general OSN_USER=alice OSN_RELAY_HOST=127.0.0.1 OSN_LOCAL_PORT=50201 \
    "$REPO/src/network/build/network" >/tmp/alice.log 2>&1 &
ALICE_PID=$!
sleep 1

echo "=== starting bob ==="
OSN_ROOM=general OSN_USER=bob OSN_RELAY_HOST=127.0.0.1 OSN_LOCAL_PORT=50202 \
    "$REPO/src/network/build/network" >/tmp/bob.log 2>&1 &
BOB_PID=$!

echo "=== running for ${SECS}s ==="
sleep "$SECS"

echo "=== shutting down ==="
kill "$ALICE_PID" "$BOB_PID" 2>/dev/null
sleep 1
kill "$RELAY_PID" 2>/dev/null
sleep 1

echo
echo "=== /tmp/relay.log (last 8 lines) ==="
tail -8 /tmp/relay.log
echo
echo "=== /tmp/alice.log (head 25 + last 6) ==="
head -25 /tmp/alice.log
echo "  ..."
tail -6 /tmp/alice.log
echo
echo "=== /tmp/bob.log (head 25 + last 6) ==="
head -25 /tmp/bob.log
echo "  ..."
tail -6 /tmp/bob.log

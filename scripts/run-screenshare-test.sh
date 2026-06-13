#!/usr/bin/env bash
# One-command local screenshare test. WSLg has no real X11 desktop (the
# root window is always black), so the screen-sharer auto-latches onto
# the largest X11 window instead. To give it something moving to grab,
# bob sends synthetic video which alice renders in a window:
#   - alice's window shows bob's bouncing square (camera lane)
#   - alice screen-captures that very window and sends it (screen lane)
#   - bob's window, titled "peer ... screen", shows the same square —
#     arriving via the SCREENSHARE path. Two windows, same content = proof.
# Ctrl+C on alice tears the whole test down.
#
# usage: scripts/run-screenshare-test.sh
# grab a specific window instead: OSN_SCREEN_WINDOW=0x<id> (see xwininfo)
# loss test: SIM_LOSS_PCT=15 SIM_JITTER_MS=30 scripts/run-screenshare-test.sh
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"

pkill -f "$REPO/src/relay/build/relay" 2>/dev/null || true
pkill -f "$REPO/src/network/build/network" 2>/dev/null || true
sleep 0.5

echo "=== starting relay (log: /tmp/relay.log) ==="
"$REPO/src/relay/build/relay" >/tmp/relay.log 2>&1 &
RELAY_PID=$!
sleep 1

echo "=== starting bob: synthetic video sender + screen viewer (log: /tmp/bob.log) ==="
OSN_VIDEO=1 OSN_VIDEO_DEVICE=/dev/null OSN_ROOM=general OSN_USER=bob OSN_RELAY_HOST=127.0.0.1 OSN_LOCAL_PORT=50202 \
  "$REPO/src/network/build/network" >/tmp/bob.log 2>&1 &
BOB_PID=$!
sleep 1

echo "=== starting alice: SHARING HER WINDOW (Ctrl+C ends the whole test) ==="
OSN_SCREEN=1 OSN_SCREEN_WINDOW="${OSN_SCREEN_WINDOW:-0}" OSN_ROOM=general OSN_USER=alice OSN_RELAY_HOST=127.0.0.1 OSN_LOCAL_PORT=50201 \
  SIM_LOSS_PCT="${SIM_LOSS_PCT:-0}" SIM_JITTER_MS="${SIM_JITTER_MS:-0}" SIM_OOO_PCT="${SIM_OOO_PCT:-0}" \
  "$REPO/src/network/build/network"

echo "=== shutting down ==="
kill "$BOB_PID" "$RELAY_PID" 2>/dev/null
sleep 1
echo "=== bob's view of alice's screen ([vid-stats], last 4 lines) ==="
grep 'vid-stats' /tmp/bob.log | tail -4

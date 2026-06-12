#!/usr/bin/env bash
# One-command local video call test: starts the relay + a synthetic-video
# peer (bob) in the background, then runs the camera client (alice) in the
# foreground. One WSLg window opens per remote stream:
#   - alice's window shows bob's bouncing square
#   - bob's window shows YOUR camera (the proof the camera path works)
# Ctrl+C on alice tears the whole test down.
#
# usage: scripts/run-video-call.sh
# loss test (sim applies to the camera sender, so watch bob's window of
# you keep moving through the loss and bob's [vid-stats] plc counters):
#   SIM_LOSS_PCT=15 SIM_JITTER_MS=30 scripts/run-video-call.sh
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"

pkill -f "$REPO/src/relay/build/relay" 2>/dev/null || true
pkill -f "$REPO/src/network/build/network" 2>/dev/null || true
sleep 0.5

echo "=== starting relay (log: /tmp/relay.log) ==="
"$REPO/src/relay/build/relay" >/tmp/relay.log 2>&1 &
RELAY_PID=$!
sleep 1

# bob is forced onto the synthetic source (device /dev/null can never
# open) so he doesn't grab the camera before alice does.
echo "=== starting bob: synthetic video peer (log: /tmp/bob.log) ==="
OSN_VIDEO=1 OSN_ROOM=general OSN_USER=bob OSN_RELAY_HOST=127.0.0.1 OSN_LOCAL_PORT=50202 \
  OSN_VIDEO_DEVICE=/dev/null \
  "$REPO/src/network/build/network" >/tmp/bob.log 2>&1 &
BOB_PID=$!
sleep 1

echo "=== starting alice: YOUR CAMERA (Ctrl+C ends the whole test) ==="
OSN_VIDEO=1 OSN_ROOM=general OSN_USER=alice OSN_RELAY_HOST=127.0.0.1 OSN_LOCAL_PORT=50201 \
  SIM_LOSS_PCT="${SIM_LOSS_PCT:-0}" SIM_JITTER_MS="${SIM_JITTER_MS:-0}" SIM_OOO_PCT="${SIM_OOO_PCT:-0}" \
  "$REPO/src/network/build/network"

echo "=== shutting down ==="
kill "$BOB_PID" "$RELAY_PID" 2>/dev/null
sleep 1
echo "=== bob's view of your camera ([vid-stats], last 4 lines) ==="
grep 'vid-stats' /tmp/bob.log | tail -4

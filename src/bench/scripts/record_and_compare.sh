#!/usr/bin/env bash
# Record N seconds of your voice from the default mic, then run compare.sh
# on the recording so you hear back-to-back what fixed-vs-adaptive sounds
# like on YOUR voice (not the synthesized sweep).
#
# Env vars:
#   DURATION  seconds to record   (default 10)
#   LOSS      simulated loss %     (default 10)
#   JIT       simulated jitter ms  (default 30)
#   DEVICE    arecord -D value     (default unset = system default)

set -euo pipefail

DURATION="${DURATION:-10}"
LOSS="${LOSS:-10}"
JIT="${JIT:-30}"
DEVICE_FLAG=( )
if [[ -n "${DEVICE:-}" ]]; then DEVICE_FLAG=(-D "$DEVICE"); fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RECORDING="${RECORDING:-/tmp/recorded_voice.wav}"

if ! command -v arecord >/dev/null 2>&1; then
    echo "[record] arecord not installed. Install with: sudo apt install alsa-utils" >&2
    exit 1
fi

echo "[record] Recording $DURATION s from mic into $RECORDING ..."
echo "[record] Speak now."
arecord "${DEVICE_FLAG[@]}" -f S16_LE -r 48000 -c 1 -d "$DURATION" "$RECORDING"
echo "[record] Done. File: $RECORDING"

# Quick sanity playback so you know whether the recording actually
# captured anything (a common WSL failure mode is "device shows up but
# captures pure silence"). Suppress aplay's own output to keep the
# console clean.
if command -v aplay >/dev/null 2>&1; then
    echo "[record] Playing back the recording (mute speakers if you don't want feedback)..."
    aplay -q "$RECORDING" || true
fi

echo
echo "[record] Running compare.sh on the recording..."
LOSS="$LOSS" JIT="$JIT" IN="$RECORDING" "$SCRIPT_DIR/compare.sh"

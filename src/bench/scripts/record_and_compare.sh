#!/usr/bin/env bash
# Record N seconds of your voice from the default mic, then run compare.sh
# on the recording so you hear back-to-back what fixed-vs-adaptive sounds
# like on YOUR voice (not the synthesized sweep).
#
# Uses the `record` binary built alongside the network CLI (SDL-based)
# rather than arecord, so it works in WSL where ALSA has no card 0.
#
# Env vars:
#   DURATION         seconds to record           (default 10)
#   LOSS             simulated loss %             (default 10)
#   JIT              simulated jitter ms          (default 30)
#   OSN_AUDIO_INPUT  device-name substring match (default = SDL default)
#   RECORDING        output WAV path             (default /tmp/recorded_voice.wav)

set -euo pipefail

DURATION="${DURATION:-10}"
LOSS="${LOSS:-10}"
JIT="${JIT:-30}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
RECORDING="${RECORDING:-/tmp/recorded_voice.wav}"
RECORD_BIN="${RECORD_BIN:-$REPO_ROOT/src/network/build/record}"

if [[ ! -x "$RECORD_BIN" ]]; then
    echo "[record] $RECORD_BIN not found. Build it with:" >&2
    echo "         cd $REPO_ROOT/src/network && cmake --build build --target record -j" >&2
    exit 1
fi

echo "[record] Recording $DURATION s from mic into $RECORDING ..."
echo "[record] Speak now."
"$RECORD_BIN" --duration "$DURATION" --out "$RECORDING"
echo "[record] Done. File: $RECORDING"

# Sanity playback so you know whether the recording actually captured
# anything (a common WSL failure mode is "device shows up but captures
# pure silence"). aplay is alsa-utils; if it's missing, just skip.
if command -v aplay >/dev/null 2>&1; then
    echo "[record] Playing back via aplay (mute speakers if you do not want feedback) — Ctrl-C to skip..."
    aplay -q "$RECORDING" || true
fi

echo
echo "[record] Running compare.sh on the recording..."
LOSS="$LOSS" JIT="$JIT" IN="$RECORDING" "$SCRIPT_DIR/compare.sh"

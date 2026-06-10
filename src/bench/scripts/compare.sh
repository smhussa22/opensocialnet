#!/usr/bin/env bash
# Generate two WAVs from the same synthetic input — one through the
# fixed-depth jitter buffer, one through the adaptive one — at matched
# loss / jitter / seed so the audible difference is purely the buffer.
# Drops both files into the current dir and prints how to open them.

set -euo pipefail

BENCH="${BENCH:-./build/bench}"
DURATION="${DURATION:-10}"
LOSS="${LOSS:-10}"
JIT="${JIT:-30}"
SEED="${SEED:-42}"

OUT_DIR="${OUT_DIR:-$PWD}"
FIXED_WAV="$OUT_DIR/fixed.wav"
ADAPT_WAV="$OUT_DIR/adaptive.wav"

if [[ ! -x "$BENCH" ]]; then
    echo "[compare] $BENCH not found. Run ./build.sh from src/bench/ first." >&2
    exit 1
fi

echo "[compare] params: duration=${DURATION}s loss=${LOSS}% jitter=${JIT}ms seed=${SEED}"
echo

"$BENCH" --duration "$DURATION" --loss "$LOSS" --jitter-ms "$JIT" --seed "$SEED" --jb-mode fixed    --out "$FIXED_WAV" | tail -1
"$BENCH" --duration "$DURATION" --loss "$LOSS" --jitter-ms "$JIT" --seed "$SEED" --jb-mode adaptive --out "$ADAPT_WAV" | tail -1

echo
echo "[compare] WAVs written:"
echo "  fixed    -> $FIXED_WAV"
echo "  adaptive -> $ADAPT_WAV"
echo

# Convenience: on WSL, open Explorer to the WAV's directory so the user
# can double-click both files. On bare Linux, just print play hints.
if grep -qi microsoft /proc/version 2>/dev/null; then
    echo "[compare] WSL detected — opening Explorer to both files..."
    explorer.exe "$(wslpath -w "$FIXED_WAV")" || true
    explorer.exe "$(wslpath -w "$ADAPT_WAV")" || true
else
    echo "[compare] play with:  aplay $FIXED_WAV  /  aplay $ADAPT_WAV"
fi

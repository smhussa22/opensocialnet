#!/usr/bin/env bash
# Generate two WAVs through the bench — one with the fixed-depth jitter
# buffer, one with the adaptive one — at matched loss/jitter/seed so the
# audible difference is purely the buffer.
#
# Input:
#   - Pass a WAV path as $1, or set IN=/path/to/file.wav, to test real audio.
#   - Otherwise the bench synthesizes a 10s frequency sweep (the default).
#
# Real input is auto-resampled to 48 kHz mono via ffmpeg if it isn't
# already, since libopus only speaks the opus sample rates and the bench
# rejects anything that isn't 48 kHz.

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

IN="${IN:-${1:-synth}}"
BENCH_IN="$IN"

if [[ "$IN" != "synth" ]]; then

    if [[ ! -f "$IN" ]]; then
        echo "[compare] input file not found: $IN" >&2
        exit 1
    fi

    # If ffmpeg is present, always resample to be safe (it's a no-op on
    # the wire when the file already matches). If it isn't, hand the file
    # to bench as-is — bench rejects non-48k input with a clear ffmpeg
    # one-liner, so the user gets the same message either way.
    if command -v ffmpeg >/dev/null 2>&1; then
        BENCH_IN="$OUT_DIR/.compare-in-48k-mono.wav"
        echo "[compare] resampling $IN -> 48 kHz mono 16-bit PCM at $BENCH_IN"
        ffmpeg -nostdin -y -loglevel error -i "$IN" -ac 1 -ar 48000 -sample_fmt s16 "$BENCH_IN"
    else
        echo "[compare] ffmpeg not on PATH; passing $IN through as-is (bench will error if not 48 kHz mono)"
    fi

fi

if [[ "$BENCH_IN" == "synth" ]]; then
    echo "[compare] params: source=synth duration=${DURATION}s loss=${LOSS}% jitter=${JIT}ms seed=${SEED}"
    BENCH_DURATION_ARG=(--duration "$DURATION")
else
    echo "[compare] params: source=$IN loss=${LOSS}% jitter=${JIT}ms seed=${SEED}"
    BENCH_DURATION_ARG=()
fi
echo

"$BENCH" --in "$BENCH_IN" "${BENCH_DURATION_ARG[@]}" --loss "$LOSS" --jitter-ms "$JIT" --seed "$SEED" --jb-mode fixed    --out "$FIXED_WAV" | tail -1
"$BENCH" --in "$BENCH_IN" "${BENCH_DURATION_ARG[@]}" --loss "$LOSS" --jitter-ms "$JIT" --seed "$SEED" --jb-mode adaptive --out "$ADAPT_WAV" | tail -1

echo
echo "[compare] WAVs written:"
echo "  fixed    -> $FIXED_WAV"
echo "  adaptive -> $ADAPT_WAV"
echo

if grep -qi microsoft /proc/version 2>/dev/null; then
    echo "[compare] WSL detected — opening Explorer to both files..."
    explorer.exe "$(wslpath -w "$FIXED_WAV")" || true
    explorer.exe "$(wslpath -w "$ADAPT_WAV")" || true
else
    echo "[compare] play with:  aplay $FIXED_WAV  /  aplay $ADAPT_WAV"
fi

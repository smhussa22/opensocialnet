#!/usr/bin/env bash
# Quick sweep: vary --jitter-ms across fixed vs adaptive at fixed --loss.
# Run from bench/ (or pass BENCH=path/to/bench). Outputs one table row per
# (jitter, mode) combo.
#
# Input: defaults to synthesized signal. Set IN=/path/to/file.wav to drive
# the sweep from a real audio file; the file is auto-resampled to 48 kHz
# mono via ffmpeg into /tmp before the sweep starts.

set -euo pipefail

BENCH="${BENCH:-./build/bench}"
DURATION="${DURATION:-8}"
LOSS="${LOSS:-2}"
IN="${IN:-synth}"

BENCH_IN="$IN"
BENCH_DURATION_ARG=(--duration "$DURATION")

if [[ "$IN" != "synth" ]]; then

    if [[ ! -f "$IN" ]]; then echo "[sweep] input not found: $IN" >&2; exit 1; fi
    if command -v ffmpeg >/dev/null 2>&1; then
        BENCH_IN="/tmp/.sweep-in-48k-mono.wav"
        ffmpeg -nostdin -y -loglevel error -i "$IN" -ac 1 -ar 48000 -sample_fmt s16 "$BENCH_IN"
    fi
    BENCH_DURATION_ARG=()   # real file dictates its own length

fi

printf "  source=%s loss=%s%% duration=%ss\n" "${IN}" "$LOSS" "$DURATION"
printf "  %-12s %-9s %-12s %-12s %-15s %-9s %s\n" "jitter_ms" "mode" "underruns" "final_th" "mean_depth" "adapts" "meas_jitter_ms"

for jit in 0 10 30 60 100; do
  for mode in fixed adaptive; do
    out=$("$BENCH" --in "$BENCH_IN" "${BENCH_DURATION_ARG[@]}" --loss "$LOSS" --jitter-ms "$jit" --jb-mode "$mode" --out /tmp/sweep.wav 2>&1 | tail -1)
    ur=$(echo  "$out" | grep -oP "pops_underrun=\K\d+")
    th=$(echo  "$out" | grep -oP "jb_final_th=\K\d+")
    md=$(echo  "$out" | grep -oP "jb_mean_depth=\K[\d.]+")
    ad=$(echo  "$out" | grep -oP "jb_adapts=\K\d+")
    jm=$(echo  "$out" | grep -oP "jb_final_jitter_ms=\K[\d.]+")
    printf "  %-12s %-9s %-12s %-12s %-15s %-9s %s\n" "$jit" "$mode" "$ur" "$th" "$md" "$ad" "$jm"
  done
done

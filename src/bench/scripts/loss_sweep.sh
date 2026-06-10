#!/usr/bin/env bash
# Quick sweep: vary --loss across fixed vs adaptive at fixed --jitter-ms.
# Run from bench/ (or pass BENCH=path/to/bench). One row per (loss, mode).
#
# Input: defaults to synthesized signal. Set IN=/path/to/file.wav to drive
# the sweep from a real audio file; the file is auto-resampled to 48 kHz
# mono via ffmpeg into /tmp before the sweep starts.

set -euo pipefail

BENCH="${BENCH:-./build/bench}"
DURATION="${DURATION:-8}"
JIT="${JIT:-30}"
IN="${IN:-synth}"

BENCH_IN="$IN"
BENCH_DURATION_ARG=(--duration "$DURATION")

if [[ "$IN" != "synth" ]]; then

    if [[ ! -f "$IN" ]]; then echo "[loss_sweep] input not found: $IN" >&2; exit 1; fi
    if command -v ffmpeg >/dev/null 2>&1; then
        BENCH_IN="/tmp/.loss-sweep-in-48k-mono.wav"
        ffmpeg -nostdin -y -loglevel error -i "$IN" -ac 1 -ar 48000 -sample_fmt s16 "$BENCH_IN"
    fi
    BENCH_DURATION_ARG=()

fi

printf "  source=%s jitter=%sms duration=%ss\n" "$IN" "$JIT" "$DURATION"
printf "  %-9s %-9s %-12s %-12s %-15s %-9s %s\n" "loss%" "mode" "underruns" "final_th" "mean_depth" "adapts" "rmse"

for loss in 0 1 5 10 15; do
  for mode in fixed adaptive; do
    out=$("$BENCH" --in "$BENCH_IN" "${BENCH_DURATION_ARG[@]}" --loss "$loss" --jitter-ms "$JIT" --jb-mode "$mode" --out /tmp/lsw.wav 2>&1 | tail -1)
    ur=$(echo "$out" | grep -oP "pops_underrun=\K\d+")
    th=$(echo "$out" | grep -oP "jb_final_th=\K\d+")
    md=$(echo "$out" | grep -oP "jb_mean_depth=\K[\d.]+")
    ad=$(echo "$out" | grep -oP "jb_adapts=\K\d+")
    rm=$(echo "$out" | grep -oP "rmse=\K[\d.]+")
    printf "  %-9s %-9s %-12s %-12s %-15s %-9s %s\n" "$loss" "$mode" "$ur" "$th" "$md" "$ad" "$rm"
  done
done

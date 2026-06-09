#!/usr/bin/env bash
# Quick sweep: vary --loss across fixed vs adaptive at fixed --jitter-ms.
# Run from bench/ (or pass BENCH=path/to/bench). One row per (loss, mode).

set -euo pipefail

BENCH="${BENCH:-./build/bench}"
DURATION="${DURATION:-8}"
JIT="${JIT:-30}"

printf "  jitter=%sms duration=%ss\n" "$JIT" "$DURATION"
printf "  %-9s %-9s %-12s %-12s %-15s %-9s %s\n" "loss%" "mode" "underruns" "final_th" "mean_depth" "adapts" "rmse"

for loss in 0 1 5 10 15; do
  for mode in fixed adaptive; do
    out=$("$BENCH" --duration "$DURATION" --loss "$loss" --jitter-ms "$JIT" --jb-mode "$mode" --out /tmp/lsw.wav 2>&1 | tail -1)
    ur=$(echo "$out" | grep -oP "pops_underrun=\K\d+")
    th=$(echo "$out" | grep -oP "jb_final_th=\K\d+")
    md=$(echo "$out" | grep -oP "jb_mean_depth=\K[\d.]+")
    ad=$(echo "$out" | grep -oP "jb_adapts=\K\d+")
    rm=$(echo "$out" | grep -oP "rmse=\K[\d.]+")
    printf "  %-9s %-9s %-12s %-12s %-15s %-9s %s\n" "$loss" "$mode" "$ur" "$th" "$md" "$ad" "$rm"
  done
done

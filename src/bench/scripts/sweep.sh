#!/usr/bin/env bash
# Quick sweep: vary --jitter-ms across fixed vs adaptive at fixed --loss.
# Run from bench/ (or pass BENCH=path/to/bench). Outputs one table row per
# (jitter, mode) combo.

set -euo pipefail

BENCH="${BENCH:-./build/bench}"
DURATION="${DURATION:-8}"
LOSS="${LOSS:-2}"

printf "  loss=%s%% duration=%ss\n" "$LOSS" "$DURATION"
printf "  %-12s %-9s %-12s %-12s %-15s %-9s %s\n" "jitter_ms" "mode" "underruns" "final_th" "mean_depth" "adapts" "meas_jitter_ms"

for jit in 0 10 30 60 100; do
  for mode in fixed adaptive; do
    out=$("$BENCH" --duration "$DURATION" --loss "$LOSS" --jitter-ms "$jit" --jb-mode "$mode" --out /tmp/sweep.wav 2>&1 | tail -1)
    ur=$(echo  "$out" | grep -oP "pops_underrun=\K\d+")
    th=$(echo  "$out" | grep -oP "jb_final_th=\K\d+")
    md=$(echo  "$out" | grep -oP "jb_mean_depth=\K[\d.]+")
    ad=$(echo  "$out" | grep -oP "jb_adapts=\K\d+")
    jm=$(echo  "$out" | grep -oP "jb_final_jitter_ms=\K[\d.]+")
    printf "  %-12s %-9s %-12s %-12s %-15s %-9s %s\n" "$jit" "$mode" "$ur" "$th" "$md" "$ad" "$jm"
  done
done

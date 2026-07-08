#!/usr/bin/env bash
set -euo pipefail

# nproc is Linux-only; macOS spells it sysctl -n hw.ncpu
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

cd "$(dirname "$0")"
cmake -S . -B build
cmake --build build --parallel "$JOBS"

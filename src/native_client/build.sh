#!/usr/bin/env bash
set -euo pipefail

# nproc is Linux-only; macOS spells it sysctl -n hw.ncpu
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

# The GUI forks src/network/build/network for all gateway/voice/video I/O —
# without it the app signs in but never connects. Build it alongside so a
# bare native_client build always yields a runnable pair.
(cd "$(dirname "$0")/../network" && cmake -S . -B build && cmake --build build --parallel "$JOBS")

cd "$(dirname "$0")"
cmake -S . -B build
cmake --build build --parallel "$JOBS"

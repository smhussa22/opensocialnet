#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cmake -S "$DIR" -B "$DIR/build" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$DIR/build" --parallel "$(nproc)"

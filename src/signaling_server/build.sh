#!/usr/bin/env bash
set -euo pipefail

cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/home/faraz/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build --parallel "$(nproc)"

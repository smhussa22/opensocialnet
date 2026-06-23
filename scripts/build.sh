#!/usr/bin/env bash
# Build every binary needed for the end-to-end OAuth test.
#
# Order matters only because signaling_server uses vcpkg and pulls a lot
# of toolchain time on first build — leaving it last lets the smaller
# builds fail fast if they're going to fail.
#
# Skip stages with NO_NATIVE=1 / NO_NETWORK=1 / NO_RELAY=1 / NO_GATEWAY=1
# when you're iterating on just one component.

set -euo pipefail

cd "$(dirname "$0")/.."

if [[ -z "${NO_NATIVE:-}" ]]
then

    echo "=== build native_client ==="
    (cd src/native_client && ./build.sh)

fi

if [[ -z "${NO_NETWORK:-}" ]]
then

    echo "=== build network ==="
    (cd src/network && ./build.sh)

fi

if [[ -z "${NO_RELAY:-}" ]]
then

    echo "=== build relay ==="
    (cd src/relay && ./build.sh)

fi

if [[ -z "${NO_GATEWAY:-}" ]]
then

    echo "=== build signaling_server (vcpkg, slow first time) ==="
    (cd src/signaling_server && ./build.sh)

fi

echo
echo "=== all builds done ==="

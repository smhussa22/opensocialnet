#!/usr/bin/env bash
# Launch the native client with Google OAuth env loaded.
#
# Sources .secrets/google_oauth.env (gitignored) so OSN_GOOGLE_CLIENT_ID
# is set, then exec's the binary. With OSN_GOOGLE_CLIENT_ID set the
# client runs the Google sign-in flow at startup; with it unset it
# falls back to the HMAC dev token path used by the test scripts.
#
# Usage:
#   ./scripts/run-native-client.sh            # against EC2 gateway (default)
#   OSN_SIGNALING_HOST=127.0.0.1 ./scripts/run-native-client.sh
#   OSN_ROOM=general OSN_USER=alice ./scripts/run-native-client.sh

set -euo pipefail

cd "$(dirname "$0")/.."

if [[ -f .secrets/google_oauth.env ]]
then

    # set -a auto-exports every var defined in the dotfile
    set -a
    # shellcheck disable=SC1091
    . .secrets/google_oauth.env
    set +a
    echo "[run] loaded Google OAuth credentials from .secrets/google_oauth.env"

else

    echo "[run] WARN: .secrets/google_oauth.env missing — falling back to HMAC dev auth"

fi

# Build if needed.
if [[ ! -x src/native_client/build/native_client ]]
then

    echo "[run] native_client binary missing, building..."
    (cd src/native_client && cmake -S . -B build >/dev/null && cmake --build build -j)

fi
if [[ ! -x src/network/build/network ]]
then

    echo "[run] network binary missing, building..."
    (cd src/network && cmake --build build -j)

fi

exec ./src/native_client/build/native_client

#!/usr/bin/env bash
# Wrapper around scripts/deploy-ec2.sh that auto-loads the OAuth
# secrets and applies sane defaults for the EC2 connection params,
# so the user-facing command is just:  ./scripts/deploy.sh
#
# Overrides: EC2_HOST=... EC2_KEY=... OPENSOCIALNET_AUTH_SECRET=...
# ./scripts/deploy.sh

set -euo pipefail

cd "$(dirname "$0")/.."

# Load Google OAuth env (OSN_GOOGLE_CLIENT_ID) — needed so the gateway
# can verify Google ID tokens. Optional: gateway falls back to HMAC if
# absent, but then OAuth sign-ins won't work.
if [[ -f .secrets/google_oauth.env ]]
then

    set -a
    # shellcheck disable=SC1091
    . .secrets/google_oauth.env
    set +a
    echo "[deploy] loaded Google OAuth credentials"

else

    echo "[deploy] WARN: .secrets/google_oauth.env missing — gateway will only accept HMAC dev tokens"

fi

# Sane defaults — the user can override any of these in the env. The
# host comes from the same value baked into the codebase (network/main.cc
# default OSN_SIGNALING_HOST), so dev defaults stay aligned across the
# stack.
: "${EC2_HOST:=ubuntu@3.144.229.204}"
: "${EC2_KEY:=$HOME/udptcp/opensocialnet-ec2-keypair.pem}"
: "${OPENSOCIALNET_AUTH_SECRET:=devsecret123}"

export EC2_HOST EC2_KEY OPENSOCIALNET_AUTH_SECRET

echo "[deploy] EC2_HOST=$EC2_HOST  EC2_KEY=$EC2_KEY"
exec ./scripts/deploy-ec2.sh

#!/usr/bin/env bash
# Print HMAC-SHA256(OPENSOCIALNET_AUTH_SECRET, user_id) as lowercase hex.
# Matches the gateway's on_hello verification so a client can use the
# output as the auth_token field of a Hello envelope.
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <user_id>" >&2
  exit 2
fi

if [[ -z "${OPENSOCIALNET_AUTH_SECRET:-}" ]]; then
  echo "error: OPENSOCIALNET_AUTH_SECRET is unset or empty" >&2
  exit 2
fi

user_id="$1"
# `openssl dgst` prints "(stdin)= <hex>"; strip the prefix so the
# output is just the 64-char digest the gateway compares against.
printf '%s' "$user_id" | openssl dgst -sha256 -hmac "$OPENSOCIALNET_AUTH_SECRET" | awk '{print $NF}'

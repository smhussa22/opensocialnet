#!/usr/bin/env bash
# Prints HMAC-SHA256(OPENSOCIALNET_AUTH_SECRET, user_id) hex for a handful
# of demo user ids so you can paste them into the client UI.
set -euo pipefail

secret="${OPENSOCIALNET_AUTH_SECRET:-devsecret}"
echo "secret: $secret"
echo
for u in alice bob faraz maroof test; do
  tok=$(printf '%s' "$u" | openssl dgst -sha256 -hmac "$secret" -hex | awk '{print $NF}')
  printf '%-10s %s\n' "$u" "$tok"
done

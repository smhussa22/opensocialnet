#!/usr/bin/env bash
# Post-deploy / post-test sanity check. SSHes to the EC2 host and:
#   1. Shows the last few lines of the signaling_server log so you can
#      eyeball "[auth] Google client_id configured" + any JWT outcomes.
#   2. Lists rows in opensocialnet.users so you can confirm your sign-in
#      actually wrote a row.
#   3. Lists rows in channel_members for `general` so you can confirm
#      the auto-join also fired.
#
# Defaults match deploy.sh — override EC2_HOST / EC2_KEY in the env.

set -euo pipefail

cd "$(dirname "$0")/.."

: "${EC2_HOST:=ubuntu@3.144.229.204}"
: "${EC2_KEY:=$HOME/udptcp/opensocialnet-ec2-keypair.pem}"

SSH=(ssh -o StrictHostKeyChecking=no -i "$EC2_KEY" "$EC2_HOST")

echo "=== signaling_server (last 30 lines) ==="
"${SSH[@]}" 'docker compose -f ~/compose.prod.yml logs signaling_server --tail 30 2>&1 | sed "s/^/  /"'

echo
echo "=== opensocialnet.users ==="
"${SSH[@]}" \
  "docker exec -i \$(docker ps --filter name=scylla -q | head -1) cqlsh -e \
   \"SELECT user_id, username, created_at FROM opensocialnet.users;\" 2>&1 | sed 's/^/  /'"

echo
echo "=== opensocialnet.channel_members WHERE channel_id='general' ==="
"${SSH[@]}" \
  "docker exec -i \$(docker ps --filter name=scylla -q | head -1) cqlsh -e \
   \"SELECT * FROM opensocialnet.channel_members WHERE channel_id='general';\" 2>&1 | sed 's/^/  /'"

echo
echo "=== verify done ==="

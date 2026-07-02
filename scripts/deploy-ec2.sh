#!/usr/bin/env bash
# One-shot deploy: build images on this machine, save them to a tarball,
# ship them to a fresh EC2 host, load + bring the stack up.
#
# Usage:
#   OPENSOCIALNET_AUTH_SECRET=devsecret123 \
#     EC2_HOST=ubuntu@<public-ip> \
#     EC2_KEY=~/udptcp/opensocialnet-ec2-keypair.pem \
#     ./scripts/deploy-ec2.sh
#
# Assumes the target EC2 has:
# - Docker + compose plugin installed
# - This script's user can `sudo systemctl restart docker` (for the
#   docker-compose installation step; we don't need it in steady state)
# - Security group open on 22/tcp + 9001/tcp + 50100/udp (relay media port)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

: "${EC2_HOST:?EC2_HOST=ubuntu@<public-ip>}"
: "${EC2_KEY:?EC2_KEY=path/to/key.pem}"
: "${OPENSOCIALNET_AUTH_SECRET:?set OPENSOCIALNET_AUTH_SECRET (hmac secret used to verify Hello frames)}"

# OAuth client_id — optional. Source .secrets/google_oauth.env before
# running to enable the Google sign-in path on the gateway. Empty value
# leaves only HMAC dev auth working.
: "${OSN_GOOGLE_CLIENT_ID:=}"

# Relay endpoint that signaling_server stamps into VoicePeerJoined replies.
# Defaults to the EC2 public IP parsed out of EC2_HOST (ubuntu@1.2.3.4).
# Exported because the local `docker compose build` interpolates
# compose.prod.yml too, not just the remote up/ps/restart calls.
export OSN_RELAY_HOST="${OSN_RELAY_HOST:-${EC2_HOST#*@}}"
echo "relay endpoint clients will be told to dial: $OSN_RELAY_HOST:50100"

# compose.prod.yml interpolates these on EVERY docker compose call (up /
# ps / restart / logs), so prefix all of them with the same env block.
COMPOSE_ENV="OPENSOCIALNET_AUTH_SECRET='$OPENSOCIALNET_AUTH_SECRET' OSN_RELAY_HOST='$OSN_RELAY_HOST' OSN_GOOGLE_CLIENT_ID='$OSN_GOOGLE_CLIENT_ID'"

SSH=(ssh -o StrictHostKeyChecking=no -i "$EC2_KEY" "$EC2_HOST")
SCP=(scp -o StrictHostKeyChecking=no -i "$EC2_KEY")

IMAGES_TAR=/tmp/opensocialnet-images.tar.gz
COMPOSE_FILE=compose.prod.yml

echo "=== 1/6  build signaling_server + relay images locally ==="
docker compose -f "$COMPOSE_FILE" build signaling_server relay

echo
echo "=== 2/6  save images to a tarball (and pull cp-zookeeper / cp-kafka / scylla so the EC2 doesn't have to fetch them) ==="
docker pull confluentinc/cp-zookeeper:7.6.0
docker pull confluentinc/cp-kafka:7.6.0
docker pull scylladb/scylla:5.4
docker save \
  opensocialnet-signaling \
  opensocialnet-relay \
  confluentinc/cp-zookeeper:7.6.0 \
  confluentinc/cp-kafka:7.6.0 \
  scylladb/scylla:5.4 \
  | gzip > "$IMAGES_TAR"
ls -lh "$IMAGES_TAR"

echo
echo "=== 3/6  ship tarball + compose + schema to EC2 ==="
"${SCP[@]}" "$IMAGES_TAR" "$EC2_HOST:~/images.tar.gz"
"${SCP[@]}" "$COMPOSE_FILE" "$EC2_HOST:~/compose.prod.yml"
"${SCP[@]}" src/signaling_server/schema/schema.cql "$EC2_HOST:~/schema.cql"
# compose.prod.yml bind-mounts ./scripts/scylla-supervisord.conf into the
# scylla container — make the relative path resolve on EC2 too.
"${SSH[@]}" 'mkdir -p ~/scripts'
"${SCP[@]}" scripts/scylla-supervisord.conf "$EC2_HOST:~/scripts/scylla-supervisord.conf"

echo
echo "=== 4/6  load images on EC2 ==="
"${SSH[@]}" 'gunzip -c ~/images.tar.gz | docker load'

echo
echo "=== 5/6  bring stack up on EC2 ==="
"${SSH[@]}" "$COMPOSE_ENV docker compose -f ~/compose.prod.yml up -d"
echo "waiting 25s for scylla + kafka to settle..."
"${SSH[@]}" "sleep 25 && $COMPOSE_ENV docker compose -f ~/compose.prod.yml ps"

echo
echo "=== 6/6  apply schema + restart signaling_server so it picks it up ==="
"${SSH[@]}" 'docker exec -i $(docker ps --filter name=scylla -q | head -1) cqlsh < ~/schema.cql 2>&1 | tail -3 || true'
# CREATE TABLE IF NOT EXISTS never adds new columns to a table that already
# exists from an older deploy, so column drift is applied as individual
# ALTERs — each errors harmlessly once the column exists.
"${SSH[@]}" 'docker exec -i $(docker ps --filter name=scylla -q | head -1) cqlsh -e "ALTER TABLE opensocialnet.users ADD email text" >/dev/null 2>&1 || true'
"${SSH[@]}" "$COMPOSE_ENV docker compose -f ~/compose.prod.yml restart signaling_server"
sleep 6
"${SSH[@]}" "$COMPOSE_ENV bash -c 'docker compose -f ~/compose.prod.yml ps && echo === signaling_server logs === && docker compose -f ~/compose.prod.yml logs signaling_server --tail 20 && echo === relay logs === && docker compose -f ~/compose.prod.yml logs relay --tail 10'"

echo
echo "=== deploy done ==="

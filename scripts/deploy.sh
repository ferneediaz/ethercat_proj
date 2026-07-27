#!/usr/bin/env bash
# Deploy the master app to the Pi and build it there.
# Usage: PI_USER=pi PI_HOST=raspberrypi.local scripts/deploy.sh
set -euo pipefail

PI_USER="${PI_USER:-pi}"
PI_HOST="${PI_HOST:-raspberrypi.local}"
DEST="$PI_USER@$PI_HOST"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "== Syncing master/ and scripts/ to $DEST:~/ethercat/ =="
rsync -av --delete --exclude build "$REPO_DIR/master/" "$DEST:~/ethercat/master/"
rsync -av "$REPO_DIR/scripts/" "$DEST:~/ethercat/scripts/"

echo "== Building on the Pi =="
ssh "$DEST" 'cmake -S ~/ethercat/master -B ~/ethercat/master/build && cmake --build ~/ethercat/master/build -j$(nproc)'

echo "== Running unit tests on the Pi =="
ssh "$DEST" '~/ethercat/master/build/test_angle'

echo
echo "Done. Run the master with:"
echo "  ssh $DEST 'sudo ~/ethercat/master/build/servo_master eth0 scan'"

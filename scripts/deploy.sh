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

# The host test for the degrees<->microsteps arithmetic includes the firmware
# header that owns it, so the Pi needs that one file even though it never
# builds firmware. Syncing the header rather than duplicating the maths is the
# whole point: a copy could drift from what the ESP32 actually runs, and the
# test would then be checking the wrong thing while passing.
ssh "$DEST" 'mkdir -p ~/ethercat/firmware/src'
rsync -av "$REPO_DIR/firmware/src/step_math.h" "$DEST:~/ethercat/firmware/src/"

echo "== Building on the Pi =="
ssh "$DEST" 'cmake -S ~/ethercat/master -B ~/ethercat/master/build && cmake --build ~/ethercat/master/build -j$(nproc)'

echo "== Running unit tests on the Pi =="
ssh "$DEST" '~/ethercat/master/build/test_angle && ~/ethercat/master/build/test_step_math'

echo
echo "Done. Run the master with:"
echo "  ssh $DEST 'sudo ~/ethercat/master/build/servo_master eth0 scan'"

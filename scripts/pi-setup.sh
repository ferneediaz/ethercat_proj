#!/usr/bin/env bash
# One-time (idempotent) Raspberry Pi provisioning for the EtherCAT master.
# Run ON the Pi:  bash pi-setup.sh
set -euo pipefail

SOEM_TAG="v1.4.0"
BASE="$HOME/ethercat"
DONE_MARKER="$BASE/.setup-complete"

mkdir -p "$BASE"
rm -f "$DONE_MARKER"

# Wait up to 10 min for any other apt/unattended-upgrade to release the
# lock rather than failing outright — a fresh Pi often runs one on boot.
APT_WAIT="-o DPkg::Lock::Timeout=600"

echo "== Installing build tools =="
sudo apt-get $APT_WAIT update
sudo apt-get $APT_WAIT install -y git cmake build-essential

echo "== Cloning SOEM ($SOEM_TAG) =="
if [ ! -d "$BASE/SOEM/.git" ]; then
  git clone --branch "$SOEM_TAG" --depth 1 \
    https://github.com/OpenEtherCATsociety/SOEM.git "$BASE/SOEM"
else
  echo "SOEM already cloned."
fi
git -C "$BASE/SOEM" rev-parse HEAD > "$BASE/SOEM_COMMIT.txt"
echo "SOEM commit: $(cat "$BASE/SOEM_COMMIT.txt")"

echo "== Building SOEM (includes slaveinfo, simple_test, eepromtool) =="
mkdir -p "$BASE/SOEM/build"
cmake -S "$BASE/SOEM" -B "$BASE/SOEM/build"
cmake --build "$BASE/SOEM/build" -j"$(nproc)"

echo "== Network interfaces =="
ip -brief link

touch "$DONE_MARKER"

echo
echo "SETUP_COMPLETE"
echo "Done. Sanity check with:"
echo "  sudo $BASE/SOEM/build/test/linux/slaveinfo/slaveinfo eth0"
echo "(0 slaves found is the expected result until hardware arrives.)"

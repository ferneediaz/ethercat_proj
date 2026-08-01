#!/usr/bin/env bash
# Build / flash / monitor the ESP32-S3 host firmware.
#
#   ./scripts/fw.sh build            # milestone 4a, no motor
#   ./scripts/fw.sh build sg90       # with the servo overlay
#   ./scripts/fw.sh build nema17     # with the stepper overlay
#   ./scripts/fw.sh flash
#   ./scripts/fw.sh monitor
#
# Runs on the Mac — that is where the USB-C port is. The Pi keeps
# running the EtherCAT master over SSH, unchanged.
set -euo pipefail

ZEPHYR_WS="${ZEPHYR_WS:-$HOME/zephyrproject}"
BOARD="${BOARD:-esp32s3_devkitc/esp32s3/procpu}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WEST="$ZEPHYR_WS/.venv/bin/west"

if [ ! -x "$WEST" ]; then
	echo "west not found at $WEST" >&2
	echo "Set ZEPHYR_WS to your Zephyr workspace, or re-run the setup." >&2
	exit 1
fi

export ZEPHYR_BASE="$ZEPHYR_WS/zephyr"

# The Espressif SoC CMake looks for esptool on PATH, not in the venv, so
# calling west by absolute path alone is not enough — without this the
# build fails at configure time with "esptool>=5.0.2 not found in PATH".
export PATH="$ZEPHYR_WS/.venv/bin:$PATH"

cmd="${1:-build}"
actuator="${2:-none}"

case "$actuator" in
none)   overlay="" ;;
sg90)   overlay="overlays/sg90.overlay" ;;
nema17) overlay="overlays/nema17.overlay" ;;
*)
	echo "Unknown actuator '$actuator' (want: none, sg90, nema17)" >&2
	exit 1
	;;
esac

case "$cmd" in
diag)
	# Diagnostic build: tests the NSS wire via the ESC's reaction, then
	# leaves a live monitor of every SPI line running.
	echo "Building link diagnostics for $BOARD"
	"$WEST" build -b "$BOARD" "$REPO/firmware" -p always \
		-- -DCONFIG_ESC_DIAG=y
	"$WEST" flash
	;;
build)
	# -p always: switching overlays without a pristine build silently
	# reuses the previous devicetree, which looks like the overlay had
	# no effect and wastes a lot of time.
	args=(build -b "$BOARD" "$REPO/firmware" -p always)
	if [ -n "$overlay" ]; then
		args+=(-- "-DEXTRA_DTC_OVERLAY_FILE=$overlay")
	fi
	echo "Building for $BOARD (actuator: $actuator)"
	"$WEST" "${args[@]}"
	;;
flash)
	"$WEST" flash
	;;
monitor)
	"$WEST" espressif monitor
	;;
*)
	echo "Usage: $0 {build|flash|monitor|diag} [none|sg90|nema17]" >&2
	exit 1
	;;
esac

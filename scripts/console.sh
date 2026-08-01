#!/usr/bin/env bash
# Read the ESP32-S3 console.
#
#   ./scripts/console.sh          # attach to a running board, no reset
#   ./scripts/console.sh 20       # ... for 20 seconds (default 10)
#   ./scripts/console.sh 20 reset # force a reboot first, to catch boot logs
#
# Attaching does NOT reset by default. The T-Display-S3 speaks over the
# SoC's native USB-Serial-JTAG, where RTS is wired to the reset line, so
# toggling it reboots the board — which silently restarts whatever was
# being observed and looks exactly like a crash.
set -euo pipefail

SECS="${1:-10}"
MODE="${2:-attach}"
ZEPHYR_WS="${ZEPHYR_WS:-$HOME/zephyrproject}"

exec "$ZEPHYR_WS/.venv/bin/python" - "$SECS" "$MODE" <<'PY'
import glob
import sys
import time

import serial

secs = float(sys.argv[1])
mode = sys.argv[2]

ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("No /dev/cu.usbmodem* found — is the board plugged in?")

port = ports[0]
s = serial.Serial(port, 115200, timeout=1)

if mode == "reset":
    s.dtr = False
    s.rts = False
    time.sleep(0.2)
    s.rts = True
    time.sleep(0.15)
    s.rts = False

end = time.time() + secs
while time.time() < end:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
PY

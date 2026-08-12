#!/usr/bin/env bash
#
# Write our ESI to the ESC's SII EEPROM, from the Pi.
#
#     ./scripts/write-eeprom.sh eth0            # back up, build, show, ask
#     ./scripts/write-eeprom.sh eth0 --yes      # same without the prompt
#     ./scripts/write-eeprom.sh eth0 --restore backups/sii-<stamp>.bin
#
# The EEPROM is ordinary electrically-erasable memory and rewriting it is the
# normal way an EtherCAT slave is commissioned -- this is not a risky
# operation in general. It has exactly one sharp edge, and the whole shape of
# this script is built around it:
#
#   Words 0..7 configure the PDI, the physical interface the ESC exposes. On
#   this board that is the SPI slave port the ESP32-S3 is wired to. Corrupt
#   those words and the ESC stops answering over SPI, which is the route you
#   would use to fix them. Recovery then needs an external programmer clipped
#   to the EEPROM.
#
# So: back up first, always; copy the config words out of that backup rather
# than generating them; read back and compare after writing. The backup is
# taken before anything else happens and the script refuses to continue if it
# cannot produce one.
#
# Order matters for what comes after. Write the EEPROM, power-cycle, confirm
# the new identity with `servo_master <iface> scan`, and only then rebuild the
# firmware with CONFIG_ESC_SII_REWRITTEN=y and the master with
# -DSERVO_SII_REWRITTEN=ON. Rebuilding first leaves a slave whose object
# dictionary is smaller than the SyncManager the master programmed for it, and
# the bus stops at PREOP.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IFACE="${1:-}"
shift || true

ASSUME_YES=0
RESTORE=""
while [ $# -gt 0 ]; do
	case "$1" in
		--yes|-y) ASSUME_YES=1 ;;
		--restore) RESTORE="${2:-}"; shift ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done

if [ -z "$IFACE" ]; then
	echo "usage: $0 <iface> [--yes] [--restore FILE]" >&2
	echo "  e.g. $0 eth0" >&2
	exit 2
fi

# eepromtool comes from the SOEM build that scripts/pi-setup.sh makes.
EEPROMTOOL="$(command -v eepromtool || true)"
for cand in "$HOME/ethercat/SOEM/build/test/linux/eepromtool/eepromtool" \
            "$REPO/master/build/_deps/soem-build/test/linux/eepromtool/eepromtool"; do
	[ -n "$EEPROMTOOL" ] && break
	[ -x "$cand" ] && EEPROMTOOL="$cand"
done
if [ -z "$EEPROMTOOL" ]; then
	echo "eepromtool not found. It is built as part of SOEM:" >&2
	echo "  ./scripts/pi-setup.sh   (or build SOEM's test/linux/eepromtool)" >&2
	exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
	echo "raw socket access needs root: re-run with sudo" >&2
	exit 1
fi

BACKUP_DIR="$REPO/backups"
mkdir -p "$BACKUP_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="$BACKUP_DIR/sii-$STAMP.bin"

echo "==> reading the current EEPROM to $BACKUP"
"$EEPROMTOOL" "$IFACE" 1 -r "$BACKUP"
if [ ! -s "$BACKUP" ]; then
	echo "backup is empty -- refusing to write anything." >&2
	echo "Check the slave is powered and enumerating: $EEPROMTOOL $IFACE 1 -i" >&2
	exit 1
fi
echo "    backed up $(wc -c < "$BACKUP") bytes"
echo

if [ -n "$RESTORE" ]; then
	# Putting a previous image back. Nothing is generated, so there is
	# nothing to preview -- the file is whatever it was when it was read.
	[ -f "$RESTORE" ] || { echo "no such file: $RESTORE" >&2; exit 1; }
	echo "==> restoring $RESTORE"
	IMAGE="$RESTORE"
else
	IMAGE="$REPO/build/sii.bin"
	mkdir -p "$(dirname "$IMAGE")"
	echo "==> building the image from esi/EthercatServoNode.xml"
	"$REPO/scripts/esi_tool.py" sii --preserve-config "$BACKUP" -o "$IMAGE"
	echo
	echo "==> what is about to be written"
	"$REPO/scripts/esi_tool.py" dump "$IMAGE" | sed 's/^/    /'
fi
echo

if [ "$ASSUME_YES" -ne 1 ]; then
	echo "Backup is at $BACKUP -- keep it. Restore with:"
	echo "    sudo $0 $IFACE --restore $BACKUP"
	echo
	read -r -p "Write this image to slave 1 on $IFACE? [y/N] " reply
	case "$reply" in
		y|Y|yes|YES) ;;
		*) echo "aborted; nothing written."; exit 0 ;;
	esac
fi

echo "==> writing"
"$EEPROMTOOL" "$IFACE" 1 -w "$IMAGE"

echo "==> reading back to verify"
READBACK="$BACKUP_DIR/sii-$STAMP-readback.bin"
"$EEPROMTOOL" "$IFACE" 1 -r "$READBACK"

# Compare only the part we control. Some ESCs return trailing 0xff for unused
# space regardless of what was written, so a whole-file diff produces false
# alarms; the categories and the identity are what matter.
if cmp -s "$IMAGE" "$READBACK"; then
	echo "    exact match"
else
	echo "    whole-image compare differs; checking the parts that matter"
	a="$("$REPO/scripts/esi_tool.py" dump "$IMAGE")"
	b="$("$REPO/scripts/esi_tool.py" dump "$READBACK")"
	if [ "$a" = "$b" ]; then
		echo "    identity, SyncManagers and PDOs all match"
	else
		echo "MISMATCH after write. The device may be in a bad state." >&2
		diff <(echo "$a") <(echo "$b") >&2 || true
		echo >&2
		echo "Restore the backup before power-cycling:" >&2
		echo "    sudo $0 $IFACE --restore $BACKUP" >&2
		exit 1
	fi
fi

if [ -n "$RESTORE" ]; then
	# A restore puts back whatever was there before, so the instructions for
	# a fresh write do not apply -- following them would set the firmware to
	# the four-byte layout against an EEPROM that may well declare six.
	cat <<EOF

Restored $RESTORE.

  1. Power-cycle the slave. The ESC loads the SII at reset; until then it is
     still running on the old contents.
  2. sudo servo_master $IFACE scan
     Expect whatever identity that image carries. If you have just rolled back
     to the stock image, that is vendor 0x00000009, product 0x26483052.
  3. Make sure both builds match what is now on the chip. For the stock image
     that means CONFIG_ESC_SII_REWRITTEN=n and no -DSERVO_SII_REWRITTEN, which
     are the defaults.

  Pre-restore backup: $BACKUP
EOF
else
	cat <<EOF

Done. Next, in this order:

  1. Power-cycle the slave. The ESC loads the SII at reset; until then it is
     still running on the old contents.
  2. sudo servo_master $IFACE scan
     Expect vendor 0x00000b95, product 0x00620300, revision 0x00010000.
  3. Rebuild both sides to match the new 4-byte input size:
       ./scripts/fw.sh build nema17            (with CONFIG_ESC_SII_REWRITTEN=y)
       cmake -DSERVO_SII_REWRITTEN=ON ...      (master)

  Until step 3 the slave declares 6 bytes of inputs and the EEPROM declares 4,
  so the master will refuse SAFEOP. That is expected between steps, not a
  fault.

  Backup: $BACKUP
EOF
fi

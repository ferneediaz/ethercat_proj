# SOES port

Runs [SOES](https://github.com/OpenEtherCATsociety/SOES) in place of the
hand-written slave in `../src/ecat_slave.c`. Both sit on the same `ax58100.c`
SPI layer and present identical process data — 2 bytes out, 6 bytes in — so
the master cannot tell which is running.

    west build -b esp32s3_devkitc/esp32s3/procpu firmware -p always \
        -d build-soes -- -DCONFIG_ESC_USE_SOES=y \
        -DEXTRA_DTC_OVERLAY_FILE=overlays/sg90.overlay

SOES is fetched at configure time rather than vendored, the same way
`master/CMakeLists.txt` pulls SOEM. Committing the tree would add ~127 files
of demos, drivers and ESI binaries for the three `.c` files actually
compiled. The pin is a commit hash, not a tag: that is the exact revision
this port was verified against.

**The EEPROM is never written.** `ecat_options.h` is hand-matched to what the
module's existing EEPROM already declares, which is the whole reason this
port was viable without an ESI flash. `esc_eep.c` is deliberately excluded
from the build.

## Files

| File | Purpose |
|---|---|
| _(fetched)_ | SOES itself, pulled by CMake, pinned by commit |
| `cc.h` | platform shim; SOES's own pulls in headers picolibc lacks |
| `ecat_options.h` | SyncManager layout, must match the EEPROM |
| `utypes.h` | process data storage |
| `slave_objectlist.c` | object dictionary |
| `esc_hw.c` | HAL: four functions onto `ax58100.c` |
| `soes_app.c` | actuator glue and the poll loop |

## Three things that cost time

**`EC_LITTLE_ENDIAN` must be defined.** SOES selects its packed register and
mailbox struct layouts on it. Without it the build fails with a long list of
`has no member named ECsm / mbxcnt / MBXstat`, which reads like a corrupt
checkout rather than a missing define.

**The HAL must populate `ESCvar.ALevent`.** Nothing in SOES assigns it —
every working HAL reads register `0x0220` into it, and `ESC_state()` only
reads AL Control when the control bit is set there. Miss it and SOES
initialises cleanly, reports INIT, and never transitions. It looks like a
dead stack; it is a HAL omission. Here it is refreshed whenever SOES reads
the local time register, which it does at the top of every cycle.

**`watchdog_cnt = 0` does not disable the watchdog.** The counter is simply
already expired, so the first OP request is refused with AL status code
`0x001b` (sync manager watchdog) — which looks like a SyncManager
configuration problem rather than a timing one. Use 150, as SOES's own demos
do.

## Status

Verified on hardware 2026-08-02: reaches OP, `wkc 3/3`, no dropped cycles,
angles from `servo_master eth0 sweep` arrive and drive the servo.

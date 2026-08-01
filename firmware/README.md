# firmware/ — ESP32-S3 host application (Zephyr)

The host MCU that sits behind the AX58100 and turns EtherCAT process
data into motor motion.

**Nothing here is flashed to the AX58100.** This all compiles into the
**ESP32-S3** binary. `src/esc.c` is the ESP32-side driver that *talks to*
the AX58100 over SPI at runtime — "ESC" is the standard abbreviation for
EtherCAT Slave Controller, which is the AX58100 chip. The AX58100 has no
user-programmable firmware at all; its EEPROM (the ESI) is a separate
thing that stays untouched until the SOES object dictionary is settled.

## Board

Built for the in-tree **`esp32s3_devkitc`** board and flashed to a
**LilyGO T-Display-S3**. Same SoC, and nothing in this application needs
the DevKitC's specific peripherals — this avoids writing a custom board
port. Consequence: **the 1.9" display does not work.** It is an 8-bit
i8080 parallel ST7789 and Zephyr's ST7789 driver is SPI-only.

`app.overlay` moves the console onto the SoC's built-in USB-Serial-JTAG.
This is required: `esp32s3_devkitc` defaults the console to UART0
(GPIO 43/44) because the DevKitC has a bridge chip there. The
T-Display-S3 does not — without the override there is no output at all.

## Build

```sh
source ~/zephyrproject/.venv/bin/activate
export ZEPHYR_BASE=~/zephyrproject/zephyr

# Milestone 4a — SPI link only, no motor
west build -b esp32s3_devkitc/esp32s3/procpu firmware

# With the SG90 (phase 5)
west build -b esp32s3_devkitc/esp32s3/procpu firmware -p \
    -- -DEXTRA_DTC_OVERLAY_FILE=overlays/sg90.overlay

# With the NEMA 17 (phase 6)
west build -b esp32s3_devkitc/esp32s3/procpu firmware -p \
    -- -DEXTRA_DTC_OVERLAY_FILE=overlays/nema17.overlay

west flash && west espressif monitor
```

`scripts/fw.sh` wraps these.

## Layout

| Path | What it is |
| --- | --- |
| `app.overlay` | AX58100 on SPI2/FSPI, plus the USB console override |
| `overlays/sg90.overlay` | Servo on LEDC PWM, GPIO 1 |
| `overlays/nema17.overlay` | Stepper motion controller, GPIO 2/17/18 |
| `dts/bindings/` | `asix,ax58100` and a local copy of `pwm-servo` |
| `src/esc.c` | AX58100 register access over PDI-SPI |
| `src/main.c` | Milestone 4a register checks |

Swapping actuator is a devicetree overlay, not a code change — the
EtherCAT layer never learns which motor is attached, and
`master/src/pdo_layout.h` is unchanged either way.

## Notes that will save time

- **Zephyr v4.4 restructured the stepper API.** `step-gpios`/`dir-gpios`
  moved from the driver node to a separate motion-controller node. Every
  pre-v4.4 tutorial shows the old layout and will not build. Follow
  `samples/drivers/stepper/generic` in the Zephyr tree.
- `pwm-servo` is **not** an upstream binding — it lives inside
  `samples/basic/servo_motor`, so this app carries its own copy in
  `dts/bindings/`.
- SPI mode 3 and active-low CS in `app.overlay` are not assumptions: they
  were read out of the live silicon (PDI Config `0x0150` = `0x03`) with
  `servo_master eth0 regs` on the Pi.

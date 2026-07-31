# Hardware Bring-up Checklists

## Phase 1 blocker — Pi needs a microSD card first

The Pi 3B boots from microSD and none is on hand yet. Buy a microSD card
(16–32 GB, class 10/A1, ~US$5–8), then:

1. Flash **Raspberry Pi OS Lite (64-bit)** with Raspberry Pi Imager on the
   Mac; in the Imager gear/settings enable SSH, set a username/password,
   and enter the WiFi credentials.
2. Boot the Pi, confirm `ssh <user>@raspberrypi.local` works from the Mac.
3. Run `scripts/pi-setup.sh` on the Pi, then `scripts/deploy.sh` from the
   Mac — the software side is already written and waiting.

# The actual AX58100 module (differs from the ASIX kit docs)

The board on hand is **not** the ASIX EVB the kit PDFs describe. The EVB
has a micro-USB 5V jack and an onboard 5V→3.3V/1.2V regulator; this one
is a bare module with **two RJ45 jacks and a single 10-pin header**, and
no independent power connector. It takes power through the header.

Header, top to bottom as printed on the board:

| # | Pin | Purpose |
| --- | --- | --- |
| 1 | `5V` | Power in — module regulates down to 3.3V/1.2V |
| 2 | `GND` | Ground |
| 3 | `SYNC0` | Distributed-clock sync output → STM32 EXTI |
| 4 | `SYNC1` | Second sync output → STM32 EXTI |
| 5 | `SCK` | SPI clock |
| 6 | `MISO` | SPI data, module → STM32 |
| 7 | `MOSI` | SPI data, STM32 → module |
| 8 | `IRQ` | Interrupt (the `SINT` signal in ASIX docs) → STM32 EXTI |
| 9 | `NSS` | SPI chip select (the `SCS_ESC` signal) |
| 10 | `LOAD` | EEPROM-loaded status (ASIX `EEP_DONE`) — verify in Phase 4 |

**Note on the 4.7K pull-up:** the kit docs insist on pulling `SCS_FUNC`
to 3.3V to select PDI-SPI mode. This module does not expose `SCS_FUNC`
at all, which strongly suggests the pull-up is already fitted on-board.
Confirm with a multimeter before adding one — do not assume either way.

## Phase 3a — power the module from the Pi (no STM32 needed)

Ethernet carries no power here (there is no PoE), so the module must be
powered separately before the link can ever come up.

- [ ] **Shut down and unplug the Pi first**: `sudo poweroff`, then pull
      the power. Never wire into a live GPIO header.
- [ ] Module `5V` → Pi GPIO **pin 4** (5V)
- [ ] Module `GND` → Pi GPIO **pin 6** (GND) — adjacent to pin 4
- [ ] Leave every other header pin unconnected; they are the SPI
      interface for the STM32 in Phase 4.
- [ ] Cat5e from Pi `eth0` → module **IN / Port 0** jack.
- [ ] Power the Pi back up. Expect LEDs on the module and, within a few
      seconds, `Link is Up` on `eth0`.

Pi GPIO orientation: pin 1 is the corner nearest the microSD slot;
odd-numbered pins form the row closest to the board edge. Pins 4 and 6
are the second and third pins along the *even* row.

# Part B checklists

Work these in order when the hardware arrives. Each phase ends with a
verifiable pass condition — do not move on until it passes.
Reference documents live in `knowledge base/` (ASIX datasheet, ESI design
note, reference schematic) and the original build plan docx.

## Phase 3 — RESULTS (completed 2026-07-31)

Powering the module from the Pi's GPIO 5V worked. The Pi stayed healthy
(`throttled=0x0`, ~40°C), `eth0` came up at 100 Mbps full duplex, and the
master enumerated the slave.

What the module actually reports:

| Field | Value |
| --- | --- |
| Name | `SSC-Device` |
| Vendor | `0x00000009` (**Beckhoff**, not ASIX) |
| Product | `0x26483052` |
| Revision | `0x00020111` |
| Process data | 2 bytes out, 6 bytes in |
| Mailbox | CoE, 128 bytes each way |
| Distributed clocks | supported |
| Active ports | port 0 only (OUT correctly empty) |

**The EEPROM ships with Beckhoff's stock Slave Stack Code demo ESI**, not
the ASIX one. That is not a fault — it means the EEPROM is programmed and
readable. It does mean the ASIX ESI in `knowledge base/` describes a
different configuration than what is on the module today.

### Correction to the original plan's Milestone 1

The build plan expected the bare ESC to reach **OP** with no STM32
attached. **That is not achievable with this module.** Observed:

```
Slave 1 state=0x01 (INIT)  AL status=0x0000 (No error)
```

Stuck in INIT with a *zero* AL status code is the signature of an ESC
whose host application never answers. Leaving INIT requires something to
service the process data interface over SPI and acknowledge the AL state
machine — that is the STM32 running a slave stack. A genuine
misconfiguration would instead report a non-zero AL status.

So the realistic milestone without the STM32 is: **link up, slave
enumerated, EEPROM readable, mailbox and DC configured.** All achieved.
Reaching SAFE_OP/OP now depends on Phase 4, not on rewriting the EEPROM.

Practical consequence: **do not bother flashing the ASIX ESI yet.** The
EEPROM contents must match whatever object dictionary the SOES firmware
implements, so write it once in Phase 4 when that is decided — not now.

## Confirmed ESC configuration (read live, `servo_master eth0 regs`)

Read over EtherCAT with no host MCU attached, so these are facts about
the module as it sits — not assumptions from the ASIX kit docs:

| Register | Value | Meaning |
| --- | --- | --- |
| `0x0000` Type | `0xc8` | AX58100-class ESC |
| `0x0140` PDI Control | **`0x05`** | **SPI Slave** — an SPI host MCU will work |
| `0x0141` ESC Config | `0x0e` | device emulation **OFF** |
| `0x0150` PDI Config | `0x03` | **SPI mode 3**, chip select **active low** |
| `0x0130` AL Control | `0x0001` | INIT requested |

Three things this settles without buying any hardware:

1. **The SCS_FUNC pull-up is already fitted on-board.** PDI Control could
   not read `0x05` otherwise. Do not buy a 4.7K resistor and do not try
   to add one — the module does not even expose the pin.
2. **SPI mode 3 with active-low chip select** is confirmed from the
   silicon, matching the ASIX ESI ConfigData `05 04 03 ...`.
3. **Device emulation is off**, which is precisely why the slave sits in
   INIT. The host MCU must drive AL status. Not a fault.

## Phase 4 — host MCU + SOES firmware

**Decision (2026-07-31, revised): the host MCU is a LilyGO T-Display-S3
(ESP32-S3).** Both the STM32 Nucleo-F303RE (original plan) and the
Raspberry Pi Pico 2 W are dropped.

Reason: the T-Display-S3 is **in hand with a USB-C cable that fits**,
while the Pico 2 W cannot be flashed without a micro-USB data cable that
isn't available (its SWD pads are unpopulated through-holes needing
solder). Hardware you can program today beats better hardware you can't.

**Honest correction to the earlier rationale.** The Pico was argued for
on determinism grounds — FreeRTOS and WiFi jitter versus bare metal.
That argument was overstated *for this project*. The master runs a 10 ms
cycle driving a 50 Hz hobby servo; tens of microseconds of scheduling
jitter are irrelevant at that timescale. Determinism would matter for
high-performance multi-axis motion control, not here. The ESP32-S3 is a
sound choice, with mitigations below.

Mitigations (do these, they are cheap):

- Pin the EtherCAT task to **core 1** and leave core 0 for system work.
- **Do not initialise WiFi or Bluetooth.** Unused radios are the main
  jitter source on ESP32.
- Give the EtherCAT task high priority; avoid blocking calls in it.

**Common misconception:** "the MCU can't do SPI slave mode" is irrelevant
either way. The **AX58100 is the SPI slave** (it has an NSS input); the
MCU is the SPI **master**.

No off-the-shelf SOES port exists for ESP32-S3 — but none existed for the
Pico or the STM32F303 either. The work is unchanged: implement the ESC
access layer (SPI register read/write) against the AX58100 datasheet.

### Board: LilyGO T-Display-S3 (ESP32-S3)

- Dual-core Xtensa LX7 @ 240 MHz, 16 MB flash, 8 MB PSRAM — far more
  headroom than this project needs.
- **Native USB-C CDC**: no serial-converter chip and no driver install.
  Flashing and serial console both run over the one cable.
- 3.3V logic, a direct match for the AX58100's I/O. No level shifting.
- **The 1.9" screen is a genuine bonus**: display the live target angle
  and EtherCAT state on-device. Much better feedback than serial alone,
  and it makes the final demo self-explanatory.

Broken out on the two headers (13 usable GPIO):

- P2 (left): GPIO 1, 2, 3, 10, 11, 12, 13
- P1 (right): GPIO 43, 44, 18, 17, 21, 16
- Plus 5V, 3V3, GND

Avoid: **GPIO 3** (strapping, JTAG source select), **43/44** (UART0
TX/RX), **17/18** (default I2C / STEMMA QT).

### Wiring — module 10-pin header → T-Display-S3

GPIO 10–13 are the ESP32-S3's **native FSPI (SPI2) IOMUX pins**, so using
them gives full-speed SPI with no GPIO-matrix routing penalty:

| Module pin | ESP32-S3 GPIO | Notes |
| --- | --- | --- |
| `5V` | breadboard 5V rail | shared with the Pi's 5V |
| `GND` | GND | **common ground required — see below** |
| `SCK` | **GPIO 12** (FSPICLK) | |
| `MOSI` | **GPIO 11** (FSPID) | MCU → module |
| `MISO` | **GPIO 13** (FSPIQ) | module → MCU |
| `NSS` | **GPIO 10** (FSPICS0) | chip select, **active low** |
| `IRQ` | **GPIO 16** | ASIX `SINT`, edge interrupt |
| `SYNC0` | **GPIO 21** | distributed-clock tick, edge interrupt |
| `SYNC1` | — | leave unconnected initially |
| `LOAD` | — | ASIX `EEP_DONE`; verify meaning later |

Drive NSS as a **plain GPIO**, not hardware CS — the ESC needs chip
select held low across a whole multi-byte transaction.

Spare pins if needed: GPIO 1, 2.

### Power and grounding (important)

The T-Display-S3 is powered from its **USB-C cable** (to the Mac). The
AX58100 module is powered from the **Pi's GPIO 5V**. Two separate
supplies means:

- [ ] **Run a ground wire from a T-Display-S3 GND pin to the breadboard
      ground rail.** Without a shared ground reference the SPI signals
      have no valid return path and will read garbage. This is the single
      most likely cause of "SPI returns 0xFF or 0x00".
- [ ] Do **not** also feed 5V from the Pi into the board while USB is
      connected.

Alternative once things work: power the board from the Pi's 5V rail
instead and use USB only when reflashing.

### Build and flash: from the Mac

Unlike the Pico plan, the loop now runs on the **Mac**, since that is
where the USB-C port is:

- Toolchain: ESP-IDF v5.x (installs on macOS; provides `idf.py`).
- Flash + monitor: `idf.py -p /dev/cu.usbmodem* flash monitor`.
- The Pi keeps running the EtherCAT master over SSH exactly as now.

### Firmware steps

- [ ] ESP-IDF project in `firmware/`.
- [ ] Configure SPI2/FSPI as **master, mode 3 (CPOL=1, CPHA=1), MSB-first**.
      Start slow (~1 MHz) and raise once it works; the AX58100 tolerates
      far more, but slow first makes scope debugging easy.
- [ ] **Smallest step first:** read ESC register `0x0000` over SPI and
      print it on USB serial. It must read **`0xc8`** — the same value
      the master already reads over EtherCAT, so you have a known-good
      expected answer. Do not add SOES until this works. Show it on the
      screen too; that becomes the live status display later.
- [ ] Integrate SOES; implement its HAL on top of the proven SPI reads.
- [ ] Object dictionary / PDO mapping must match `master/src/pdo_layout.h`
      (uint16 `target_angle` out, uint16 `echo_angle` in). Note the stock
      EEPROM is currently 2 bytes out / **6** bytes in — reconcile by
      rewriting the EEPROM once the dictionary is settled.
- [ ] `sudo servo_master eth0 set 90` → 90 appears on the ESP32's serial
      output and echoes back (`echo=90` in the master's status line).
      **MILESTONE 2** — data flows Pi → ESC → SPI → ESP32 → back.

## Phase 5 — Servo motion (needs: SG90, separate 5V supply)

- [ ] SG90 orange (signal) → a free ESP32-S3 GPIO driven by LEDC PWM
      (GPIO 1 or 2 are unused and suitable).
- [ ] SG90 red (5V) → its **own** 5V supply, never the board's 3V3 pin
      (a stalled SG90 draws 500–700 mA and browns out the board).
- [ ] SG90 brown (GND) → common ground with the ESP32 and servo supply.
- [ ] Firmware maps angle → pulse width: `0.5 + angle/180 * 2.0` ms at
      50 Hz (this exact formula is unit-tested in `master/test/test_angle.c`).
- [ ] `sudo servo_master eth0 set 0` / `90` / `180` → servo hits each end.
- [ ] `sudo servo_master eth0 sweep` → servo sweeps continuously.
      **MILESTONE 3** — full chain works. Record a video.

## Future upgrades (not planned in detail)

- **Real motor:** replace only the STM32 output stage (stepper/BLDC driver
  per the ASIX kits in `knowledge base/`); extend `pdo_layout.h` with
  velocity/enable fields on both sides. EtherCAT + SPI layers unchanged.
- **Daisy chain:** plug node 2 into node 1's OUT port; generalize the
  master to iterate `ec_slave[1..ec_slavecount]` instead of assuming
  slave 1.

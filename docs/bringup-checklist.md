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
Reference documents (ASIX datasheet, ESI design note, reference schematic)
and the original build plan docx live in `knowledge base/`. That directory is
ASIX's material rather than ours, so it is not redistributed with this repo —
it is gitignored. Download the AX58100 kits from ASIX and unpack them there if
you need them.

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
readable. It does mean ASIX's own ESI, and the one this project now carries
in `esi/EthercatServoNode.xml`, both describe a different configuration than
what is on the module today. Everything downstream is built for the image
that is actually on the chip; see `firmware/soes/device_identity.h`.

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

Practical consequence: **do not bother flashing any ESI yet.** The EEPROM
contents must match whatever object dictionary the SOES firmware implements,
so write it once when that is decided — not now.

That point has since arrived. The dictionary is settled, `esi/` holds our own
ESI, and `scripts/write-eeprom.sh` writes it. Doing so is optional: the chain
reaches OP on the stock image and always has. What it buys is our own identity
instead of Beckhoff's, and a four-byte SM3 with no padding entry. See
"Rewriting the EEPROM" below.

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
| `SYNC0` | **GPIO 2** | distributed-clock tick, edge interrupt. **Not GPIO 21** — see phase 7 |
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

The loop now runs on the **Mac**, since that is where the USB-C port is.
The Pi keeps running the EtherCAT master over SSH exactly as now.

**Stack decided 2026-08-01: Zephyr RTOS**, not ESP-IDF. Built for the
in-tree **`esp32s3_devkitc`** board and flashed to the T-Display-S3 —
same SoC, and nothing here needs DevKitC-specific peripherals, so no
custom board port is required.

Why Zephyr: it has a real stepper subsystem
(`zephyr,gpio-step-dir-stepper-ctrl`), so SG90-vs-NEMA17 becomes two
devicetree overlays instead of hand-written motion code, and
SOES + Zephyr + devicetree is a much better fit for an EtherCAT job than
ESP-IDF.

**Accepted trade-off: the 1.9" display will not work.** It is an 8-bit
i8080 parallel ST7789 and Zephyr's ST7789 driver is SPI-only. Under
ESP-IDF it would have worked via `esp_lcd`. Status goes to the USB
serial console instead.

```sh
./scripts/fw.sh build          # milestone 4a, no motor
./scripts/fw.sh build sg90     # phase 5
./scripts/fw.sh build nema17   # phase 6
./scripts/fw.sh flash && ./scripts/fw.sh monitor
```

### Firmware steps

- [x] Zephyr application in `firmware/` (`app.overlay`, `src/esc.c`,
      `src/main.c`, actuator overlays, custom `asix,ax58100` binding).
- [x] SPI2/FSPI configured as **master, mode 3 (CPOL=1, CPHA=1),
      MSB-first**, 1 MHz to start. Raise it once the link works; slow
      first makes a scope trace easy to read.
- [ ] **Smallest step first:** read ESC register `0x0000` over SPI. It
      must read **`0xc8`** — the same value the master already reads over
      EtherCAT, so there is a known-good expected answer. `src/main.c`
      also cross-checks `0x0140`, `0x0141` and `0x0150` against the
      values `servo_master eth0 regs` reported. Do not add SOES until
      this passes.
- [ ] If register `0x0000` reads `0x00` or `0xff`, that is a **dead SPI
      link, not a config problem**. In order of likelihood: no common
      ground between the USB-powered ESP32 and the Pi-powered module;
      MISO/MOSI swapped (they are adjacent on the header); module
      unpowered.
- [ ] Integrate SOES; implement its HAL on top of the proven SPI reads.
- [x] Object dictionary / PDO mapping must match `master/src/pdo_layout.h`
      (uint16 `target_angle` out, uint16 `echo_angle` in). The stock EEPROM
      is 2 bytes out / **6** bytes in, which the TxPDO reaches with a padding
      entry. Both sides now take those numbers from one source rather than
      restating them — see "Rewriting the EEPROM".
- [ ] `sudo servo_master eth0 set 90` → 90 appears on the ESP32's serial
      output and echoes back (`echo=90` in the master's status line).
      **MILESTONE 2** — data flows Pi → ESC → SPI → ESP32 → back.

## Phase 5 — Servo motion (needs: SG90, 5V supply)

**MILESTONE 5a PASSED 2026-08-02** — servo sweeps end to end under local
control, driven by `servo_demo()` in `firmware/src/main.c`. Build with
`./scripts/fw.sh build sg90`.

- [x] SG90 orange (signal) → ESP32-S3 **GPIO 1**, LEDC PWM channel 0.
- [x] SG90 red (5V) → Raspberry Pi header **pin 2**. Interim supply: the
      dedicated DC5V2A adapter (Jin-Hua ref 11396) is not bought yet. The
      Pi rail has held up fine unloaded — the ESC keeps reading 0xc8 in the
      heartbeat while the servo sweeps, which is what would fail first if
      the servo were dragging the rail down. Revisit if the servo is ever
      loaded, since a stalled SG90 pulls 500-700 mA.
- [x] SG90 brown (GND) → common ground with the ESP32 and the Pi.
- [x] Firmware maps angle → pulse width, 500..2500 us at 50 Hz, in integer
      arithmetic matching `angle_to_pulse_ms()` in `master/src/angle.c` so
      both ends agree on what a degree means.

Remaining, and blocked on SOES (phase 4b) — the angle has to arrive over
EtherCAT before the master can command it:

- [x] `sudo servo_master eth0 set 0` / `90` / `180` → angle arrives and echoes.
- [x] `sudo servo_master eth0 sweep` → continuous sweep, echo tracks target
      within one degree (the offset is round-trip latency: the master samples
      its own ramp a cycle before the echo returns), wkc 3/3, low_wkc 0.
      **MILESTONE 3 PASSED 2026-08-02** — full chain works.

Power note: the Pi ran the master, powered the AX58100 from header pin 4 and
the SG90 from pin 2 simultaneously, with `vcgencmd get_throttled` polled
every 3 s throughout the sweep. It stayed `0x0` — no undervoltage, no resets.
The dedicated DC5V2A adapter (Jin-Hua ref 11396) is therefore still optional
for an unloaded servo, and only becomes necessary under mechanical load.

## Phase 7 — Distributed-clock synchronisation (PASSED 2026-08-02)

**MILESTONE 7 PASSED.** The slave runs one cycle per SYNC0 edge, locked to
the bus clock, through a full sweep with `wkc 3/3` and `low_wkc 0`.

Measured: 201 edges per 2 s against an expected 200 (10 ms cycle = 100 Hz),
sustained. Master phase error converged 1,134,440 ns -> ~45,000 ns and held,
peak excursion 278,200 ns — that peak is Linux scheduling jitter on a
non-PREEMPT_RT kernel, and is the honest limit of this setup.

What changed:

- **Master** (`master/src/ecat.c`): `ecat_to_op()` now calls
  `ec_dcsync0(1, TRUE, 10 ms, 2 ms shift)` after SAFEOP and before requesting
  OP, so the first OP cycle is already DC-paced. `ecat_dc_correction()` is a
  PI controller (SOEM's `red_test.c` constants) that phase-locks the master's
  `clock_nanosleep` deadline to `ec_DCtime`. Without it the master's cycle
  and SYNC0 drift relative to each other, which is worse than no DC at all.
- **Master** (`master/src/main.c`): the status line gains
  `dc=<phase error>ns peak=<worst since settling>ns`.
- **Slave** (`firmware/src/sync0.c`, new): edge interrupt on GPIO 2.
  `sync0_pace()` polls at 1 ms until 3 edges have been seen, then blocks on
  edges with a 25 ms timeout, and reverts to polling if they stop. Both the
  SOES loop and the minimal-slave loop call it.
- `CONFIG_ESC_DC_SYNC` (default y) and `SERVO_DC=0` on the master turn it off,
  so the two modes can be compared back to back.

### Register 0x0151 — checked, and fine

The worry was that `0x0151` bit 2 (SYNC0 output vs LATCH0 input) is
EEPROM-loaded and we never write the EEPROM. Resolved: it reads **`0x44`**,
SYNC0 is a push-pull active-high output. Nothing about this module prevents
DC. `servo_master eth0 regs` prints and decodes it.

### SYNC0 is on GPIO 2, not GPIO 21

**GPIO 21 does not work as an input on this board.** With the ESC provably
generating SYNC0, GPIO 21 read a hard low that beat an internal pull-up and
showed zero transitions across 148k samples spanning five DC cycles. The same
wire on GPIO 2 gave a clean 201 edges per 2 s immediately, nothing else
changed.

GPIO 21 was never verifiable before, because SYNC0 is not driven until a
master activates DC — so a static level on that pin proved nothing either
way. `app.overlay` is the authority: **module pin 3 (SYNC0) -> ESP32 GPIO 2.**

### Verification, in order

- [ ] `./scripts/fw.sh build sg90` then `flash`; console shows
      `SYNC0 armed on GPIO 2 — polling at 1 ms until the master activates
      the distributed clock`.
- [ ] `sudo servo_master eth0 set 90` → master prints
      `Distributed clocks: SYNC0 active on slave 1, 10000 us cycle, 2000 us shift.`
- [ ] Slave console within a few hundred ms:
      `SYNC0 running — cycle is now DC-synchronised (3 edges seen)`.
- [ ] Master `dc=` figure converges toward zero over ~2 s. `peak=` after
      settling is the jitter number worth quoting.
- [ ] `wkc=3/3`, `low_wkc=0` throughout — DC must not cost working counter.
- [ ] Ctrl-C → slave prints `SYNC0 stopped after N edges — back to 1 ms
      polling.` This is expected, not a fault.
- [ ] A/B it: `sudo SERVO_DC=0 servo_master eth0 sweep` versus the default.
      Same motion, no `dc=` column, slave stays `polled`.

**If the slave never locks** but `0x0151` bit 2 is set, suspect the wire
before the code: SYNC0 is module pin 3 → GPIO 2. Note that SYNC0 reads high
after a power cycle whether or not it is connected, so a static level proves
nothing — only counted edges do.

## Future upgrades (not planned in detail)

- **Real motor:** replace only the STM32 output stage (stepper/BLDC driver
  per the ASIX kits in `knowledge base/`); extend `pdo_layout.h` with
  velocity/enable fields on both sides. EtherCAT + SPI layers unchanged.
- **Daisy chain:** plug node 2 into node 1's OUT port; generalize the
  master to iterate `ec_slave[1..ec_slavecount]` instead of assuming
  slave 1.

## Rewriting the EEPROM

Optional, and deliberately not done for most of this project's life. The
module arrived carrying Beckhoff's demo SII, that image is valid, and the
whole chain reaches OP on it. Everything that looks hardcoded in the slave —
the Beckhoff identity in `0x1018`, the padding entry in the TxPDO — is a
consequence of matching it.

What rewriting buys, and it is worth being honest that it is not much
functionally:

- the node reports its own identity instead of enumerating as a Beckhoff
  demo board, which is the first thing anyone familiar with EtherCAT checks
- SM3 becomes four bytes, so the padding entry disappears
- any ESI-driven master (TwinCAT) can import the device at all

The source of truth is `esi/EthercatServoNode.xml`. Two things are generated
from it, so that the EEPROM, the object dictionary and the master's struct
cannot drift apart:

```
esi/EthercatServoNode.xml ──┬─> firmware/soes/esi_generated.h
                            └─> build/sii.bin
```

Regenerate the header after any ESI change:

```
./scripts/esi_tool.py header -o firmware/soes/esi_generated.h
```

### The one sharp edge

Words 0..7 of the SII configure the PDI — the physical interface the ESC
exposes, which on this board is the SPI slave port the ESP32-S3 is wired to.
Write those wrong and the ESC stops answering over SPI, which is the route
you would use to fix them; recovery then needs an external programmer clipped
to the EEPROM.

`esi_tool.py` therefore refuses to synthesise that region. It copies it out of
a backup of the live device instead. The CRC in word 7 covers exactly that
block, so copying it keeps the checksum valid without recomputing anything.
(`--use-esi-config` does synthesise it, CRC included, for a board that is
already unreachable and has nothing left to lose.)

### What the image does not carry

No DC category (type 60) is written, even though the ESI declares a `Dc`
OpMode. SOEM configures distributed clocks from the ESC's own registers, so
SYNC0 and the phase-error figures are unaffected — this is why the omission is
invisible on our bench. A tool that configures DC from the EEPROM alone would
not offer a SYNC0 mode; give it `esi/EthercatServoNode.xml` directly. The tool
prints a reminder whenever it writes an image for an ESI that declares DC.

### Procedure

```
sudo ./scripts/write-eeprom.sh eth0
```

It reads the current EEPROM to `backups/sii-<stamp>.bin` before anything else
and refuses to continue without one, builds the image with the config words
copied from that backup, shows a decode of exactly what is about to be
written, and asks. After writing it reads back and compares. If the readback
disagrees it tells you to restore and does not pretend otherwise:

```
sudo ./scripts/write-eeprom.sh eth0 --restore backups/sii-<stamp>.bin
```

**Order matters.** The EEPROM goes first, then both builds:

1. `sudo ./scripts/write-eeprom.sh eth0`
2. Power-cycle. The ESC loads the SII at reset; until then it is still
   running on the old contents.
3. `sudo servo_master eth0 scan` — expect vendor `0x00000b95`, product
   `0x00620300`, revision `0x00010000`.
4. Rebuild both sides for the four-byte input size:
   `CONFIG_ESC_SII_REWRITTEN=y` for the firmware, `-DSERVO_SII_REWRITTEN=ON`
   for the master.

Between steps 1 and 4 the slave declares six bytes of inputs while the EEPROM
declares four, so the master refuses SAFEOP. That is expected in the gap, not
a fault. Reversing the order produces the same stall for the same reason.

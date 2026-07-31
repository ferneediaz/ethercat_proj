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

**Decision (2026-07-31): the host MCU is a microcontroller on SPI —
Raspberry Pi Pico recommended over ESP32.** The STM32 Nucleo-F303RE from
the original plan is dropped.

Rationale: EtherCAT cycles are driven by the SYNC0 interrupt and the
AX58100 delivers ~150 ns sync jitter; a bare-metal RP2040 preserves that,
whereas ESP32's FreeRTOS scheduling and WiFi stack introduce jitter for
no benefit here (the wireless is unused). The Pico is also 3.3V logic —
a direct match for the AX58100's I/O with no level shifting — and
pico-sdk is CMake + C, the same toolchain this repo already uses.

**Common misconception:** "RP2040 doesn't support SPI slave mode" is true
but irrelevant. The **AX58100 is the SPI slave** (it has an NSS input);
the MCU is the SPI **master**, which the RP2040 fully supports.

No off-the-shelf SOES port exists for RP2040 — but none existed for the
STM32F303 either. The work is the same: implement the ESC access layer
(SPI register read/write) against the AX58100 datasheet.

### Wiring — module 10-pin header → Pico

| Module pin | Pico | Notes |
| --- | --- | --- |
| `5V` | VBUS (pin 40) or shared 5V | already fed from the Pi today |
| `GND` | any GND | **common ground required** |
| `SCK` | SPI0 SCK (GP18, pin 24) | |
| `MOSI` | SPI0 TX (GP19, pin 25) | MCU → module |
| `MISO` | SPI0 RX (GP16, pin 21) | module → MCU |
| `NSS` | GP17 (pin 22) | chip select, **active low** |
| `IRQ` | any GPIO, edge interrupt | ASIX `SINT` |
| `SYNC0` | any GPIO, edge interrupt | distributed-clock tick |
| `SYNC1` | optional | leave unconnected initially |
| `LOAD` | optional input | ASIX `EEP_DONE`; verify meaning |

Drive NSS as a plain GPIO rather than hardware CS — the ESC needs chip
select held low across a whole multi-byte transaction.

### Firmware steps

- [ ] pico-sdk project in `firmware/`, CMake, C.
- [ ] Configure SPI0 as **master, mode 3 (CPOL=1, CPHA=1), MSB-first**.
      Start slow (~1 MHz) and raise once it works; the AX58100 tolerates
      far more, but slow first makes scope debugging easy.
- [ ] **Smallest step first:** read ESC register `0x0000` over SPI and
      print it on USB serial. It must read **`0xc8`** — the same value
      the master already reads over EtherCAT, so you have a known-good
      expected answer. Do not add SOES until this works.
- [ ] Integrate SOES; implement its HAL on top of the proven SPI reads.
- [ ] Object dictionary / PDO mapping must match `master/src/pdo_layout.h`
      (uint16 `target_angle` out, uint16 `echo_angle` in). Note the stock
      EEPROM is currently 2 bytes out / **6** bytes in — reconcile by
      rewriting the EEPROM once the dictionary is settled.
- [ ] `sudo servo_master eth0 set 90` → 90 appears on the Pico's serial
      output and echoes back (`echo=90` in the master's status line).
      **MILESTONE 2** — data flows Pi → ESC → SPI → Pico → back.

## Phase 5 — Servo motion (needs: SG90, separate 5V supply)

- [ ] SG90 orange (signal) → a Pico PWM-capable GPIO.
- [ ] SG90 red (5V) → its **own** 5V supply, never the Pico's 3V3 pin
      (a stalled SG90 draws 500–700 mA and browns out the board).
- [ ] SG90 brown (GND) → common ground with Pico and servo supply.
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

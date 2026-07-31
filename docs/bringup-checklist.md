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

## Phase 3 — AX58100 bring-up (needs: AX58100 board, Cat5e cable, 5V USB supply)

- [ ] Power the AX58100 board from its own 5V/USB supply; power LED lights.
- [ ] Cat5e from the Pi's `eth0` to the AX58100 **IN / Port 0** jack (not OUT).
- [ ] `sudo ~/ethercat/master/build/servo_master eth0 scan`
      → a slave appears with vendor `0x0B95`. A blank EEPROM may show an
      unnamed slave — chip-alive is the pass here.
- [ ] Write the ESI/EEPROM (one-time):
      - Preferred (Linux-only): SOEM `eepromtool` built at
        `~/ethercat/SOEM/build/test/linux/eepromtool/eepromtool`.
        The ESI XML is in the repo under `knowledge base/.../ESI_File/`.
        Note: eepromtool writes a binary EEPROM image; if only the XML is
        available, generate/obtain the `.bin` (ESI Design Note explains the
        EEPROM layout; ConfigData `050403440a00000000001a00003c`).
      - Fallback: TwinCAT on a Windows PC → drop the ESI XML into
        `TwinCAT\3.1\Config\Io\EtherCAT`, scan, EEPROM Update (this is the
        route the ASIX user guide documents).
- [ ] Power-cycle the board, rescan → name and product code `0x00620300`
      appear; `scan` prints PASS.
- [ ] `sudo servo_master eth0 op` → slave reaches OP.
      **MILESTONE 1** — master/slave EtherCAT link proven.

## Phase 4 — STM32 + SOES firmware (needs: Nucleo-F303RE, breadboard, jumpers, 4.7K resistor)

Wiring (from the AX58100 reference schematic + build plan Stage 3):

- [ ] AX58100 SCLK → STM32 SPI1_SCK
- [ ] AX58100 MOSI → STM32 SPI1_MOSI
- [ ] AX58100 MISO → STM32 SPI1_MISO
- [ ] AX58100 SCS_ESC → STM32 SPI1_NSS (chip select)
- [ ] AX58100 SINT → STM32 GPIO (EXTI)
- [ ] AX58100 SYNC0 → STM32 GPIO (EXTI)
- [ ] AX58100 reset pin → STM32 GPIO output
- [ ] AX58100 GND ↔ STM32 GND (common ground — required)
- [ ] **4.7K pull-up from SCS_FUNC to 3.3V.** Verify ~3.3V with a
      multimeter before any firmware debugging. Missing this is the #1
      cause of dead SPI.

Firmware (in `firmware/`, STM32CubeIDE project):

- [ ] CubeMX: target F303RE; SPI1 **Mode 3 (CPOL=1, CPHA=1), MSB-first**;
      TIM2 PWM channel at 50 Hz; the two EXTI inputs; reset GPIO output.
- [ ] Smallest step first: read one known AX58100 ID register over SPI and
      print it on the ST-LINK VCP. Do not add SOES on top of unproven SPI.
- [ ] Integrate SOES (Simple Open EtherCAT Slave); implement its HAL as
      SPI register reads/writes per the AX58100 datasheet.
- [ ] Object dictionary / PDO mapping must match
      `master/src/pdo_layout.h` exactly (uint16 target_angle out,
      uint16 echo_angle in). Echo the received angle back as echo_angle.
- [ ] `sudo servo_master eth0 set 90` → 90 visible on the STM32 serial
      output and echoed back (`echo=90` in the master's status line).
      **MILESTONE 2** — data flows Pi → ESC → SPI → STM32 → back.

## Phase 5 — Servo motion (needs: SG90, separate 5V supply)

- [ ] SG90 orange (signal) → STM32 TIM2 PWM pin.
- [ ] SG90 red (5V) → its **own** 5V supply, never the Nucleo's 5V pin
      (a stalled SG90 draws 500–700 mA and browns out the board).
- [ ] SG90 brown (GND) → common ground with STM32 and servo supply.
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

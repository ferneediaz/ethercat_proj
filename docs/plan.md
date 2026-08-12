# EtherCAT Servo Node — AI-Executable Project Plan

## Context

The goal is a complete EtherCAT chain: **Raspberry Pi 3B (SOEM master) → AX58100 (ESC slave) → STM32 Nucleo-F303RE (SPI host, SOES) → SG90 servo**. A human-readable build plan already exists (`knowledge base/EtherCAT_Intern_Build_Plan_1.docx`, kept locally and not committed); this plan turns it into concrete, agent-executable steps.

**Current hardware reality:** only the Raspberry Pi 3B exists today. The AX58100 board, STM32 Nucleo, and servo come later. So the plan has two parts:

- **Part A (execute now):** Claude Code runs on the Mac, drives the Pi over SSH. Set up the Pi, build SOEM, and write the complete custom master application so it is ready the day hardware arrives.
- **Part B (hardware-gated, documented for later):** AX58100 bring-up, STM32/SOES firmware, servo motion. Detailed checklists, not executed now.

**Locked decisions (from user):**
- Agent runs on Mac, executes on Pi via SSH; repo lives on the Mac.
- Build scope now = SOEM setup + master application only (no STM32 firmware scaffolding yet).
- Motor = SG90 first, with a short upgrade-path section for a real motor later.
- Topology = single slave node; daisy-chaining a second node is a future note only.

**Key facts from the ASIX kit** (their reference material is not redistributed
with this repo — see `.gitignore`; download it from ASIX):
- ASIX's ESI XML: Vendor ID `0x0B95`, Product Code `0x00620300`, Revision `0x00000002`, CiA 402 profile, EEPROM ConfigData `050403440a00000000001a00003c`. **This is not what the module reports.** As shipped it carries Beckhoff's stock SSC demo image (`0x00000009` / `0x26483052` / `0x00020111`); this project's own ESI is `esi/EthercatServoNode.xml`.
- AX58100 SPI to STM32: Mode 3 (CPOL=1/CPHA=1), MSB-first; `SCS_FUNC` needs a 4.7K pull-up to 3.3V to enable PDI-SPI on `SCS_ESC`.
- STM32 slave firmware source is not freely downloadable from ASIX → the plan assumes the open-source **SOES** stack for Part B.

---

## Part A — Execute now (Pi only)

### Phase 0: Repo + Mac↔Pi link

1. **Initialize the repo** at `/Users/dandiaz/workspace/ethercat_proj` (`git init`, `.gitignore` for build dirs). Layout:
   ```
   ethercat_proj/
   ├── knowledge base/        # ASIX kits + docx — local only, gitignored
   ├── docs/                  # plan.md (this plan, committed), bringup-checklists
   ├── master/                # custom SOEM master app (C, CMake)
   ├── scripts/               # pi-setup.sh, deploy.sh (rsync to Pi), run helpers
   └── firmware/              # empty placeholder + README (Part B)
   ```
2. **Establish SSH to the Pi.** Ask the user for the Pi's hostname/IP and username (likely `raspberrypi.local`). If the Pi isn't flashed yet, the user must do Task 1.1 from their docx manually (Raspberry Pi Imager is interactive) — the plan pauses here until `ssh <user>@<pi>` works. Set up key-based auth (`ssh-copy-id`) so the agent can run non-interactive commands.
3. **`scripts/deploy.sh`:** rsync `master/` to `~/ethercat/` on the Pi and run the build there (Pi 3B is slow but SOEM + a small app builds in minutes; no cross-compile complexity needed).

### Phase 1: Pi provisioning + SOEM build

Executed over SSH, captured in `scripts/pi-setup.sh` so it's reproducible:

1. `sudo apt update && sudo apt install -y git cmake build-essential`
2. Clone SOEM on the Pi. **Pin a known version**: use the SOEM v1.4.x tag (`OpenEtherCATsociety/SOEM`), because the v1.x tree ships the classic `slaveinfo`/`simple_test` samples the docx references and its API is what most examples use. Record the exact commit in `docs/`.
3. `mkdir build && cd build && cmake .. && make`
4. Verify: `sudo ./test/linux/slaveinfo/slaveinfo eth0` runs and reports **0 slaves found** (expected — nothing is attached). That exact output is the Phase 1 pass criterion; an error about the interface or permissions is a fail to debug.
5. Note the Ethernet interface name from `ip link` (expect `eth0`) into `docs/pi-facts.md`. The Pi stays on WiFi for SSH; `eth0` is reserved for EtherCAT.

### Phase 2: Custom master application (`master/`)

A small C program, `servo_master`, built with CMake against SOEM, structured so every part testable without hardware is tested now.

**Files:**
- `master/CMakeLists.txt` — finds/builds SOEM (FetchContent or add_subdirectory of the pinned checkout), builds `servo_master` + a host-side unit test binary.
- `master/src/main.c` — CLI entry: `servo_master <ifname> [command]`.
- `master/src/ecat.c/.h` — SOEM lifecycle: init interface → `ec_config_init` → verify slave 1 is Vendor `0x0B95` / Product `0x00620300` → PDO mapping → transition INIT→PREOP→SAFEOP→OP with clear error messages at each step → cyclic loop (10 ms cycle via `clock_nanosleep`, `ec_send_processdata`/`ec_receive_processdata`, working-counter check).
- `master/src/angle.c/.h` — pure logic, no SOEM dependency: clamp/validate angle 0–180, map angle → output PDO value, parse sweep parameters. **Unit-testable on the Mac.**
- `master/test/test_angle.c` — plain-assert unit tests for `angle.c`; runs on Mac (`cc` + run) and on the Pi.

**CLI commands:**
- `scan` — list slaves with vendor/product/name (wraps what slaveinfo shows, plus a pass/fail check against the expected ASIX IDs).
- `op` — bring slave to OP and hold, printing state + working counter (Milestone 1 tool).
- `set <angle>` — write one target angle to the output PDO.
- `sweep [period_s]` — 0→180→0 loop until Ctrl-C (Milestone 3 demo).

**PDO layout:** start with the simple Stage-4 mapping from the docx — one output variable (target angle, e.g. `uint16`) and one input (echo/status). Keep the PDO offsets in one header (`master/src/pdo_layout.h`) with a comment that they must match the SOES application object dictionary in Part B.

**Definition of done for Part A:**
- Repo committed with the structure above.
- `pi-setup.sh` has been run; SOEM builds clean on the Pi.
- `servo_master eth0 scan` runs on the Pi and cleanly reports "no slaves found" (graceful, correct exit code).
- `test_angle` passes on the Mac and the Pi.
- `docs/bringup-checklist.md` written (the Part B checklists below).

---

## Part B — Hardware-gated (documented now, executed when boards arrive)

Written into `docs/bringup-checklist.md` as step/verify checklists an agent + human can walk through:

### Phase 3: AX58100 bring-up (needs AX58100 board)
1. Power board via its own 5V; Cat5e from Pi `eth0` → AX58100 **IN/Port 0** jack.
2. `servo_master eth0 scan` → expect Vendor `0x0B95` (blank EEPROM may show unnamed slave — chip alive is the pass).
3. Write ESI to EEPROM: `sudo ./scripts/write-eeprom.sh eth0`, which wraps SOEM's `eepromtool` with a mandatory backup, preserves the PDI config words and verifies the readback. Power-cycle, rescan → expect Product Code `0x00620300`, revision `0x00010000`. Deferred in practice until the object dictionary settled; see "Rewriting the EEPROM" in the bring-up checklist.
4. `servo_master eth0 op` → slave reaches OP with the bare ESC. **Milestone 1.**

### Phase 4: STM32 + SOES firmware (needs Nucleo-F303RE, wiring kit)
1. Wiring per docx Stage 3: SPI1 (SCK/MOSI/MISO/NSS→SCS_ESC), SINT + SYNC0 → EXTI GPIOs, reset GPIO, common ground, **4.7K pull-up SCS_FUNC→3.3V** (verify ~3.3V with multimeter before any firmware debugging).
2. CubeMX project: F303RE, SPI1 Mode 3 MSB-first, TIM2 PWM @ 50 Hz, EXTI pins, reset GPIO out.
3. Smallest-step SPI bring-up first: read one known AX58100 ID register, print over ST-LINK VCP. Only then integrate SOES.
4. Port SOES HAL to the AX58100 SPI register access (ASIX datasheet + ESI Design Note in `knowledge base/` are the references). Object dictionary must match `pdo_layout.h` from Part A.
5. `servo_master eth0 set 90` → value visible in STM32 serial output. **Milestone 2.**

### Phase 5: Servo motion (needs SG90 + separate 5V supply)
1. Wire SG90: signal→TIM2 channel, 5V from its own supply, common ground (never power servo from the Nucleo).
2. Firmware maps angle PDO → pulse 0.5–2.5 ms @ 50 Hz (formula already in the docx).
3. `servo_master eth0 set 0|90|180`, then `sweep`. **Milestone 3 — record video.**

### Future notes (short sections only)
- **Real-motor upgrade:** swap Phase 5 for a stepper/BLDC driver; EtherCAT + SPI + SOES layers are untouched — only the PDO layout gains fields (velocity/enable) and the STM32 output stage changes. The ASIX stepper/BLDC kits in `knowledge base/` become the reference.
- **Daisy chain:** second node plugs into the first AX58100's OUT port; master app changes = iterate slaves in `ec_slave[]` instead of assuming slave 1.

---

## Verification (Part A, runnable now)

1. On Mac: build & run `test_angle` — all asserts pass.
2. On Pi (via SSH): `pi-setup.sh` idempotent re-run succeeds; SOEM `slaveinfo eth0` and `servo_master eth0 scan` both run and report no slaves without crashing.
3. `deploy.sh` round-trip: edit a file on Mac → deploy → rebuild on Pi → rerun.
4. Everything committed; `docs/bringup-checklist.md` reviewed against the docx stages.

**Blocked-on-user items:** Pi must be flashed + reachable over SSH (interactive Imager step), and user provides Pi hostname/username at Phase 0.

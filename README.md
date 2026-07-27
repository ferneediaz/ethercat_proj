# EtherCAT Servo Node

A number typed on a Raspberry Pi becomes physical servo motion over real
industrial EtherCAT:

```
Raspberry Pi 3B ──EtherCAT──> AX58100 ──SPI──> STM32 Nucleo-F303RE ──PWM──> SG90
   (SOEM master)               (ESC slave)        (SOES host)             (servo)
```

Current status: **Part A** — only the Pi exists; master software is built
and ready. Slave hardware (AX58100, STM32, servo) comes later — see
`docs/bringup-checklist.md` for the hardware phases.

## Layout

- `master/` — custom SOEM master app (`servo_master`) + unit tests
- `scripts/pi-setup.sh` — idempotent Pi provisioning (tools + SOEM v1.4.0)
- `scripts/deploy.sh` — rsync to the Pi and build there
- `docs/` — execution plan, bring-up checklists, Pi facts
- `firmware/` — STM32/SOES slave firmware (Part B, empty for now)
- `knowledge base/` — ASIX AX58100 kits, datasheets, ESI files, original plan

## Quick start

```sh
# On the Pi (once):
bash scripts/pi-setup.sh

# From the Mac:
PI_USER=<user> PI_HOST=<host> scripts/deploy.sh

# On the Pi:
sudo ~/ethercat/master/build/servo_master eth0 scan   # list/check slaves
sudo ~/ethercat/master/build/servo_master eth0 op     # bring slave to OP
sudo ~/ethercat/master/build/servo_master eth0 set 90 # hold an angle
sudo ~/ethercat/master/build/servo_master eth0 sweep  # 0->180->0 demo
```

Unit tests (run anywhere): `cc master/src/angle.c master/test/test_angle.c -o test_angle && ./test_angle`

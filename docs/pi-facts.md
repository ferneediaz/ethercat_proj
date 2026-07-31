# Pi Facts

## Flashing settings (use these exact values)

Chosen so `scripts/deploy.sh` works with no environment variables — its
defaults are `PI_USER=pi`, `PI_HOST=raspberrypi.local`.

- OS image: **Raspberry Pi OS Lite (64-bit)**
- Hostname: `raspberrypi`
- Username: `pi`
- Password: _(your choice — you'll type it on first SSH)_
- SSH: **enabled**, **public-key authentication only**
  - Key: `~/.ssh/id_ed25519.pub` (`SHA256:pXsLnbJ+...`, dan@everblissgreen.com)
  - The private key is passphrase-protected, so load it into the macOS
    keychain once on the Mac — otherwise every SSH prompts for the
    passphrase and automated deploys stall:
    `ssh-add --apple-use-keychain ~/.ssh/id_ed25519`
  - The account password set in Imager is separate; it's for console
    login. Raspberry Pi OS grants the first user passwordless `sudo`,
    which is what lets `pi-setup.sh` run unattended.
- WiFi: your 2.4 GHz network SSID + password (the Pi 3B does support
  5 GHz, but 2.4 GHz is more reliable for headless setup)
- WiFi country: set it, or the WiFi radio stays disabled

Card: 32 GB microSD. Flash from the Mac with Raspberry Pi Imager
(installed at `/Applications/Raspberry Pi Imager.app`).

## Confirmed working (2026-07-31)

- Hostname: `raspberrypi` → `raspberrypi.local` resolves via mDNS
- IP at first boot: `192.168.0.133` (DHCP; router at `192.168.0.1`).
  Prefer `raspberrypi.local` — the IP can change.
- Username: `pi`, passwordless `sudo` confirmed
- OS: Debian GNU/Linux 13 (trixie), aarch64, kernel `6.18.34+rpt-rpi-v8`
- RAM 905 MB, 4 cores, 25 GB free on the 32 GB card
- SSH: public-key only, key loaded into the macOS keychain
- **EtherCAT interface: `eth0`** (MAC `b8:27:eb:29:fb:7f`), currently
  `NO-CARRIER` — nothing attached yet, which is correct. SSH runs over
  `wlan0` so `eth0` stays free for the EtherCAT segment.
- SOEM v1.4.0, commit `abbf0d42e38d6cfbaa4c1e9e8e07ace651c386fd`
  - built at `~/ethercat/SOEM/build/` (libsoem.a, slaveinfo,
    simple_test, eepromtool)
- `servo_master` built at `~/ethercat/master/build/servo_master`;
  `test_angle` passes on the Pi.

### Verified behaviour with no hardware attached

| Command | Result |
| --- | --- |
| `servo_master eth0 scan` | "No slaves found", exit 0 |
| `servo_master eth0 op` | refuses, exit 1 |
| `servo_master badname scan` | names the bad interface, exit 1 |

## Gotchas hit during setup (avoid repeating)

- `apt` on a freshly-flashed Pi may already be running an automatic
  update; `pi-setup.sh` now waits for the lock instead of failing.
- Run long Pi jobs detached (`setsid nohup ... &`) and poll for the
  `~/ethercat/.setup-complete` marker. Do **not** detect progress with
  `pgrep -f pi-setup.sh` — it matches its own SSH command line and
  always reports a false positive.
- The SSH host key must be accepted for `raspberrypi.local` separately
  from the bare IP, or `scp` fails with "Host key verification failed".

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

## Confirmed after boot

- Hostname: _(pending first SSH)_
- Username: _(pending)_
- EtherCAT interface: _(expected `eth0`; confirm with `ip -brief link`)_
- SSH: WiFi only; `eth0` is reserved for the EtherCAT segment.
- SOEM commit: _(see `~/ethercat/SOEM_COMMIT.txt` on the Pi after setup)_

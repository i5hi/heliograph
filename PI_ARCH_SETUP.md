# Minimal Arch Linux ARM on Raspberry Pi 4 for the `helio` broadcast daemon

Target: a **single-purpose, headless broadcast appliance** — minimal Arch Linux ARM (aarch64), ALSA-direct audio (no PipeWire), running the `helio` Rust daemon (audio-in → Opus → Icecast, + image snapshots). Ethernet uplink for reliability.

Design note: **no PipeWire/WirePlumber.** The daemon owns the one capture device exclusively; a session manager only adds failure modes for a single-purpose box. `cpal` → ALSA directly.

---

## 1. Flash Arch Linux ARM (from scratch, on another Linux machine)
Use the **rpi-aarch64** tarball (has Pi boot/config.txt + overlay support for audio HATs). Replace `sdX` with your card.

```bash
# partition: p1 = 512M FAT32 (boot), p2 = rest ext4 (root)
fdisk /dev/sdX          # o (new MBR), n p1 +512M, t c, n p2 (rest), w
mkfs.vfat /dev/sdX1
mkfs.ext4 /dev/sdX2

mkdir -p boot root
mount /dev/sdX1 boot
mount /dev/sdX2 root

# get the tarball from archlinuxarm.org (Pi 4 / aarch64 page), then:
bsdtar -xpf ArchLinuxARM-rpi-aarch64-latest.tar.gz -C root
sync
mv root/boot/* boot/
sync
umount boot root
```
Boot on the Pi (ethernet plugged in). Logins: `alarm`/`alarm`, root `root`/`root`.
> Check archlinuxarm.org's current Pi 4 page for any fstab/boot nuance before first boot.

## 2. First boot — keyring + full update
```bash
pacman-key --init
pacman-key --populate archlinuxarm
pacman -Syu
```

## 3. Headless base
```bash
pacman -S --needed openssh sudo
systemctl enable --now sshd
systemctl enable --now systemd-timesyncd     # NTP — accurate broadcast timestamps
# Ethernet: systemd-networkd is in base; the rpi image usually DHCPs eth0 out of the box.
# WiFi (only if needed): pacman -S iwd ; systemctl enable --now iwd ; iwctl to connect.
```

## 4. helio runtime dependencies (the whole list)
```bash
pacman -S --needed \
    alsa-lib \      # cpal audio capture (ALSA-direct)
    alsa-utils \    # arecord / alsamixer — test capture + set input gain
    opus \          # Opus encoding
    libshout \      # Icecast source client (pulls libogg/libvorbis)
    lame            # MP3 fallback mount (optional — drop if Opus-only)
```
That's it for runtime. HTTP/TLS (reqwest+rustls), PNG (image crate), and config (serde) are **pure Rust — no system packages**.

## 5. Building `helio`
**Preferred: cross-compile on your dev machine** (fast), copy the binary over. The Pi then needs only §4's runtime libs.
```bash
# on the dev box:
rustup target add aarch64-unknown-linux-gnu
cargo install cross          # or cargo-zigbuild
cross build --release --target aarch64-unknown-linux-gnu
scp target/aarch64-unknown-linux-gnu/release/heliod alarm@<pi>:
```
**Or build on the Pi** (slower; add toolchain):
```bash
pacman -S --needed rust base-devel clang pkgconf
```

## 6. Run as a service
Install a systemd unit for `heliod` (auto-start, restart-on-crash, journald logs) — extends the "unattended / survives crashes" goal. Control via `helioctl` (HTTP+JSON API) over SSH/LAN.

---

## Audio-in hardware
- **Prototype:** class-compliant **USB line-in** interface (Behringer UMC202HD — 24-bit + gain knob — or UCA202) into a USB-A port. No driver (class-compliant = `snd-usb-audio` binds instantly). Verify: `arecord -l` shows it, `alsamixer` (F6 → pick it) sets capture gain.
- **Product (single unit):** an **ADC HAT** (HiFiBerry ADC-class) inside one enclosure with a panel line-in jack + volume pot + status LED. Enable its device-tree overlay in `/boot/config.txt`. Appears as a normal ALSA capture device → **no daemon change** (interface-agnostic).

## Appliance caveat
Arch ARM is rolling — great for dev / your own unit, and the SOTA-audio knowledge (realtime-privileges, etc.) transfers directly. For **giftable units**, freeze a **pinned image** (build once, don't live-`-Syu` in the field) or base them on Pi OS Lite. See [[broadcast-device-rust-plan]] in agent memory.

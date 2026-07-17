# Heliograph on Arch → Eurorack-style Pi Broadcast Device — Session Handoff

**Date:** 2026-07-16
**Machine:** Arch Linux laptop (kernel `7.0.14-zen1-1-zen`, KDE Plasma **Wayland**/kwin, PipeWire, Intel iGPU + NVIDIA RTX 3050Ti)
**Purpose of this doc:** (1) record everything we fixed to get **heliograph** running/broadcasting on this Arch box, and (2) spec the **goal** — retiring the visualizer and rebuilding it as a small, modular **Rust broadcast appliance** for a Raspberry Pi in a **Eurorack-compatible** enclosure. Written as a handoff for the agent that will build the Rust successor (on a different system).

---

## PART 1 — What we fixed about heliograph on Arch Linux

heliograph = a C++ / openFrameworks 0.12.1 audio-reactive broadcast visualizer. Repo: `/mnt/data/i5hi/heliograph` (standalone clone). Getting it to build, run, and broadcast on this laptop took a chain of environment fixes. **The app source itself is now byte-identical to upstream stock** — all fixes are environment/build/config, not app code.

### 1. Build
- **openFrameworks 0.12.1** downloaded + extracted to `/mnt/data/i5hi/of_v0.12.1_linux64_gcc6_release/` (GitHub release asset `of_v0.12.1_linux64_gcc6_release.tar.gz`).
- Local build config in `app/config.make` (git-tracked; **do not** `git checkout` it away):
  - `OF_ROOT = /mnt/data/i5hi/of_v0.12.1_linux64_gcc6_release`
  - `APPNAME = heliograph` (folder is `app`, so OF would otherwise emit `bin/app`)
- **Dependencies (current Arch gotchas):** OF's `scripts/linux/archlinux/install_dependencies.sh` is stale — `glfw-x11` is now just **`glfw`**, and **`freeimage` is AUR-only** (`yay -S freeimage`). Everything else via pacman.
- Build: `cd app && make -j Release` → `bin/heliograph`. Alias in `~/.zshrc`.

### 2. GLFW crashes on Wayland (the only code patch, in the OF tree)
- Symptom: `ofAppGLFWWindow: 65550: X11: Platform not initialized` → SIGSEGV at launch.
- Cause: OF 0.12.1's window code uses X11-native GLFW calls (`glfwGetX11Display`, `XOpenIM`…) but **GLFW 3.4 auto-selects Wayland**, so those calls return null → crash.
- Fix (in `<OF>/libs/openFrameworks/app/ofAppGLFWWindow.cpp`, before `glfwInit()`):
  ```cpp
  #if defined(GLFW_PLATFORM_X11)
      glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);   // route via XWayland
  #endif
  ```
- **Any OF/GLFW-3.4 app on this Wayland box needs this.**

### 3. Zoom LiveTrak L-8 audio interface — driver + usage
- The L-8 is **NOT USB-Audio-Class compliant** — vendor-specific bulk protocol (512-byte packets); stock `snd_usb_audio` never binds it (`<no driver bound>`). Zoom's driver is Win/Mac only.
- **Driver:** community `sreimers/zoom-l8` — DKMS ALSA module `snd_usb_zoom`:
  ```
  sudo git clone https://github.com/sreimers/zoom-l8.git /usr/src/snd-usb-zoom-0.0.1
  sudo dkms add snd-usb-zoom/0.0.1 && sudo dkms autoinstall
  ```
  Builds against the zen kernel (needs `linux-zen-headers`).
- **48 kHz is mandatory:** the driver only binds USB product id **`0525`**, which the L-8 presents **only at 48 kHz** (SETTING → SYSTEM → SAMPLE RATE → 48 kHz). At other rates it enumerates as `0515` and nothing binds. (SD "sample rate different" warning is harmless.)
- **Correct usage (settled):** treat it as a normal input — activate it and set it as **default input in KDE**; apps capture the default source. **No app-side device code, no PipeWire "disable" hacks.** (We tried forcing raw `hw:L8,0` + a WirePlumber disable rule + app patches; it constantly fought the normal KDE workflow — all reverted.)
- Driver is experimental/fragile (can wedge → `RtApiAlsa` read errors); power-cycle the L-8 to recover.

### 4. Session config (Mac → Linux port)
- `~/.heliograph/session.json` restored from the user's Mac session. Three macOS-specific fields translated for Linux: `inputDevice` (BlackHole → empty/Auto so it follows the KDE default), `recDir` (`/Users/.../Movies` → `""` = `~/Videos`), `imgDir` (macOS path → Linux path).

### 5. Unattended / lid-closed broadcasting (VERIFIED WORKING)
- **No suspend on lid close:** KDE Powerdevil `lidAction=0` on all profiles (was `1`=Sleep). `kwriteconfig6 --file powermanagementprofilesrc --group AC --group HandleButtonEvents --key lidAction 0` (+ Battery/LowBattery). Powerdevil — not logind — is the lid authority here.
- **No idle-suspend during broadcast:** the `heliograph` alias wraps `systemd-inhibit --what=idle:sleep:handle-lid-switch`; Powerdevil honors it. (There is a 15-min AC idle-suspend otherwise.)
- **Verified:** lid closed ~2 min on AC during a live broadcast → stock `BROADCAST alive — uptime=Ns` heartbeat climbed with no gap (180→840 s), 0 reconnects, 0 drops, full 320 kbps. The XWayland render loop keeps running with the panel off.

### 6. Broadcast stutter — root cause (IMPORTANT: it was NOT bitrate)
- Symptom: listener audio stuttered/dropped intermittently; fine on the user's Mac.
- Telemetry proved: **capture fine** (~48k fr/s), **ffmpeg not dying** (0 reconnects), **video fine** (60 fps). The choke was the **TCP upload path** to `radio.stackmate.org` (~**288 ms RTT**, likely intercontinental) intermittently collapsing → heliograph's 8-s buffer overflowed → dropped audio.
- Ruled out: WiFi power-save (off), USB autosuspend (off), snapshot uploads (383 KB, backgrounded), local WiFi (1170 Mbit/s, 0% loss). Linux uses **`cubic`** TCP congestion control, which collapses on lossy long-haul paths where macOS's stack coped.
- **Do not "fix" this by lowering bitrate** — hi-def is a requirement. Real levers: better network path, larger source-side buffer, or (for the successor) a more resilient codec/transport (Opus needs far less bandwidth for the same quality). This directly motivated the Opus decision below.

### 7. Audio stack audit (for future pro-audio tuning)
- PipeWire 1.6.7 + WirePlumber 0.5.15 + pipewire-jack/alsa; zen kernel; 48 kHz clock; CPU governor `performance` — all good.
- **Gap:** user not in `realtime` group, `rtprio` limit 0, no `limits.d` rule → the recurring `RtAudio: NOT running realtime scheduling` warning. Fix = `sudo pacman -S realtime-privileges` + join `realtime` group + re-login (grants `rtprio 98`/`memlock unlimited`). Biggest pro-audio win; not yet applied.

### Hard-won principles (carry these forward)
1. **The app must be interface-agnostic** — it selects an input; it never knows or cares what the hardware is. No device-specific code, ever.
2. **Getting a specific interface working is a SYSTEM concern** (install driver, select it in the OS), never an app concern.
3. **Prefer class-compliant audio hardware** to avoid the L-8's driver saga.
4. **Stutter over a WAN is a transport/network problem, not a quality problem** — don't sacrifice audio quality to paper over it.

---

## PART 2 — The goal: Eurorack-style Pi broadcast device (Rust)

**Concept:** retire heliograph's GL audio-visualizer; build a successor focused on being a **broadcast client** — a small, modular, shareable "concept device." A headless Raspberry Pi appliance that anyone (non-expert friends) can use to easily live-broadcast audio from **any** input, in a **Eurorack-compatible** enclosure. The "visual" reduces to: pick an image → scale / opacity / color-gradient / x-y pan → periodic PNG snapshot → HTTP POST to the server (no GL, no GPU, headless).

### Hardware
- **Board:** target **Raspberry Pi Zero 2 W** (aarch64, WiFi, tiny, ~1–2 W) — fits a slim Eurorack module.
- **Power:** Eurorack bus ±12 V / +5 V; feed the Pi from +5 V rail or a +12 V→5 V buck.
- **Audio-in** (Pi has no native input): either a **class-compliant USB ADC** from a line/headphone jack (simplest, stays interface-agnostic), or an **I2S codec** (PCM1808/WM8782) with a front-end op-amp conditioning ~10 Vpp modular level → line level (Eurorack-native, custom PCB, exposed as an ALSA device via device-tree overlay). Either way the daemon just sees "the selected ALSA input."
- **Front panel (later):** OLED + encoder = another client of the daemon's control API.

### Language: **Rust** (not Zig)
- Rust wins here for mature ecosystem (audio/HTTP/TLS/image/config crates), memory safety for an unattended network daemon, and adequate Pi cross-compile. Zig's only real edge (cross-compile + C interop) is outweighed for this library-heavy, ship-and-trust networked client.

### Architecture: daemon + thin clients
```
Pi:  heliod (systemd daemon)  ── control API (HTTP+JSON on loopback, opt-in LAN+token) ──┐
     ├─ audio: capture → encode → stream                                                 │
     ├─ visual: image + transforms → PNG snapshot → POST                                 ├─ helioctl (CLI)  now
     └─ state: registration / session / channel / on-air                                 ├─ GUI            later
                                                                                          └─ web UI         optional
```
- Daemon owns all state + pipelines; runs the broadcast regardless of any UI attached (extends the unattended goal). CLI/GUI/web are stateless frontends over one API. systemd service = autostart/restart/journald.

### Modular core — trait-based swappable backends (a hard requirement)
- `AudioInput`  → impl: **`cpal`** (ALSA/PipeWire; interface-agnostic).
- `Encoder`     → impl: **Opus** (primary), MP3 (legacy).
- `Transport`   → impl: **Icecast** (now); SRT / MoQ (`moq-lite`) / WHIP (later) drop in without a rewrite.
- Plus `tokio` + `axum` (control API), `clap` (CLI), `serde`+`toml/json` (config). Cross-compile via `cross` / `cargo-zigbuild` to aarch64.

### Streaming decision: **Opus over Icecast** (research-backed, 2026)
Priorities were: (1) hi-fi quality, (2) reliability over flaky ~280 ms WiFi, (3) latency *not* critical, (4) many listeners, (5) Rust-on-Pi feasibility, (6) modularity.
- **Codec → Opus.** ~**192–256 kbps Opus is transparent** — better quality-per-bit than 320 kbps MP3 **and** ~30% less bandwidth (directly helps the flaky-WiFi reliability problem from Part 1 §6). Serve **Opus primary + MP3 fallback** multi-mount for broad browser `<audio>` support.
- **Source→server:** Icecast source (Ogg/Opus) via `libshout`, wrapped in a **generous reconnecting buffer** (bigger than heliograph's 8 s). Add **SRT** as a reliability upgrade for bad links (swappable `Transport`).
- **Server→listeners:** Icecast serving Opus + MP3 — HTTP/CDN-scalable, dead simple for many friends.
- **Not now (but design for as swappable transports):** **MoQ** (Media over QUIC) is the exciting future but early — IETF drafts still incompatible, only emerging production in 2026; needs QUIC relays. **WebRTC/WHIP-WHEP** gives sub-second latency but needs an SFU — overkill for radio where latency isn't the priority.

### UX fix to carry over
- **Register-first state machine:** Unregistered → Registered → Configuring → On Air.
- **Artist identity entered ONCE** on the register page (heliograph wrongly asks in both REGISTER and SESSION). Session/Channel page only edits mutable per-broadcast fields (title, note, channel, image + transforms); artist name is read-only there.
- **Reuse heliograph's proven server protocol + `session.json` schema** against `radio.stackmate.org` so the server needs no changes.

### Migration path
1. Rust daemon at parity (Icecast + MP3).
2. Add Opus encode + Opus mount (quality + reliability win).
3. Add SRT source-leg backend for bad links.
4. Evaluate MoQ distribution when drafts stabilize (plug-in via the `Transport` trait).

---

## References
- Driver: `sreimers/zoom-l8` — https://github.com/sreimers/zoom-l8
- MoQ: Cloudflare https://blog.cloudflare.com/moq/ · https://moq.dev/ · IETF draft-ietf-moq-transport
- WHIP RFC 9725 (WHEP still draft) · Opus over Icecast (Radio Mast, icecast.org) · SRT/RIST are contribution-only (need transcode to reach browsers)
- Rust: `cpal` (RustAudio), `libopus` bindings, `libshout-sys`, `quinn`, `moq-lite`/`hang`, `str0m`/`webrtc-rs`
- Detailed notes live in this machine's agent memory: `laptop-audio-stack`, `zoom-l8-arch-linux`, `glfw-wayland-x11-of`, `heliograph-linux-setup`, `laptop-lid-suspend-broadcast`, `arch-sudo-faillock`, `broadcast-device-rust-plan`.

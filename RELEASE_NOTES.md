# heliograph v0.3.0

An audio-reactive broadcast visualizer. It turns whatever audio is playing into a live visualizer,
**records** it to a clean 1440p MP4, and can **broadcast** it live (audio + a visual feed) to a station
that listeners tune into from their browser. New in this release: **publish your recordings as a
gallery of collections**, a headless **Raspberry-Pi broadcast appliance**, a **command-line tool**, and
**Windows support**.

## New in 0.3.0

- **📼 Publish your recordings — the Gallery.** A new **PUBLISH** tab (`S → PUBLISH`): type a collection
  name, hit publish, and heliograph uploads the audio from every recording in your folder as tracks in a
  named collection on your station. Listeners get a **Gallery** on the web player where they browse
  collections, play tracks on demand (seek, next/prev, download), and the station's global player rolls
  through the whole archive. Uploads reuse the app's existing `curl`/`ffmpeg` — no new dependencies.
- **🛰 heliod — the broadcast appliance** (`lite/`). A tiny Rust daemon that turns a Raspberry Pi (or any
  Linux box) into a standalone "broadcast box": captures line-in audio, encodes to **Opus**, streams to
  **Icecast**, and pushes live metadata + a single image — no laptop in the loop. systemd unit included;
  auto-resumes on reboot.
- **⌨️ helio — the command-line tool** (`lite/`). Control a heliod device and manage the gallery from a
  terminal: `helio collection upload <folder> --name "…"`, plus `ls / create / add / rm / edit / cover`.
  Cross-platform, tiny, size-optimized.
- **🪟 Windows support.** The shell-out layer (all networking/encoding runs through `curl`/`ffmpeg`) is now
  cross-platform — argument quoting and the null device work under `cmd.exe`. Build guide: **[`app/WINDOWS.md`](app/WINDOWS.md)**.
- **Backward compatible.** Existing stations need no changes — metadata + images still go over the same
  REST snapshot endpoint.

_(0.2.0 added IMAGE mode + the new app icon; 0.1.0 was the first public build.)_

## What's in this release

| Component | What it is |
|-----------|-----------|
| **heliograph** (`app/`) | The desktop visualizer — record, broadcast, and now **publish** recordings. macOS / Linux / Windows. |
| **heliod** (`lite/crates/heliod`) | The headless broadcast appliance (Rust) for a Pi/Linux box. |
| **helio** (`lite/crates/helio-cli`) | The command-line control + gallery-upload tool (Rust). |

## Downloads

| OS / target | Asset | Notes |
|----|-------|-------|
| macOS (Apple Silicon) | `heliograph-macos.zip` | ad-hoc signed (not notarized) — right-click → Open on first launch |
| Linux (x86-64) | `heliograph-linux.tar.gz` | see INSTALL for the runtime `apt` line |
| Windows (x64) | `heliograph-windows.zip` | keep the `.dll`s + `data/` next to the `.exe`; see **app/WINDOWS.md** |
| `helio` CLI | `helio-<os>-<arch>` | macOS / Linux / Windows single binary |
| `heliod` appliance | `heliod-<arch>` (Linux, incl. aarch64) | install as a systemd service on the box |

**Install & setup for every platform: [INSTALL.md](INSTALL.md).** Recording, broadcasting, and publishing
require **ffmpeg** on PATH; `curl` ships with modern macOS/Linux/Windows.

## What it does

- **Visualize any audio** — pick an input in `S → ROUTING`; heliograph reacts in real time over a
  deep-space HUD. Switch the visual **Type** (RADIAL / GRID / IMAGE) from the control panel (`C`); `TAB`
  toggles RADIAL ↔ GRID. To broadcast your computer's own sound, route it through a virtual audio channel
  (BlackHole / PulseAudio monitor / VB-CABLE — see INSTALL §6).
- **Record** (`R`) — a single 1440p MP4 to your Videos folder. No account needed.
- **Broadcast** (`B`) — go live to a heliod station (audio + a visual feed). **Independent of recording.**
  Requires a **single-use registration token** from the station admin.
- **Publish** (`S → PUBLISH`) — turn your recordings into a **collection** listeners can browse and play
  from the station's Gallery. Same auth as broadcasting; safe to re-run (same track name overwrites).
- **Brand your channel** (`S → CHANNEL`) — channel name, UI font, accent colour, and **donation addresses**
  (Lightning / Bitcoin `bc1` / Liquid `lq1`) shown on the listener's player and their gallery collections.
  Address fields stay locked until you confirm **Wallet Backed Up = YES**.
- **Screen-only meters/HUD** — the control panels, the `V` level meter, the IMAGE carousel, and the
  REC/ON-AIR indicators are drawn to the screen only and are **never** part of the recording or broadcast.

## Shortcuts

`C` panels · `S` settings (SESSION / ROUTING / REGISTER / CHANNEL / **PUBLISH**) · `R` record ·
`B` broadcast · `V` level meter · `M` mode · `TAB` layout (RADIAL/GRID) · `←/→` step image (IMAGE) ·
`U` HUD · `T` telemetry · `F` fullscreen · `P` screenshot · `H` help · `X` reset

## Broadcast servers

The broadcast server stack (Icecast + snapshot/collections API + gateway) will be **open-sourced soon**,
so anyone can run their own station + gallery and point heliograph at it. For now there is **one live
station: `radio.stackmate.org`** — the default Registration Server. To broadcast/publish there, ask the
admin for a single-use registration token (see [INSTALL.md](INSTALL.md) §4). Change the server anytime in
`S → REGISTER`.

## Notes

- No credentials or personal data ship with the app — only the public station URL as the default server.
  Your settings and recordings live in `~/.heliograph/`. Images stay wherever you added them from.
- Built on **openFrameworks 0.12.1** (desktop); `heliod`/`helio` are Rust. Binaries are produced by CI.

## macOS first-launch

Not notarized, so macOS Gatekeeper will warn. Right-click the app → **Open** → **Open**. If it still
refuses: `xattr -dr com.apple.quarantine /path/to/heliograph.app`.

---

## Previous releases

### v0.2.0
- **🖼 IMAGE mode** — a new visual **Type** alongside RADIAL and GRID: load a folder of your own images
  and blend them into the deep-space scene. GLOW (additive) / SOFT (feathered alpha) blend modes;
  opacity, scale, pan X/Y, rotate, brightness, accent tint, Ken-Burns drift, crossfade, and an
  audio-reactive pulse. Screen-only carousel (lock-to-click or Auto Cycle); **only the image itself is
  ever recorded or broadcast**.
- **New app icon** — the heliograph mark (greyscale circle + dot) replaces the generic openFrameworks
  icon on macOS/Windows/Linux.

### v0.1.0
- First public build — visualize any audio over a deep-space HUD, **record** to a clean 1440p MP4, and
  **broadcast** live (audio + visual feed) to a heliod station that listeners tune into from the browser.
  Channel branding (name / font / accent / donation addresses) and screen-only meters/HUD.

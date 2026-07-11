# heliograph v0.1.0

An audio-reactive broadcast visualizer. It turns whatever audio is playing into a live visualizer,
**records** it to a clean 1440p MP4, and can **broadcast** it live (audio + a visual feed) to a station
that listeners tune into from their browser.

## Downloads

| OS | Asset | Notes |
|----|-------|-------|
| macOS (Apple Silicon) | `heliograph-macos.zip` | ad-hoc signed (not notarized) — right-click → Open on first launch |
| Linux (x86-64) | `heliograph-linux.tar.gz` | see INSTALL for the runtime `apt` line |
| Windows (x64) | `heliograph-windows.zip` | keep the `.dll`s + `data/` next to the `.exe` |

**Install & setup for every platform: [INSTALL.md](INSTALL.md).** Requires **ffmpeg** for recording/broadcasting.

## What it does

- **Visualize any audio** — pick an input in `S → ROUTING`; heliograph reacts in real time over a
  deep-space HUD. To broadcast your computer's own sound, route it through a virtual audio channel
  (BlackHole / PulseAudio monitor / VB-CABLE — see INSTALL §6).
- **Record** (`R`) — a single 1440p MP4 to your Videos folder. No account needed.
- **Broadcast** (`B`) — go live to a heliod station (audio + a visual feed). Broadcasting also records a
  local copy. Requires a **single-use registration token** from the station admin.
- **Brand your channel** (`S → CHANNEL`) — channel name, UI font, accent colour, and **donation
  addresses** (Lightning / Bitcoin `bc1` / Liquid `lq1`) that appear on the listener's player. Address
  fields stay locked until you confirm **Wallet Backed Up = YES**.
- **Screen-only meters/HUD** — the control panels, the `V` level meter, and the REC/ON-AIR indicators are
  drawn to the screen only and are **never** part of the recording or broadcast.

## Shortcuts

`C` panels · `S` settings · `R` record · `B` broadcast (+record) · `V` level meter · `M` mode ·
`TAB` layout · `U` HUD · `T` telemetry · `F` fullscreen · `P` screenshot · `H` help · `X` reset

## Broadcast servers

The broadcast server stack (Icecast + snapshot API + gateway) will be **open-sourced soon**, so anyone
can run their own station and point heliograph at it. For now there is **one live station:
`radio.stackmate.org`** — it's the default Registration Server. To broadcast there, ask the admin for a
single-use registration token (see [INSTALL.md](INSTALL.md) §4). You can change the server anytime in
`S → REGISTER` once you're running your own.

## Notes

- No credentials or personal data ship with the app — only the public station URL as the default server.
  Your settings and recordings live in `~/.heliograph/`.
- Built on **openFrameworks 0.12.1**. macOS/Linux/Windows binaries are produced by CI.

## macOS first-launch

Not notarized, so macOS Gatekeeper will warn. Right-click the app → **Open** → **Open**. If it still
refuses: `xattr -dr com.apple.quarantine /path/to/heliograph.app`.

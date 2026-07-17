# heliograph lite — `heliod` + `helio-cli`

A small, headless **broadcast appliance**: capture audio from any input, encode to
**Opus**, stream to **Icecast**, and push channel metadata to the backend — driven by a
tiny CLI. Built to run as a standalone daemon or a **Eurorack-style Raspberry Pi**
module. No GL, no GPU: the entire "visual" is one still image with transforms, sent as a
periodic PNG snapshot.

This is the focused successor to the openFrameworks `heliograph` visualizer. That project
is unchanged; all new work lives here.

## Design

```
                    ┌────────────────────── heliod (daemon) ──────────────────────┐
  audio in ──▶ AudioInput(cpal) ──▶ Encoder(Opus) ──▶ Transport(Icecast) ──▶ Icecast ──▶ listeners
                    │                                                              │
  image + xforms ──▶ snapshot (PNG + X-Transmission) ──── PUT ──────────────────▶ snapshot-api
                    │                                                              │
  metadata ────────▶ metalink (WebSocket, fast-path) ──── wss ─────────────────▶ /ingest
                    └──────────────────────────────────────────────────────────────┘
                                        ▲  HTTP control API (loopback)
                                        │
                                   helio-cli  (register · start/stop · meta · image · device)
```

- **Daemon owns all state + pipelines** and runs regardless of any UI (systemd
  autostart/restart). `helio-cli` — and later a GUI / web UI / front-panel encoder — are
  stateless clients over the loopback HTTP API.
- **Everything swappable is a trait** (`helio-core::pipeline`): `AudioInput`, `Encoder`,
  `Transport`. Moving Opus→MP3 or Icecast→SRT/MoQ/WHIP is a new impl, not a rewrite.
- **Interface-agnostic + hotplug-aware.** The daemon selects an input by name, or follows
  the system default (empty selection — ideal for the Pi's aux-in). It re-scans devices
  every ~2 s; USB interfaces coming/going trigger a transparent re-open, and the broadcast
  streams **silence** in the gap so listeners never drop.
- **Two metadata paths, by design.** The WebSocket (`metalink`) is the low-latency
  fast-path (title / presence / level land in ~1 s). The periodic snapshot PUT carries a
  copy of the metadata as the **guaranteed floor** and the liveness signal. Metadata is
  best-effort and never blocks or stalls the audio.
- **Reconnect policy** (from hard-won heliograph experience): retry forever while ON AIR;
  give up **only** on auth failure.

## Crates

| Crate | What |
|---|---|
| `helio-core` | Session schema (server-compatible), `Transmission` metadata, image renderer, pipeline traits, shared API DTOs. Dependency-light, platform-neutral. |
| `heliod` | The daemon: cpal capture, Opus+Ogg encoder, pure-Rust Icecast source client, WS metalink, snapshot uploader, axum control API. |
| `helio-cli` | `helio` — the operator CLI. |

## Build & run

```bash
cargo build --release            # → target/release/{heliod,helio}
RUST_LOG=heliod=info ./target/release/heliod &
```

State lives in `~/.heliod` (`HELIO_HOME` to override); the API binds `127.0.0.1:4777`
(`HELIO_BIND`). `helio` talks to `$HELIO_URL` (default `http://127.0.0.1:4777`).

```bash
helio register --invite ABCD-… --artist amo_eba   # one-time
helio devices                                      # list inputs
helio device "ZOOM L-8"                             # or: helio device default
helio start                                         # go on air
helio meta --title "Mirepoix" --note "Holding Station"
helio presence away                                 # ON DECK ⇄ AWAY (instant via WS)
helio image --path ~/pic.jpg --scale 1.2 --pan-x -0.2 --opacity 0.9 --blur 3
helio status
helio stop
```

## Raspberry Pi (aarch64)

Static libopus is vendored & built by `audiopus_sys`, so there's no system audio-codec
dependency to chase.

```bash
rustup target add aarch64-unknown-linux-gnu
cargo build --release --target aarch64-unknown-linux-gnu   # via `cross` for easy C toolchain
# install heliod → /usr/local/bin, crates/heliod/heliod.service → /etc/systemd/system
```

On the Pi, leave the input selection empty so it follows the aux-in default; plug a
class-compliant USB ADC and it's hot-detected.

## Status

Verified on macOS: workspace compiles clean; daemon serves the control API; device
enumeration + hotplug selection; CLI register/status/devices/meta/image/presence/start/stop;
session persistence; Opus encode + Ogg muxing; graceful shutdown.

Needs on-device verification (requires a registered channel / backend): live Opus→Icecast
playback end-to-end, the snapshot PUT round-trip, and the `/ingest` WS metadata path.

## Notes

- **Opus replaces MP3.** Ogg/Opus plays natively in Chrome/Firefox/Android; Safari is
  spotty. An MP3 fallback mount can be added later as a second `Transport`/`Encoder` (the
  architecture already supports it).
- Metadata schema is unchanged from heliograph, so the backend needs no changes to keep
  working — the WS `/ingest` endpoint is an additive fast-path.

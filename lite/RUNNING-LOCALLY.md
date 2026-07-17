# Running the whole stack locally

End-to-end on one machine: the **server** (Icecast + snapshot-api + Caddy gateway), the
**`heliod`** appliance, and the **web client** — with an Opus broadcast and live metadata.

## Prerequisites
- Rust (stable) + Docker, and (optional, for verifying) `ffmpeg`.
- Repos side by side: `~/Music/heliograph` (this repo, `lite/` here), `~/Music/heliod`
  (server), `~/Music/helio-client` (web player).

## 0. Build the binaries + aliases (once)
```bash
cd ~/Music/heliograph/lite
cargo build --release          # → target/release/{heliod,helio}
cargo build                    # → target/debug/{heliod,helio}  (for x-* aliases)
```
Aliases (added to `~/.zshrc`): `helio` / `heliod` = release, `x-helio` / `x-heliod` =
debug. `source ~/.zshrc` to load them.

## 1. Start the server stack
```bash
cd ~/Music/heliod
./heliod up local              # builds + starts icecast + snapshot-api + caddy (detached)
./heliod status local          # sanity: containers up, Icecast reachable
```
Ports: **8090** gateway (Caddy), **8091** snapshot-api (direct), **8000** Icecast.
The gateway routes `/register /channels /snapshot/* /stream/* /ingest /board /time` →
snapshot-api/Icecast, and serves the client at `/`.

## 2. Mint an invite
```bash
./heliod invite create local   # prints a single-use code, e.g. 473F-665C-FEB2
```

## 3. Run the daemon
```bash
export HELIO_HOME=~/.heliod-local     # keep local state separate from prod
heliod &                              # control API on 127.0.0.1:4777
```
Logs go to stdout; `RUST_LOG=heliod=info` for detail.

## 4. Register + configure
```bash
helio register --invite 473F-665C-FEB2 --artist lite_dj --server http://localhost:8090
helio devices                         # list inputs
helio device default                  # or:  helio device "ZOOM L-8"
helio meta --title "Soundcheck" --note "opus test"
helio image --path ~/pic.jpg --scale 1.1 --opacity 0.9    # optional
```

## 5. Go on air
```bash
helio start
helio status                          # state OnAir, uptime climbing
```

## 6. Serve + open the web client
The client detects it's on a non-gateway port and targets `http://localhost:8090`
cross-origin (the local Caddyfile allows CORS):
```bash
cd ~/Music/helio-client/dist
python3 -m http.server 5173 --bind 127.0.0.1
# open http://localhost:5173  →  tune into "lite_dj"  →  Opus plays, metadata is live
```

## Verify from the shell (no browser needed)
```bash
CH=$(python3 -c "import json;print(json.load(open('$HELIO_HOME/session.json'))['registration']['channelId'])")

# audio: the stream decodes as Opus (this is the browser's <audio> source)
ffprobe -v error -show_entries stream=codec_name,sample_rate,channels \
  -of default=noprint_wrappers=1 "http://localhost:8090/stream/$CH"
# → codec_name=opus  sample_rate=48000  channels=2

# discovery + metadata the client polls
curl -s "http://localhost:8090/channels" | python3 -m json.tool
curl -s "http://localhost:8090/snapshot/$CH/meta.json" | python3 -m json.tool

# WS metadata fast-path: change the title, it reflects in ~1s (not the 12s snapshot)
helio meta --title "live update"; sleep 1
curl -s "http://localhost:8090/snapshot/$CH/meta.json" | grep -o '"title":"[^"]*"'
```

## Teardown
```bash
helio stop
kill %1                # or: pkill -f target/release/heliod
# stop the client http.server (Ctrl-C or kill its PID)
cd ~/Music/heliod && ./heliod down local
```

## Notes
- **Debug vs release:** use `x-heliod` / `x-helio` (debug) for quick iteration; they read the
  same `HELIO_HOME`. Release binaries are tiny (heliod ~2.7 MB) and Pi-ready.
- **Opus in the browser:** plays natively in Chrome/Firefox/Android; Safari support for
  Ogg/Opus is spotty (add an MP3 fallback mount later if you need Safari).
- **Metadata paths:** the WS `/ingest` is the ~1 s fast-path; the 12 s snapshot PUT is the
  liveness signal + metadata floor. Both write the channel's meta atomically.

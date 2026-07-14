# Installing heliograph

**heliograph** turns whatever audio is playing into a live visualizer, **records** it to a clean
1440p MP4, and can **broadcast** it live (audio + a visual feed) to a station that listeners tune
into from their browser.

There's nothing to compile — grab the build for your platform from the
[**Releases**](../../releases) page and run it.

---

## 1. Download

On the [Releases](../../releases) page, open the latest release and download the asset for your OS:

| OS | Asset |
|----|-------|
| macOS (Apple Silicon) | `heliograph-macos.zip` |
| Linux (x86-64) | `heliograph-linux.tar.gz` |
| Windows (x64) | `heliograph-windows.zip` |

> Keep the `data/` folder that ships next to the app — it holds the fonts and factory presets.
> Your personal settings/recordings live in `~/.heliograph/` and are created on first launch.

---

## 2. Install & run

### macOS
1. Unzip `heliograph-macos.zip` → you get **`heliograph.app`**. Drag it to **Applications** (optional).
2. The app is ad-hoc signed but **not notarized**, so the first launch needs one extra step:
   - **Right-click** `heliograph.app` → **Open** → **Open** again in the dialog.
   - If macOS still refuses ("damaged / can't be opened"), clear the quarantine flag in Terminal:
     ```bash
     xattr -dr com.apple.quarantine /path/to/heliograph.app
     ```
3. After the first open it launches normally by double-click.

### Linux
1. Extract:
   ```bash
   tar -xzf heliograph-linux.tar.gz && cd heliograph
   ```
2. Install the runtime libraries openFrameworks needs (Debian/Ubuntu):
   ```bash
   sudo apt update
   sudo apt install -y libglfw3 libfreeimage3 libassimp5 liburiparser1 \
     libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 libgtk-3-0 libopenal1 libsndfile1 ffmpeg
   ```
3. Run it (keep `heliograph` and its `data/` folder side by side):
   ```bash
   ./heliograph
   ```

### Windows
1. Extract `heliograph-windows.zip`.
2. Run **`heliograph.exe`** (keep the `.dll` files and the `data/` folder next to it).
3. SmartScreen may warn on an unsigned app → **More info → Run anyway**.

> **ffmpeg** is required for recording and broadcasting. It's bundled on Windows and pulled in by the
> apt line on Linux. On macOS, install it with [Homebrew](https://brew.sh): `brew install ffmpeg`.

---

## 3. First-run setup

Open heliograph and press **`H`** any time for the full key list. The essentials:

| Key | Action |
|-----|--------|
| **C** | show / hide the control panels |
| **S** | settings (session · routing · register · channel) |
| **R** | **record** locally (MP4) |
| **B** | **broadcast** live (independent of record) — needs a server account |
| **V** | on-screen audio level meter (never recorded) |
| **M** | cycle visual mode (orbit / vehicle / platform / helix / **image**) |
| **F** | fullscreen |
| **P** | save a screenshot |

**Pick your audio input:** press **`S`** → **ROUTING** → choose your **Input Device** and **Channels**
(the SIGNAL meter jumps when audio is coming through). heliograph visualizes whatever is on that input.

**Record:** press **`R`**. Recordings are saved to your Videos folder — no account needed.

**Image mode:** press **`M`** to cycle to **IMAGE**. Drop images (png/jpg) into **`~/.heliograph/images/`**,
switch into IMAGE mode to load them, and it plays a slow crossfading slideshow that blends into the
deep-space background. **`←` / `→`** step between images; the control panel (`C`) has blend (GLOW/SOFT),
feather, ken-burns, opacity, scale/pan/rotate, brightness, tint, hold/fade timing, and audio-reactivity.

---

## 4. Broadcasting — ask an admin for a registration token

Broadcasting to a station requires an **account on that station's server**. Accounts are created with a
**single-use broadcast registration token (invite code)** that only the station admin can issue.

1. **Ask the station admin** for a broadcast registration token. Tell them the **artist name** you want —
   they mint a one-time code tied to it.
2. In heliograph press **`S`** → **REGISTER** and fill in:
   - **Registration Server** — where you broadcast. Defaults to the public station `https://radio.stackmate.org`; change it to your own heliod stack once the server is open-sourced.
   - **Invite Code** — the token the admin gave you (you can paste it: click the field, then ⌘V / Ctrl-V).
   - **Artist Name** — your name (this becomes your channel; it locks once registered).
3. Click **REGISTER**. On success it shows *"Registered as …"* — heliograph now holds your broadcast
   config; you never type raw server passwords.
4. Press **`B`** to go live. Your channel appears on the station's web player for listeners to tune in.

> Each token is **single-use**. If registration fails with "invalid/used code", ask the admin for a new one.
> A station admin issues codes with `./heliod invite create` on the server (see the server repo).

---

## 5. Brand your channel (optional)

Press **`S`** → **CHANNEL** to set how your channel looks on the listener's player:

- **Channel Name**, **Font** (live preview), and **Accent** colour.
- **Donations** — advertise a **Lightning**, **Bitcoin (bc1)**, and/or **Liquid (lq1)** address. As a
  safety measure the address fields stay greyed out until you tick **Wallet Backed Up = YES**. New to
  Bitcoin? You can set up a wallet at [wallet.bullbitcoin.com](https://wallet.bullbitcoin.com).

**SAVE** applies your branding to your live channel within a few seconds.

---

## 6. Broadcast your laptop's audio (virtual audio channel)

heliograph visualizes an **input** device (a mic, an interface, etc.). To broadcast **whatever is playing
on your computer** — Spotify, a DAW, a browser — you route your system *output* into a **virtual audio
channel** (a "loopback" device) and point heliograph's input at it. **This is the easiest way to just
broadcast your laptop output**, and it's the same idea on every platform:

> **system output → virtual channel → heliograph input**

Pick your OS below. Afterwards, in heliograph do `S → ROUTING → Input Device → <the virtual device>`,
Channels **1-2**, and press **`B`** to go live.

### macOS — BlackHole

1. Install **[BlackHole 2ch](https://existential.audio/blackhole/)** (free).
2. **System Settings → Sound → Output → BlackHole 2ch.** All system audio now flows into BlackHole.
3. heliograph: `S → ROUTING → Input Device → **BlackHole 2ch**`, Channels **1-2**. Play something — the
   SIGNAL meter should react. (No signal? Grant Microphone permission — see Troubleshooting.)
4. **To also hear the audio yourself:** open **Audio MIDI Setup**, click **+ → Create Multi-Output
   Device**, tick **both** BlackHole 2ch **and** your speakers/headphones, then set that Multi-Output
   Device as the system Output instead of BlackHole alone.

### Linux — PulseAudio / PipeWire monitor (no install needed)

Linux already exposes a loopback of every output: the **monitor source** of your sink. Nothing to install.

1. Just launch heliograph and open `S → ROUTING → Input Device`. Look for an entry named like
   **"Monitor of <your output>"** (e.g. *Monitor of Built-in Audio Analog Stereo*) and select it. Play
   audio → the meter reacts. You keep hearing everything normally — the monitor is a passive tap.
2. If no monitor appears in the list, expose/select it, then click **REFRESH DEVICE LIST**:
   - **PipeWire** (most modern distros): use `pavucontrol` → **Recording** tab, and while heliograph is
     capturing, set its stream's source to **Monitor of …**. Or `wpctl status` to find the monitor.
   - **PulseAudio**: `pactl list sources short` lists the `*.monitor` sources; `pavucontrol` lets you
     pick it per-app. `pactl load-module module-loopback` can also bridge devices if needed.

### Windows — VB-CABLE (or Stereo Mix)

1. Install **[VB-CABLE](https://vb-audio.com/Cable/)** (free virtual audio cable).
2. **Settings → System → Sound → Output → CABLE Input (VB-Audio Virtual Cable).** System audio now flows
   into the cable.
3. heliograph: `S → ROUTING → Input Device → **CABLE Output (VB-Audio Virtual Cable)**`, Channels **1-2**.
4. **To also hear it yourself:** Sound → **More sound settings → Recording → CABLE Output → Properties →
   Listen → "Listen to this device" → Playback through your speakers**. (Alternatively some sound cards
   expose **"Stereo Mix"** as a recording device — enable it under Recording and select it in heliograph;
   no VB-CABLE needed.)

---

## Troubleshooting

- **"You need an account on this server to broadcast"** — you haven't registered. Do step 4 first.
- **macOS won't open the app** — see the quarantine step in §2.

### No signal on the SIGNAL meter (macOS)

Work through these in order:

1. **Grant microphone permission.** heliograph captures audio as an input device, so macOS requires
   Microphone access — **without it every input reads silence**, virtual devices included.
   Open **System Settings → Privacy & Security → Microphone** and enable **heliograph**. If it isn't
   listed, force a fresh prompt in Terminal, then relaunch the app:
   ```bash
   tccutil reset Microphone cc.openFrameworks.heliograph
   ```
2. **Select the right input.** `S → ROUTING → Input Device` — pick the device carrying your sound, then
   cycle **Channels** until the meter jumps. If a device you just created/plugged in isn't listed, click
   **REFRESH DEVICE LIST**.
3. **Broadcasting your laptop's own audio?** You need a virtual audio channel — see
   [§6](#6-broadcast-your-laptops-audio-virtual-audio-channel).

### No signal (Linux / Windows)

- **Linux** — pick the **"Monitor of <your output>"** source in `S → ROUTING`; if it's missing, expose it
  via `pavucontrol`/`pactl` (see §6). No mic permission needed.
- **Windows** — if a virtual cable's output isn't listed, reopen `S → ROUTING` after installing it and
  click **REFRESH DEVICE LIST**; confirm the app has Microphone access under **Settings → Privacy →
  Microphone**.

# heliograph — the app

> Built on top of **visualSynthesizerOrigins** — the visualSynth matrix/grid engine from
> [i5hi/openFrameworksProjects](https://github.com/i5hi/openFrameworksProjects/tree/master/visualSynthesizerOrigins).

An audio-reactive broadcast visualizer (openFrameworks 0.12.1, C++). A ring / grid /
double-helix **VISUALIZER** over deep space with a branded **HUD**, designed to be recorded
for YouTube. Audio comes from whatever is playing on your system (via a loopback device),
so it reacts to Bitwig, a DAW, a browser tab — anything.

Works on **macOS, Linux and Windows**. The source in `src/` is portable; only the
*build project* and the *audio loopback* differ per platform.

---

## 1. Install dependencies

| | what | how |
|---|------|-----|
| **openFrameworks 0.12.1** | the C++ creative-coding framework this is built on | download from <https://openframeworks.cc/download/> and unzip somewhere (that folder is your `<OF>`) |
| **a C++ toolchain** | compiler + IDE | macOS: **Xcode** (+ `xcode-select --install`) · Linux: `g++` + `make` · Windows: **Visual Studio** (Desktop C++) |
| **ffmpeg** | encodes the recordings | macOS: `brew install ffmpeg` · Linux: `sudo apt install ffmpeg` · Windows: grab a build from <https://www.gyan.dev/ffmpeg/builds/> and add `ffmpeg.exe` to your **PATH** |
| **a loopback audio device** | feeds your system/DAW sound into the app | see §3 (BlackHole / VB-Cable / PulseAudio monitor) |

Fonts ship in `bin/data/fonts/`. No other assets are required.

---

## 2. Build

Put this `app/` folder inside your openFrameworks apps directory:
`<OF>/apps/myApps/heliograph/`  (so the project can find the framework).
> Prefer to keep it elsewhere? Open `Project.xcconfig` and set `OF_PATH` to your `<OF>` folder (an absolute path works).

### macOS (Xcode)
```bash
cd <OF>/apps/myApps/heliograph/app
xcodebuild -project heliograph.xcodeproj -target heliograph -configuration Release build
# → builds bin/heliograph.app
```
(or open `heliograph.xcodeproj` in Xcode and press ⌘B.)

### Linux (Makefile)
```bash
cd <OF>/apps/myApps/heliograph/app
<OF>/projectGenerator -o<OF> .      # generate a Linux Makefile from src/
make -j ReleaseRun                  # build + run  (or: make && make run)
```
First time only, install the oF Linux deps: `<OF>/scripts/linux/<distro>/install_dependencies.sh`.

### Windows (Visual Studio)
1. Run `projectGenerator-vs.exe`, point it at this `app/` folder, click **Update**.
2. Open the generated `.sln`, choose **Release / x64**, build, run.

> **Why regenerate on Linux/Windows?** GPUs, codecs and build files differ per platform. The
> code already picks the right pieces at compile time (`#ifdef`): VideoToolbox + Homebrew ffmpeg
> on macOS; `libx264` + PATH ffmpeg and `_popen` on Linux/Windows.

---

## 3. Audio routing (feed system/DAW audio into the app)

The app captures from a **loopback input** — a virtual device that exposes your *output* as an
*input*. It auto-selects one by name (BlackHole / VB-Cable / VoiceMeeter / PulseAudio monitor /
JACK / loopback); otherwise it falls back to the first input. The HUD's **SIGNAL** reads
`NONE` (no audio) · `AUDITION` (audio in) · `LIVE` (audio + recording).

- **macOS — BlackHole:** `brew install blackhole-2ch`, then in **Audio MIDI Setup** create a
  **Multi-Output Device** ticking **BlackHole 2ch** *and* your speakers (so you still hear it),
  and set it as your system/DAW output.
- **Linux — PulseAudio/PipeWire:** every output has a `.monitor` source — pick it in `pavucontrol`
  (Recording tab), or the app matches a device containing "monitor". JACK: wire output → capture ports.
- **Windows — VB-CABLE:** install it (free), set **CABLE Input** as playback, app captures **CABLE Output**;
  use VoiceMeeter to also monitor.

---

## 4. Run

```bash
open bin/heliograph.app          # macOS  (launching the .app gives it its own Microphone
                                 #         permission identity — important for loopback capture)
```
On Linux/Windows run the built binary in `bin/`. Play audio — the visualizer reacts.

### Tip: make an alias
So you can launch it from anywhere by just typing `heliograph`, add this to your shell rc
(`~/.zshrc` on macOS, `~/.bashrc` on Linux) and restart the terminal:
```bash
alias heliograph='open /full/path/to/heliograph/app/bin/heliograph.app'   # macOS
# alias heliograph='/full/path/to/heliograph/app/bin/heliograph'          # Linux
```

### Controls
Press **`H`** in-app for the full overlay. Quick reference:

| key | action |
|-----|--------|
| `H` | shortcuts overlay |
| `I` | parameter help — hover any control for a live explainer (Bitwig-style) |
| `G` | show / hide the control panels (they're **never** captured in the recording) |
| `E` | edit session details (Channel, Episode, Artist, Title, Note…) |
| `R` | start / stop recording |
| `M` | cycle mode (orbit · vehicle · platform · helix-static · helix-dynamic) |
| `TAB` | toggle layout (radial / grid) — each layout keeps its own independent settings |
| `X` | reset settings to defaults · `U` HUD on/off |
| `F` | fullscreen · `S` screenshot · `ESC` close a dialog |

**Right panel tabs:** **GRAPH** (sub-tabs **GLOBAL** = type/mode/fill/falloff + a **Camera-Angle**
XY pad & Z slider; **PRESETS** = save/load looks) · **AUDIO** (Rate/Spread/Punch) · **MOD**
(route audio level or an LFO to up to 3 parameters). The left column holds the per-layout
sliders (scale, colour, line width, Spin/Cam, glow…). You can open the panels and tweak settings
**mid-recording** without any of it showing up in the video.

**Double-click any fader** to snap it back to its default. Press **`I`** for hover-help: a centred
card explains whatever control your mouse is over (and the grid gotchas — e.g. *Count* does nothing
until you raise *Trans*). The card hides while you drag so you can watch the visual, then returns.
When you add a modulation it starts at the parameter's current value (no jump) — drag *Target Value*
to open it up; deleting a mod leaves the parameter frozen where it was.

**Presets** (GRAPH → PRESETS): *Save current as preset*, give it a name — it stores the current
layout's full look (not your session text). Saved as `.json` files in the `presets/` folder; any
found there are listed for one-click load. Camera angle is saved too.

---

## 5. Session content

Press **`E`** to edit the broadcast fields live (saved with **SAVE**, which also closes the dialog;
`ESC` closes without saving):

- **Channel** — the big top-right title word (e.g. `TRANSMISSION`).
- **Episode** — the number after it.
- **Artifact Title** + **Series Shorthand** — used to build the recording filename (e.g. `HelioGraph` + `TxN`).
- **Artist**, **Title**, **Note** — Title & Note appear bottom-left while recording.
- **Movement** — FREEFORM / COMPOSED / OTHER (type your own).
- **Recordings** — the folder recordings are saved to (click to choose). See §6.

The settings panel shows a live preview of the resulting recording filename at the bottom.
Auto-generated, un-fakeable telemetry: **date** = system date · **waypoint** = randomised per
session · **HDG** = the detected primary sub-frequency, shown as a bearing + note letter ·
**DIST** = seconds recorded. Config (session + presets) lives in **`~/.heliograph/`**
(`session.json` and `presets/`), portable across machines and not in the repo.

---

## 6. Recording

`R` records **native 1440p H.264** video with the audio muxed in — a **single MP4** (AAC 320k),
ready for upload, no separate audio file. Filename is composed from your session fields (lowercased):
```
heliograph-txn001-amo_eba.mp4      # 1440p video + audio, one file
```
- **First time you record**, a folder picker asks where to save. Your choice is remembered (and
  editable any time under **`E` → Recordings**). Recordings always go into a **`HelioRecordings/`**
  subfolder of the chosen folder.
- Default (if you skip the picker): the OS videos folder — macOS `~/Movies/HelioRecordings`,
  Linux `~/Videos/HelioRecordings` (`$XDG_VIDEOS_DIR`), Windows `%USERPROFILE%\Videos\HelioRecordings`.
- Changing Artifact / Shorthand / Episode / Artist / Title makes a new file; re-recording the same
  name **overwrites** (clean retakes) — you get a warning first (`press R to overwrite · ESC to cancel`).
- Frame timing is locked to wall-clock so audio/video stay in sync even if a frame is slow.
- Encoder: VideoToolbox (GPU) on macOS; `libx264` (CPU) elsewhere. On a slow CPU, lower
  `REC_W`/`REC_H` in `src/ofApp.h` or switch `GS_VCODEC` (top of `src/ofApp.cpp`) to a HW encoder
  (`h264_nvenc` / `h264_vaapi` / `h264_qsv`).

---

## 7. Tweaking the code

- Defaults & slider ranges: `src/ofApp.h` **TWEAK ZONE** + `buildSliders()`.
- Audio sensitivity / band mapping: `update()` (`fftGain`, `energy()` gains, the treble tilt).
- The visualizer: `drawPortal()` (radial) · `drawGrid()` (matrix) · `drawHelix()` (DNA).
- HUD / wordmark: `drawHud()`. Recording / codec / paths: top of `src/ofApp.cpp` + `startRecording()`.
- Resolution: `RW/RH` (render) and `REC_W/REC_H` (record) in `src/ofApp.h`.

---

## 8. Releases (prebuilt binaries)

Publishing a GitHub **Release** triggers `.github/workflows/release.yml`, which builds and attaches:
- **macOS** — a self-contained `heliograph.app` (the `data/` folder is copied into the bundle's
  `Contents/Resources/`, so no sibling folder is needed). Unsigned: first launch may need
  right-click → **Open**.
- **Linux / Windows** — a folder/zip with the binary **and its `data/` folder beside it** (the app
  reads `data/` relative to the executable — keep them together).

On first run the app creates **`~/.heliograph/`** and seeds it with `session.json` + the factory
presets bundled in `data/`. Everything after that (your edits, saved presets, chosen recordings
folder) lives in `~/.heliograph/`.

#pragma once

#include "ofMain.h"
#include <mutex>
#include <vector>
#include <map>

struct Star { glm::vec2 p; float r, base, ph; };
struct Env  { float v = 0, atk = 0.40f, rel = 0.08f; float process(float t){ v += (t > v ? atk : rel) * (t - v); return v; } };
struct Slider { std::string name; float* val = nullptr; float def = 0; float lo = 0, hi = 1; int side = 0; bool intStep = false; int prec = 2; bool reactive = false; int show = 0; int tab = -1; ofRectangle track; std::vector<std::string> opts; std::vector<ofRectangle> boxes; bool icons = false; bool toggleMask = false; };  // toggleMask: opts are independent on/off bits (value is a bitmask), not a single radio choice  // show: 0 always·1 radial·2 grid · tab: -1 left/always · 0 LAYOUT · 1 AUDIO right-tab · opts=>radio · icons=>shapes
struct Field  { std::string label; std::string* sp = nullptr; int* ip = nullptr; float* fp = nullptr; std::string buf; ofRectangle box; ofRectangle pasteBox; ofRectangle clearBox; std::vector<std::string> choices; std::string* op = nullptr; bool folder = false; bool header = false; bool secret = false; bool locked = false; bool gateBackup = false; int tab = 0; };  // gateBackup => greyed out + read-only until "Wallet Backed Up" = YES (donation protection)  // locked => read-only (e.g. artist name once registered — it defines the channel id)  // editable settings-dialog field (choices != empty => selector; op set => last choice "OTHER" is a free-text field; folder => click opens a folder picker; header => section label, no control; secret => masked when not focused (passwords/tokens); tab => which settings-dialog tab (0 SESSION · 1 ROUTING · 2 BROADCAST) this belongs to
struct ModSlot { int dest = -1; float base = 0, amt = 0; bool bipolar = false; };  // modulation: dest = slider index · base = set value · amt = target · bipolar = LFO swings ± around base

class ofApp : public ofBaseApp {
public:
    void setup();
    void update();
    void draw();
    void exit();
    void keyPressed(int key);
    void mousePressed(int x, int y, int button);
    void mouseDragged(int x, int y, int button);
    void mouseReleased(int x, int y, int button);
    void windowResized(int w, int h);

    // ---- audio ----
    void  audioIn(ofSoundBuffer & input);
    void  setupAudio();
    void  computeFFT(const std::vector<float>& in, std::vector<float>& out);

    ofSoundStream stream;
    int    sampleRate = 48000, bufferSize = 512;   // requested; replaced by the device's actual rate at setup
    std::string deviceName = "(none)";
    // ---- audio routing (input device + channel selection, ROUTING section of the settings editor) ----
    std::string sInputDevice = "";          // persisted device name; "" = auto-detect (loopback keyword match / first input)
    int    sInputDeviceIdx = 0;             // UI state: index into audioDeviceChoices (0 = Auto)
    int    sInputChannelPair = 0;           // persisted + UI state: 0-based stereo-pair index into the device's channels
    int    sInputChannelPairAtOpen = 0;     // snapshot taken when the settings dialog (re)builds fields, to detect a change on SAVE
    std::vector<std::string> audioDeviceChoices;   // "Auto" + names of input-capable devices; refreshed each time settings opens
    int    captureChannels = 0;             // channel count actually requested from the stream (device's, capped)
    int    activeChannelOffset = 0;         // resolved 0-based channel index audioIn() reads from (= sInputChannelPair*2, clamped)
    ofRectangle refreshDevicesBox;           // ROUTING tab's "REFRESH DEVICE LIST" button
    void   refreshAudioDevices();            // closes + reopens the stream to force a hardware rescan, then rebuilds the device list
    void   applyChannelSelection();          // recompute activeChannelOffset + recChannels from sInputChannelPair (live — no stream reopen; the stream already carries every channel)

    // ---- broadcast (live Icecast forwarding + snapshot push, BROADCAST section of the settings editor) ----
    FILE*  icePipe = nullptr;                     // ffmpeg subprocess piping PCM -> icecast:// (live, independent of local recording)
    std::vector<short> broadcastAudioQueue;       // PCM queued by audioIn() (audio thread), drained + piped by update() (main thread)
    bool   broadcasting = false;
    bool   recStartedByBroadcast = false;   // B auto-started the recording (so stopping B stops that recording; a manual R recording is left alone)
    float  broadcastStart = 0;                    // t when startBroadcast() ran — drives the ON AIR elapsed-time readout
    std::string sIceHost = "", sIcePort = "8000", sIceMount = "live.mp3", sIcePassword = "";
    std::string sSnapshotUrl = "", sSnapshotToken = "";
    // ---- artist registration (REGISTER section): the broadcast config above is filled in by the
    //      server's /register response — the artist enters a server + invite code + name, not raw hosts.
    std::string sRegServer = "https://radio.stackmate.org";   // registration/broadcast server base URL — defaults to the public station; change it in S -> REGISTER to point at your own heliod stack
    std::string sInviteCode = "";                             // single-use invite code (consumed on register)
    std::string sChannelId = "";                              // hashed channel id returned by the server (mount)
    bool   sRegistered = false;                               // true once a successful /register response is saved
    std::string regStatus = "";                               // last registration result/error message (shown in the dialog)
    float  regFlash = -10;                                    // timestamp of the last registration attempt (drives the status flash)
    ofRectangle registerBox;                                  // the REGISTER button in the settings dialog
    void   registerArtist();                                  // POST server/register {inviteCode, artist} -> save the returned broadcast config
    void   verifyRegistration();                              // on startup: non-blocking GET server/whoami to confirm we're still registered server-side
    bool   regVerifyPending = false;                          // a /whoami check is in flight (result polled in update())
    float  regVerifyT = 0;                                    // t when the check was fired (for a give-up timeout)
    // ---- channel appearance (CHANNEL tab): the artist owns how their channel looks on the listener
    //      client — UI font + accent colour (channel display name = sChannel). These ride in the shared
    //      snapshot metadata (X-Transmission), so SAVE + the next snapshot re-themes the live channel;
    //      no separate endpoint. Font keys are SHARED verbatim with helio-client's FONTS list.
    int    sFontIdx = 4;                                       // 0 plex · 1 jetbrains · 2 space · 3 sharetech · 4 vt323 (default)
    std::string sAccent = "#ff3b30";                          // channel accent colour (hex) applied on the client
    ofTrueTypeFont fPreview[5];                                // the 5 client faces, loaded for the live CHANNEL preview
    float  fPreviewScale[5] = {1,1,1,1,1};                     // per-face scale so every preview shares one cap-height + baseline
    // ---- donations (CHANNEL tab): a Bitcoin (bc1) and/or Liquid (lq1) address the client shows in a
    //      room. Only broadcast once the artist confirms the wallet is backed up. Ride the shared metadata.
    std::string sLnAddress  = "";                             // lightning address (user@domain, highest preference)
    std::string sBtcAddress = "";                             // bc1… Bitcoin address (validated bech32/bech32m)
    std::string sLqAddress  = "";                             // lq1… Liquid address (validated structurally)
    int    sWalletBackedUp = 0;                               // 0 NO · 1 YES — gates broadcasting the addresses
    float  lastSnapshotT = -100;                  // t of the last snapshot push
    float  lastStreamTsT = -100;                  // t of the last in-stream timestamp injection (accurate audio-lag measurement)
    void   pushStreamTimestamp();                 // inject wall-clock epoch-ms into the Icecast ICY metadata so the client can measure true end-to-end audio lag at the playhead
    static constexpr float SNAPSHOT_INTERVAL = 15.0f;   // seconds between broadcast snapshot pushes — a crisp HD grab every 15s beats a blurry one every 1-2s
    ofFbo  fboSnap;                                // HD (1080p) downscale target for the periodic JPEG snapshot push
    void   startBroadcast();
    void   stopBroadcast();
    void   pushSnapshot();
    static constexpr int N = 2048;
    std::vector<float> ringBuf;
    int    writePos = 0;
    std::mutex mtx;
    std::vector<float> spectrum, spectrumSmooth;
    float  rms = 0;
    float  peak = 0;   // decaying peak-hold of |sample| (0..1) — sent in snapshot metadata for the client console meters
    Env    levelE, bassE, midE, highE;
    float  level = 0, bass = 0, mid = 0, high = 0, prevBass = 0, beatEnv = 0;
    static constexpr int BANDS = 128;
    std::vector<float> bands;

    // ---- forest ----
    void  addTree(float pos, float h, float spr);
    void  branch(float len, float angle, int depth);
    // ---- visualizer (audioVisualizer port) ----
    void  drawPortal();
    void  drawHelix();             // Mode 3: rotating double helix (DNA), audio-reactive
    float rotationX = 0, rotationY = 0, rotationZ = 0;   // SPIN — per-ring accumulation (radial tunnel/twist)
    float camX = 0, camY = 0, camZ = 0;                  // CAMERA — accumulated spin from the Cam rate sliders
    float camManX = 0, camManY = 0, camManZ = 0;         // CAMERA — MANUAL static angle (XY pad + Z slider); total rotation = man + accumulated
    ofRectangle camPadBox, camZBox;                      // GLOBAL section: XY pad (X/Y angle) + Z slider

    void  drawScene();
    void  drawSpace();
    void  drawStars();
    void  drawForest();
    std::vector<Star> stars;
    float t = 0;

    // ---- render = window size (1:1, no display scaling => crisp text); record at the same res (no downscale) ----
    static constexpr int RW = 2560, RH = 1440;          // live render = window (1:1, crisp — no resample aliasing)
    static constexpr int REC_W = 2560, REC_H = 1440;    // recorded video = 1440p, native (no downscale softening)
    float S = 1.0f;
    float fm = 60;                 // frame margin (corner ticks + content bounds)
    ofFbo fboFinal, fboRec;

    // ---- recorder ----
    FILE*  vidPipe = nullptr;
    std::vector<short> recAudio;
    int    recChannels = 2;
    long   recFrames = 0;          // video frames written so far (drives wall-clock A/V sync)
    ofPixels recPixels;            // last captured record frame (reused for catch-up dupes)
    bool   recWarn = false;        // "file exists" overwrite prompt is showing
    std::string recFinalPath;
    bool   autoRecTest = false, didRecTest = false;
    void   startRecording();
    void   stopRecording();

    // ===========================================================================
    //  TWEAK ZONE — these are the live defaults (also draggable in-app via the
    //  FOREST/VISUALIZER panels). Change a number here to set its startup value.
    // ===========================================================================
    // --- FOREST (left panel) ---
    float cfgTreeLen = 110;       // tree height in px @1080 (scaled by S at runtime)
    float cfgAngle   = 0.40f;     // branch split angle (radians)
    float cfgSway    = 0.25f;     // how much the tree sways with bass
    float cfgTreeOpacity = 0.70f; // overall tree visibility (0..1)
    // --- VISUALIZER (right panel) — the audioVisualizer engine ---
    float cfgRate    = 0.90f;   // FFT smoothing / ring "trail" (0 = snappy, ~0.95 = long trails). Needs live audio to show.
    float cfgCenter  = 4.0f;    // spacing between concentric rings (ring size)
    float cfgSpread  = 1.5f;    // how far audio pushes each ring outward
    float cfgPunch   = 1.0f;    // how hard the detected beat/kick makes the whole visual jump
    float cfgLfoRate = 1.9575f; // LFO MOD sine freq — Schumann (7.83Hz) down 10 octaves to 7.83/1024 Hz; default = Schumann/4 (oct 2)
    float cfgBands   = 40;      // number of rings (4..128)
    float cfgMode    = 0;       // 0 = Orbit (circles), 1 = Triangle, 2 = Square, 3 = Helix
    float cfgTwist   = 0;       // per-ring fan rotation in degrees (visible on Triangle/Square)
    float cfgScale   = 1.0f;    // global scale of the whole visualizer (visualSynth Global Scale)
    float cfgLineW   = 2.6f;    // inner ring line width (Line-Width)
    float cfgAlpha   = 185;     // base ring opacity 0..255 (Alpha)
    float cfgFill    = 1;       // bitmask: bit0 (1) = OUTLINE · bit1 (2) = FILL · 3 = both · 0 falls back to outline
    float cfgRandom  = 0;       // 0 = use RGB, 1 = randomise colours per ring (Randomize)
    float cfgR = 175, cfgG = 175, cfgB = 175;  // base ring colour (RGB 0..255) — grey by default; audio shifts it
    float cfgRotX = 0, cfgRotY = 0, cfgRotZ = 0;  // SPIN rate — per-ring rotation accumulation (deg/frame, ±0.12)
    float cfgCamX = 0, cfgCamY = 0, cfgCamZ = 0;  // CAMERA rate — rigid 3D tumble (deg/frame, ±0.12)
    float fftGain    = 600.0f;  // overall audio sensitivity feeding the rings + tree
    // --- LAYOUT: 0 = RADIAL (concentric), 1 = GRID (visualSynth matrix) ---
    float cfgLayout  = 0;
    float cfgGlobalRot = 0;     // static global rotation (deg)
    // GRID-only (visualSynth matrix params)
    float cfgCountX = 6, cfgCountY = 3;     // columns / rows  (±count)
    float cfgTransX = 0, cfgTransY = 0;     // spacing X / Y (start stacked at centre)
    float cfgTwistX = 0, cfgTwistY = 0;     // per-column / per-row twist (deg)
    float cfgPinch  = 0;                    // row pinch
    float cfgShiftY = 0;                    // per-cell vertical shift
    float cfgScaleX = 0.5f, cfgScaleY = 0.5f; // per-cell scale
    float cfgFalloff = 0;                    // line-width taper curve: 0 EUCLID · 1 DIAMOND · 2 FRAME
    float cfgGlow    = 0.5f;                  // radial bloom: additive glow halo around the lines (0 = off)
    int   lastLayout = -1;
    void  relayout();
    void  drawGrid();
    bool  sliderVisible(const Slider& s);

    // ---- per-layout independent settings: RADIAL and GRID each keep their OWN copy of everything
    //      shared — appearance, colour, rotation, audio-reactive, AND the full modulation setup ----
    struct LayoutState { float scale, lineW, fill, alpha, r, g, b, rotX, rotY, rotZ, rate, spread, punch, accX, accY, accZ, lfoRate;
                         float camRX, camRY, camRZ, camAX, camAY, camAZ;   // camera rates + accumulated angles
                         float manX, manY, manZ;                           // manual camera angle (XY pad + Z)
                         float glow;
                         ModSlot audioMod[3], lfoMod[3]; };
    LayoutState layoutState[2];
    void saveLayoutState(int i);
    void loadLayoutState(int i);

    // ---- controls ----
    std::vector<Slider> sliders;
    int   activeSlider = -1;
    int   camDrag = 0;             // dragging the camera-angle control: 0 none · 1 XY pad · 2 Z slider
    ofRectangle resetBox;          // clickable RESET button in the control panel
    void  buildSliders();
    bool  showPanel = true, recording = false;
    float recStart = 0;
    bool  showHud = true, autoShot = true;
    bool  showMeter = true;   // 'V' — show/hide a horizontal audio level meter along the bottom of the screen. ON by default. SCREEN ONLY (drawn after the FBO), never part of the recording/broadcast — a performance monitor.
    bool  showMeta = false;   // 'T' — show/hide the SIGNAL indicator + session metadata/telemetry text in the frame. Default OFF: the frame (recording + client snapshot) is a clean visualizer; the client renders the metadata itself from what heliograph sends.

    // ---- MODULATION (right "MODULATION" tab): AUDIO MOD + LFO MOD, each up to 3 destinations ----
    //      A destination's value rides from its set value (base) toward the slot's target (amt),
    //      driven by audio amplitude (Audio Mod) or a sine LFO (LFO Mod). One dest per slot,
    //      no parameter modulated twice. All of this is per-layout (see LayoutState).
    ModSlot audioMod[3], lfoMod[3];
    int   rightTab = 0;            // right panel: 0 GRAPH · 1 AUDIO · 2 MODULATION
    int   graphSub = 0;            // GRAPH tab sub-section: 0 = GLOBAL (settings) · 1 = PRESETS
    ofRectangle graphSubBox[2];
    int   modSource = 0;           // MOD tab "Source" sub-tab: 0 = AUDIO · 1 = LFO (one shown at a time)
    ofRectangle sourceBox[2];
    int   modPickKind = -1, modPickSlot = -1;   // assigning a slot: kind 0 = audio, 1 = lfo
    int   activeModAmt = -1;       // dragging a slot amount (0..2 audio · 3..5 lfo) or 6 = LFO rate
    ofRectangle tabBox[3], modDestBox[6], modClearBox[6], modAmtBox[6], modBipBox[3], lfoRateBox, lfoOctPrevBox, lfoOctNextBox;
    // ---- presets: each is a named .json file in the presets/ folder; the GUI lists whatever is there ----
    std::vector<std::string> presetList;        // preset names found in the folder
    std::vector<ofRectangle> presetBox, presetDelBox;   // one row per preset (load · delete) — sized to presetList
    ofRectangle presetAddBox, presetNameBox;
    bool  presetNaming = false;                 // typing a name for "save current as preset"
    std::string presetNameBuf;
    void  scanPresets();                        // list presets/*.json into presetList
    void  savePresetNamed(const std::string& name);   // write current config -> presets/<name>.json
    void  loadPresetFile(const std::string& name);
    void  deletePresetFile(const std::string& name);
    void  applyMods();             // called each frame from update()
    bool  isModulated(int sliderIdx) const;   // already a destination in any slot?
    float errFlash = -10;          // transient error toast (e.g. "already modulated")
    std::string errMsg;

    // ---- help overlay ----
    bool  showHelp = false;
    void  drawHelp();
    // ---- parameter help ('i'): live, non-blocking. Hover any control -> a centred explainer card; dragging hides it so the art shows.
    bool  helpMode = false;
    void  drawParamHelp();
    void  drawHelpCard(const std::string& eyebrow, const std::string& title, const std::string& tag,
                       const std::string& body, const std::string& footer);   // shared centred card
    std::string hoverParamKey();                                   // key of the control under the mouse (FBO space); "" if none
    static std::pair<std::string, std::string> paramHelp(const std::string& key);   // {title, body}
    // ---- double-click a fader to restore its default ----
    float lastClickT = -100; int lastClickSlider = -1;

    // ---- HUD / session ----
    ofTrueTypeFont fKick, fTitle, fLabel, fValue, fNote, fUI, fBrand;   // fBrand = the HelioGraph Mk1 wordmark font
    int   sTransmission = 1;                 // EPISODE number
    std::string sTitle = "UNTITLED", sDate = "", sArtist = "", sChannel = "TRANSMISSION",   // Channel = the big top-right title word
                sNote = "", sWaypoint = "00·00·000";
    std::string sShorthand = "TxN", sArtifact = "HelioGraph";   // filename series shorthand · artifact name
    std::string sRecDir = "";                // chosen recordings folder ("" = default ~/.heliograph/recordings)
    std::string recBaseName() const;         // composes the recording filename from artifact + shorthand + episode + artist
    std::string recDir();                    // the active recordings folder (sRecDir if set, else the default), with trailing slash, created
    bool        pickRecDir();                // native folder chooser -> sRecDir; true if the user picked one
    float sHeading = 0, sDist = 0;
    float subFreq = 0;                        // detected primary sub frequency (Hz) — shown as the HDG value
    std::string subNote = "--";               // its note letter (no octave) — shown in brackets
    void  seedUserData();          // first launch: create ~/.heliograph/ with a default session.json + the factory presets (from bin/data)
    void  loadSession();
    void  drawHud();
    void  drawPanels();
    // ---- in-app settings editor (edit content; no JSON) ----
    std::vector<Field> fields;
    bool  settingsOpen = false;
    int   editingField = -1;
    // Settings are NOT autosaved: field edits mutate the in-memory config live (for the CHANNEL preview
    // etc.), but ESC discards them. We snapshot every field's target when the dialog opens (and after a
    // SAVE — the new baseline), and revert to it on ESC.
    std::map<std::string*, std::string> snapS;
    std::map<int*, int> snapI;
    std::map<float*, float> snapF;
    void snapshotFields();   // capture current field values as the baseline
    void revertFields();     // restore the baseline (ESC = discard unsaved edits)
    int   settingsTab = 0;         // which settings-dialog tab is showing: 0 SESSION · 1 ROUTING · 2 REGISTER · 3 CHANNEL
    ofRectangle settingsTabBox[4]; // clickable tab chips, sized/positioned in drawSettings()
    float saveFlash = -10;        // timestamp of last successful save (drives the "SAVED ✓" flash)
    ofRectangle saveBox;
    void  buildFields();
    void  drawSettings();
    void  writeSession();
    std::string fieldText(const Field& f);
    void  commitField(Field& f);
    ofRectangle displayRect();     // aspect-fit (letterbox) rect for the FBO in the window
    void  resetConfig();           // restore init settings + reset the visual's orientation

    // palette — fully greyscale. cNeon kept as the bright "accent" white (used by SAVE, cursor, LIVE).
    ofColor cTrunk{60, 62, 61}, cNeon{238, 242, 239}, cPastel{182, 186, 184}, cStar{176, 178, 177};
};

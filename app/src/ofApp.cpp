#include "ofApp.h"
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <thread>
#ifndef _WIN32
  #include <unistd.h>    // write()
  #include <fcntl.h>     // fcntl() O_NONBLOCK — non-blocking broadcast pipe
  #include <cerrno>      // errno / EAGAIN
#endif
#include "ofAppGLFWWindow.h"
#include <GLFW/glfw3.h>

// System clipboard text (for Cmd/Ctrl+V into the settings fields — e.g. pasting an invite code).
static std::string gsClipboard() {
    if (auto* w = dynamic_cast<ofAppGLFWWindow*>(ofGetWindowPtr())) {
        if (const char* s = glfwGetClipboardString(w->getGLFWWindow())) return std::string(s);
    }
    return "";
}

// Wall-clock epoch milliseconds — matches JavaScript Date.now() on the client, so end-to-end lag
// subtractions line up. NOT ofGetSystemTimeMillis(): that returns oF's internal (monotonic/uptime)
// clock, ~5 days of ms, never a real epoch — which broke the audio-lag + artist→server readouts.
static long long gsEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
#ifndef _WIN32
  #include <csignal>
#endif

// ===========================================================================
//  Platform portability — macOS keeps its original setup; Linux/Windows use
//  portable equivalents. (Generate the build per-OS with oF projectGenerator.)
// ===========================================================================
#ifdef __APPLE__
  #define GS_VCODEC "-c:v h264_videotoolbox -b:v 100M -pix_fmt yuv420p"          // GPU encode (VideoToolbox)
#else
  #define GS_VCODEC "-c:v libx264 -preset veryfast -crf 16 -pix_fmt yuv420p"      // portable CPU encode
#endif
// ffmpeg path resolved at runtime: GUI apps don't inherit the shell PATH, so on macOS we probe the
// common Homebrew/system locations (Apple Silicon AND Intel); elsewhere we rely on PATH.
static std::string gsFFmpeg() {
#ifdef __APPLE__
    const char* cands[] = { "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg" };
    for (const char* c : cands) if (ofFile::doesFileExist(c)) return c;
#endif
    return "ffmpeg";   // Linux/Windows (and macOS fallback): from PATH
}
#ifdef _WIN32
  #define GS_POPEN  _popen
  #define GS_PCLOSE _pclose
  #define GS_PIPEMODE "wb"
#else
  #define GS_POPEN  popen
  #define GS_PCLOSE pclose
  #define GS_PIPEMODE "w"
#endif

// User data (session.json, presets/, recordings/) lives in ~/.heliograph/ — portable across machines,
// so a distributed binary stores its config in the user's home, not next to the executable.
static std::string gsHome() {
    const char* h = getenv("HOME");
#ifdef _WIN32
    if (!h || !*h) h = getenv("USERPROFILE");
#endif
    std::string base = std::string((h && *h) ? h : ".") + "/.heliograph/";
    ofDirectory::createDirectory(base, false, true);   // created on demand
    return base;
}
// the OS-standard "videos" location (no side effects) — where default recordings go
static std::string gsVideosDir() {
    const char* h = getenv("HOME");
#ifdef _WIN32
    if (!h || !*h) h = getenv("USERPROFILE");
    return std::string((h && *h) ? h : ".") + "\\Videos\\";
#elif defined(__APPLE__)
    return std::string((h && *h) ? h : ".") + "/Movies/";
#else
    const char* x = getenv("XDG_VIDEOS_DIR");
    if (x && *x) { std::string d = x; if (d.back() != '/') d += '/'; return d; }
    return std::string((h && *h) ? h : ".") + "/Videos/";
#endif
}
static std::string gsSessionPath() { return gsHome() + "session.json"; }
static std::string gsPresetsDir() {
    std::string d = gsHome() + "presets/";
    ofDirectory::createDirectory(d, false, true);
    return d;
}
static std::string gsImagesDir() {                       // IMAGE mode reads slideshow images from here
    std::string d = gsHome() + "images/";
    ofDirectory::createDirectory(d, false, true);
    return d;
}
static std::string gsRecDir() {                          // default recordings: <OS videos>/HelioRecordings/
    std::string d = gsVideosDir() + "HelioRecordings/";
    ofDirectory::createDirectory(d, false, true);
    return d;
}
static std::string gsScratch(const std::string& f) {
#ifdef __APPLE__
    return "/tmp/" + f;
#else
    return ofToDataPath(f, true);
#endif
}
// Quote a value for safe interpolation into a shell command line (BROADCAST fields — Icecast
// password, snapshot URL/token — are free-typed by the artist, so this isn't optional).
// Cross-platform: POSIX sh uses single quotes; Windows cmd.exe ignores single quotes, so there we
// wrap in double quotes and escape embedded double-quotes. (Our interpolated args — URLs, tokens,
// base64 header values, file paths — don't contain cmd %VAR% patterns, so double-quoting is safe.)
static std::string gsShQuote(const std::string& s) {
#if defined(_WIN32)
    std::string q = "\"";
    for (char c : s) q += (c == '"') ? std::string("\\\"") : std::string(1, c);
    q += "\"";
    return q;
#else
    std::string q = "'";
    for (char c : s) q += (c == '\'') ? "'\\''" : std::string(1, c);
    q += "'";
    return q;
#endif
}
// The null device + a stderr-discard suffix, per platform (POSIX `/dev/null` vs Windows `NUL`).
static std::string gsNullDev() {
#if defined(_WIN32)
    return "NUL";
#else
    return "/dev/null";
#endif
}
// Channel-branding fonts. The KEY is shared verbatim with helio-client's FONTS list (client maps it to
// a @font-face family) — heliograph only sends the key; it bundles the same faces to render the preview.
static const int   kNumFonts = 5;
static const char* kFontKeys[kNumFonts]   = { "plex", "jetbrains", "space", "sharetech", "vt323" };
static const char* kFontLabels[kNumFonts] = { "IBM PLEX MONO", "JETBRAINS MONO", "SPACE MONO", "SHARE TECH MONO", "VT323" };
static const char* kFontFiles[kNumFonts]  = { "fonts/IBMPlexMono-Regular.ttf", "fonts/JetBrainsMono.ttf",
                                              "fonts/SpaceMono-Regular.ttf", "fonts/ShareTechMono-Regular.ttf",
                                              "fonts/VT323-Regular.ttf" };
// Generative track-art styles for a published collection (PUBLISH tab) — the client renders whichever is
// set (sent as `artStyle` on POST /collections), falling back to the first. Keep in sync with the server's
// ART_STYLES and the web client's generators.
static const int   kNumArtStyles = 6;
static const char* kArtStyles[kNumArtStyles] = { "sigil-a", "sigil-b", "sigil-c", "sonar", "matrix", "spectrogram" };
// bech32/bech32m checksum (BIP173/350) — validates a bc1 Bitcoin address (witness v0 bech32 OR v1
// taproot bech32m). Liquid lq1 addresses use blech32 (a different, longer checksum) — those are
// validated structurally in gsValidAddr (charset + length), not by checksum.
static bool gsBech32Verify(const std::string& hrp, const std::vector<int>& data, uint32_t want) {
    static const uint32_t GEN[5] = { 0x3b6a57b2u, 0x26508e6du, 0x1ea119fau, 0x3d4233ddu, 0x2a1462b3u };
    uint32_t chk = 1;
    auto push = [&](int v) {
        uint32_t b = chk >> 25;
        chk = ((chk & 0x1ffffffu) << 5) ^ (uint32_t)v;
        for (int i = 0; i < 5; i++) if ((b >> i) & 1) chk ^= GEN[i];
    };
    for (char c : hrp) push((unsigned char)c >> 5);
    push(0);
    for (char c : hrp) push((unsigned char)c & 31);
    for (int v : data) push(v);
    return chk == want;
}
// Accept a bc1 Bitcoin address (real bech32/bech32m checksum) OR an lq1 Liquid address (structural).
static bool gsValidAddr(const std::string& addr) {
    if (addr.size() < 14 || addr.size() > 130) return false;
    for (char c : addr) if (c >= 'A' && c <= 'Z') return false;      // must be all-lowercase
    bool isBtc = addr.rfind("bc1", 0) == 0, isLq = addr.rfind("lq1", 0) == 0;
    if (!isBtc && !isLq) return false;
    size_t sep = addr.rfind('1');
    if (sep != 2) return false;                                       // hrp is exactly "bc"/"lq"
    std::string data = addr.substr(sep + 1);
    static const std::string CS = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    std::vector<int> vals;
    for (char c : data) { size_t p = CS.find(c); if (p == std::string::npos) return false; vals.push_back((int)p); }
    if (isBtc) return vals.size() >= 6 && (gsBech32Verify("bc", vals, 1u) || gsBech32Verify("bc", vals, 0x2bc830a3u));
    return vals.size() >= 30 && vals.size() <= 120;                   // lq1 blech32 — structural only
}
static std::string gsGroup4(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) { if (i && i % 4 == 0) out += ' '; out += s[i]; }
    return out;
}
// A Lightning address is email-like (user@domain.tld, LNURL-pay style) — validate structurally.
static bool gsValidLightning(const std::string& s) {
    if (s.size() < 3 || s.size() > 120) return false;
    for (char c : s) if (c == ' ' || (c >= 'A' && c <= 'Z')) return false;   // no spaces, all-lowercase
    size_t at = s.find('@');
    if (at == std::string::npos || at == 0 || at != s.rfind('@')) return false;   // exactly one @, non-empty local part
    std::string dom = s.substr(at + 1);
    size_t dot = dom.find('.');
    return dot != std::string::npos && dot > 0 && dot + 1 < dom.size();
}
// Standard base64 — used to carry the transmission metadata JSON (which may contain unicode
// titles/notes) in an ASCII-only HTTP header on the snapshot push (see pushSnapshot).
static std::string gsBase64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c; bits += 8;
        while (bits >= 0) { out += T[(val >> bits) & 0x3F]; bits -= 6; }
    }
    if (bits > -6) out += T[((val << 8) >> (bits + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

// =============================================================================
//  heliograph — where to tweak things
//  - Default look / slider ranges ....... ofApp.h "TWEAK ZONE" + buildSliders()
//  - Audio sensitivity / band mapping ... update()      (fftGain, energy() gains)
//  - The ring visualizer ................ drawPortal()  (thickness, colour-mod, rotation)
//  - The tree ........................... branch() + addTree() + drawForest()
//  - Starfield / background ............. drawStars() + drawSpace()
//  - HUD text + layout .................. drawHud()      (margins via fm, baselines)
//  - Recording quality / codec .......... startRecording() (bitrate, REC_W/REC_H in .h)
//  - Window size / resolution ........... main.cpp + RW/RH in ofApp.h
// =============================================================================

static void writeWav(const std::string& path, const std::vector<short>& d, int ch, int sr) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    uint32_t dataBytes = (uint32_t)d.size() * 2, byteRate = sr * ch * 2;
    auto w32 = [&](uint32_t v){ f.write((char*)&v, 4); };
    auto w16 = [&](uint16_t v){ f.write((char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(ch); w32(sr); w32(byteRate); w16(ch * 2); w16(16);
    f.write("data", 4); w32(dataBytes);
    if (dataBytes) f.write((const char*)d.data(), dataBytes);
}

//--------------------------------------------------------------
void ofApp::setup() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);   // a dropped Icecast connection must fail a pipe write, not kill the app
#endif
    ofDisableArbTex();
    ofSetFrameRate(60);
    ofSetVerticalSync(true);
    ofSetBackgroundColor(0, 0, 0);   // black letterbox bars in fullscreen
    ofSetCircleResolution(120);   // plenty at 1440p; lighter
    S = RH / 1080.0f;
    fm = 64 * S;                  // frame margin (corner ticks + content bounds)

    ringBuf.assign(N, 0.0f);
    spectrum.assign(N / 2, 0.0f);
    spectrumSmooth.assign(N / 2, 0.0f);
    bands.assign(BANDS, 0.0f);

    // single 4K target (MSAA gives clean lines; no bloom)
    ofFboSettings fs; fs.width = RW; fs.height = RH; fs.internalformat = GL_RGBA; fs.numSamples = 4; fs.useDepth = false;
    fboFinal.allocate(fs);
    ofFboSettings fr; fr.width = REC_W; fr.height = REC_H; fr.internalformat = GL_RGBA; fr.numSamples = 0; fr.useDepth = false;
    fboRec.allocate(fr);
    ofFboSettings fsnap; fsnap.width = RW; fsnap.height = RH; fsnap.internalformat = GL_RGBA; fsnap.numSamples = 0; fsnap.useDepth = false;
    fboSnap.allocate(fsnap);   // FULL native render res (2560x1440) — a 1:1 copy of fboFinal, no downscale. Lossless PNG every SNAPSHOT_INTERVAL s, so max quality with no compromise; bandwidth is fine at 15s cadence
    fboFinal.getTexture().setTextureMinMagFilter(GL_LINEAR, GL_LINEAR);
    fboRec.getTexture().setTextureMinMagFilter(GL_LINEAR, GL_LINEAR);
    fboSnap.getTexture().setTextureMinMagFilter(GL_LINEAR, GL_LINEAR);

    stars.clear();
    for (int i = 0; i < 200; i++) {
        Star s; s.p = glm::vec2(ofRandom(RW), ofRandom(RH));
        s.r = ofRandom(0.6f, 2.0f) * S; s.base = ofRandom(0.10f, 0.35f); s.ph = ofRandom(0, TWO_PI);
        stars.push_back(s);
    }

    auto loadFont = [&](ofTrueTypeFont& f, const char* path, int sz) {
        ofTrueTypeFontSettings st(path, sz);
        st.antialiased = true;
        st.addRanges({ ofUnicode::Latin, ofUnicode::Latin1Supplement, ofUnicode::GeneralPunctuation });
        f.load(st);
    };
    // STATIC Saira (not the variable .ttf): openFrameworks/FreeType renders variable fonts with thin,
    // badly-hinted strokes that alias hard at these sizes. The static Medium weight is crisp + solid.
    const char* reg = "fonts/SairaSemiCondensed-Medium.ttf";
    loadFont(fKick,   reg, (int)(18 * S)); fKick.setLetterSpacing(1.35f);   // (legacy kick font; wordmark now uses fBrand)
    loadFont(fTitle,  reg, (int)(26 * S));
    loadFont(fLabel,  reg, (int)(10 * S)); fLabel.setLetterSpacing(1.42f);
    loadFont(fValue,  reg, (int)(15 * S));
    loadFont(fNote,   reg, (int)(16 * S));
    loadFont(fUI,     reg, (int)(13 * S));
    loadFont(fBrand,  reg, (int)(15 * S)); fBrand.setLetterSpacing(1.5f);   // HelioGraph Mk1 wordmark (usual Saira font, roomy letterspacing)
    for (int i = 0; i < kNumFonts; i++) loadFont(fPreview[i], kFontFiles[i], (int)(22 * S));   // CHANNEL tab: live font preview
    // Normalize the preview faces: each has a different cap-height at the same point size (VT323 reads
    // small + low). Scale each so its uppercase cap-height matches the tallest, drawn from one baseline.
    { float target = 0; float capH[kNumFonts];
      for (int i = 0; i < kNumFonts; i++) { capH[i] = fPreview[i].getStringBoundingBox("HNXO", 0, 0).height; target = std::max(target, capH[i]); }
      for (int i = 0; i < kNumFonts; i++) fPreviewScale[i] = (capH[i] > 0.1f) ? target / capH[i] : 1.0f; }
    ofSetEscapeQuitsApp(false);   // ESC closes the settings/help overlay — it must NOT quit the app

    seedUserData();               // first launch: create ~/.heliograph/ (session.json + factory presets) from bin/data
    // Persistent log — survives Finder launches (console output is otherwise lost), so an overnight
    // unattended broadcast can be reviewed in the morning. Appends to ~/.heliograph/heliograph.log.
    ofLogToFile(gsHome() + "heliograph.log", true);
    ofSetLogLevel(OF_LOG_NOTICE);
    ofLogNotice() << "───── heliograph launch " << ofGetTimestampString("%Y-%m-%d %H:%M:%S") << " ─────";
    gsImagesDir();                // ensure ~/.heliograph/images/ exists for IMAGE mode
    loadImages();                 // scan any images already dropped in
    loadSession();
    verifyRegistration();         // on startup: non-blocking check that we're still registered server-side
    if (!ofFile::doesFileExist(gsSessionPath())) writeSession();   // belt-and-suspenders: if no factory file shipped, persist defaults
    buildFields();
    buildSliders();
    saveLayoutState(0); saveLayoutState(1);   // both layouts start from the same defaults, then diverge independently
    scanPresets();
    setupAudio();

    // Headless resilience: if a prior run was broadcasting and exited unexpectedly (crash / kill / power
    // blip), the sentinel written by startBroadcast() still exists — come straight back on air with no
    // keystroke, so an overnight broadcast survives an app restart. A clean stop (B) or an auth failure
    // deletes the sentinel, so a normal launch never auto-broadcasts.
    if (sRegistered && ofFile::doesFileExist(gsHome() + "broadcasting.flag")) {
        ofLogNotice() << ofGetTimestampString("%H:%M:%S") << "  AUTO-RESUME: broadcast sentinel present — going live without a keystroke";
        startBroadcast();
    }
}

//--------------------------------------------------------------
// First launch: populate ~/.heliograph/ from the factory data shipped in bin/data (session.json + presets/).
// Idempotent — only seeds what's missing, so the user's own settings/presets are never overwritten.
void ofApp::seedUserData() {
    std::string userSession = gsSessionPath();                       // also creates ~/.heliograph/
    if (!ofFile::doesFileExist(userSession)) {
        std::string factory = ofToDataPath("session.json", true);
        if (ofFile::doesFileExist(factory)) ofFile::copyFromTo(factory, userSession, false, false);
    }
    std::string userPresets = gsPresetsDir();                        // also creates ~/.heliograph/presets/
    ofDirectory ud(userPresets); ud.allowExt("json"); ud.listDir();
    if (ud.size() == 0) {                                            // no user presets yet -> seed the factory ones
        ofDirectory fd(ofToDataPath("presets/", true)); fd.allowExt("json"); fd.listDir();
        for (size_t i = 0; i < fd.size(); i++) {
            ofFile f(fd.getPath(i));
            ofFile::copyFromTo(f.getAbsolutePath(), userPresets + f.getFileName(), false, false);
        }
    }
}
void ofApp::loadSession() {
    std::ifstream in(gsSessionPath());
    if (in) {
        try {
            ofJson j; in >> j;
            sTransmission = j.value("transmission", 1);
            sTitle   = j.value("title", sTitle);
            sArtist  = j.value("artist", sArtist);
            sChannel = j.value("channel", sChannel);
            sNote    = j.value("note", sNote);
            sAccent  = j.value("accent", sAccent);
            if (j.contains("donations")) { auto& d = j["donations"];
                sLnAddress = d.value("ln", sLnAddress); sBtcAddress = d.value("btc", sBtcAddress);
                sLqAddress = d.value("lq", sLqAddress); sWalletBackedUp = d.value("backedUp", 0); }
            { std::string fk = j.value("font", std::string(kFontKeys[sFontIdx]));   // CHANNEL font, stored as a key
              for (int i = 0; i < kNumFonts; i++) if (fk == kFontKeys[i]) { sFontIdx = i; break; } }
            sArtifact  = j.value("artifact", sArtifact);
            sShorthand = j.value("shorthand", sShorthand);
            sRecDir    = j.value("recDir", sRecDir);
            sImgDir    = j.value("imgDir", sImgDir);   // IMAGE type: chosen images folder
            sInputDevice      = j.value("inputDevice", sInputDevice);
            sInputChannelPair = j.value("inputChannelPair", sInputChannelPair);
            if (j.contains("broadcast")) {
                auto& b = j["broadcast"];
                sIceHost       = b.value("iceHost", sIceHost);
                sIcePort       = b.value("icePort", sIcePort);
                sIceMount      = b.value("iceMount", sIceMount);
                sIcePassword   = b.value("icePassword", sIcePassword);
                sSnapshotUrl   = b.value("snapshotUrl", sSnapshotUrl);
                sSnapshotToken = b.value("snapshotToken", sSnapshotToken);
            }
            if (j.contains("registration")) {
                auto& r = j["registration"];
                sRegServer  = r.value("server", sRegServer);
                sChannelId  = r.value("channelId", sChannelId);
                sRegistered = r.value("registered", false);
                if (sRegistered) regStatus = "Registered as " + sArtist;
            }
            sCollectionName = j.value("collection", sCollectionName);   // PUBLISH tab: persisted collection name
            { std::string ak = j.value("collectionArt", std::string(kArtStyles[0]));   // PUBLISH tab: persisted track-art style (by key)
              for (int i = 0; i < kNumArtStyles; i++) if (ak == kArtStyles[i]) { sCollectionArtIdx = i; break; } }
        } catch (...) { ofLogError() << "session.json parse failed"; }
    }
    // date is always the system date — sessions capture live, so it can't be edited or faked
    auto pad = [](int v){ return (v < 10 ? "0" : "") + ofToString(v); };
    sDate = ofToString(ofGetYear()) + "." + pad(ofGetMonth()) + "." + pad(ofGetDay());
    // telemetry is auto-generated, never user-set: fresh waypoint + heading each session; distance counts recorded seconds
    auto wp = [](int v, int w){ std::string s = ofToString(v); while ((int)s.size() < w) s = "0" + s; return s; };
    sWaypoint = wp((int)ofRandom(0, 100), 2) + "\xC2\xB7" + wp((int)ofRandom(0, 100), 2) + "\xC2\xB7" + wp((int)ofRandom(0, 1000), 3);
    sHeading  = ofRandom(0, 360);
    sDist     = 0;
}

//--------------------------------------------------------------
void ofApp::buildSliders() {
    sliders.clear();
    // tab: -1 = left column (the modulatable params, always shown) · 0 = LAYOUT tab · 1 = AUDIO tab
    auto add = [&](std::string n, float* v, float lo, float hi, bool isInt, int prec, int tab, int show) {
        Slider s; s.name = n; s.val = v; s.def = *v; s.lo = lo; s.hi = hi; s.intStep = isInt; s.prec = prec; s.tab = tab; s.show = show; sliders.push_back(s);   // def = the TWEAK-ZONE startup value (double-click a fader to restore it)
    };
    auto addChoice = [&](std::string n, float* v, std::vector<std::string> opts, int show) {
        Slider s; s.name = n; s.val = v; s.def = *v; s.lo = 0; s.hi = (float)opts.size() - 1; s.intStep = true; s.prec = 0; s.show = show; s.tab = 0; s.opts = opts; sliders.push_back(s);
    };
    // FOREST disabled (uncomment to restore): cfgTreeLen / cfgAngle / cfgTreeOpacity / cfgSway
    // ---- LAYOUT tab (right): layout / mode / fill / falloff ----
    addChoice("Type",   &cfgLayout, {"RADIAL", "GRID", "IMAGE"}, 0);          // 0 Radial · 1 Grid (TAB toggles these two) · 2 Image slideshow
    addChoice("Mode",   &cfgMode,   {"ORBIT", "VEHICLE", "PLATFORM", "HELIX-S", "HELIX-D"}, 0); sliders.back().icons = true;   // ○ △ ▢ + static/dynamic helix (or 'm') — hidden in IMAGE type
    addChoice("Fill", &cfgFill, {"OUTLINE", "FILL"}, 0); sliders.back().toggleMask = true;   // independent toggles: bit0 OUTLINE · bit1 FILL · select BOTH for fill-with-outline
    addChoice("Falloff", &cfgFalloff, {"EUCLID", "DIAMOND", "FRAME", "REVERSE"}, 0);    // taper/fade curve — both layouts (REVERSE flips the width taper)
    // ---- LEFT column: the modulatable parameters (always shown) ----
    add("Scale",    &cfgScale,     0.2f, 2.5f, false, 2, -1, 0);
    add("Glob Rot", &cfgGlobalRot, -180, 180, false, 0, -1, 2);   // grid-only (radial ignores it)
    add("Line W",   &cfgLineW,     0.3f, 8,  false, 1, -1, 0);
    add("Alpha",    &cfgAlpha,     0, 255,   true,  0, -1, 0);
    add("Red",      &cfgR,         0, 255,   true,  0, -1, 0);
    add("Green",    &cfgG,         0, 255,   true,  0, -1, 0);
    add("Blue",     &cfgB,         0, 255,   true,  0, -1, 0);
    add("Spin X",   &cfgRotX,     -0.12f, 0.12f, false, 3, -1, 1);   // radial-only: per-ring accumulating twist (tunnel)
    add("Spin Y",   &cfgRotY,     -0.12f, 0.12f, false, 3, -1, 1);
    add("Spin Z",   &cfgRotZ,     -0.12f, 0.12f, false, 3, -1, 1);
    add("Cam X",    &cfgCamX,     -0.12f, 0.12f, false, 3, -1, 0);   // both: rigid whole-object camera tumble
    add("Cam Y",    &cfgCamY,     -0.12f, 0.12f, false, 3, -1, 0);
    add("Cam Z",    &cfgCamZ,     -0.12f, 0.12f, false, 3, -1, 0);
    add("Centre",   &cfgCenter,    1, 30,    false, 1, -1, 1);   // radial-only
    add("Bands",    &cfgBands,     4, BANDS, true,  0, -1, 1);
    add("Twist",    &cfgTwist,     0, 90,    false, 1, -1, 1);
    add("Glow",     &cfgGlow,      0, 1.5f,  false, 2, -1, 1);   // radial-only: bloom/glow halo on the lines
    add("Count-X",  &cfgCountX,    0, 200,   true,  0, -1, 2);   // grid-only
    add("Count-Y",  &cfgCountY,    0, 50,    true,  0, -1, 2);
    add("Trans-X",  &cfgTransX,    0, 200,   false, 0, -1, 2);
    add("Trans-Y",  &cfgTransY,    0, 200,   false, 0, -1, 2);
    add("Twist-X",  &cfgTwistX,   -90, 90,   false, 1, -1, 2);
    add("Twist-Y",  &cfgTwistY,   -90, 90,   false, 1, -1, 2);
    add("Pinch",    &cfgPinch,     0, 30,    false, 1, -1, 2);
    add("Shift-Y",  &cfgShiftY,   -1000, 1000, false, 0, -1, 2);
    add("Scale-X",  &cfgScaleX,    0, 20,    false, 2, -1, 2);
    add("Scale-Y",  &cfgScaleY,    0, 20,    false, 2, -1, 2);
    // ---- AUDIO tab (right): audio-reactive ----
    add("Rate",     &cfgRate,      0, 0.99f, false, 2, 1, 0);
    add("Spread",   &cfgSpread,    0, 12,    false, 1, 1, 0);
    add("Punch",    &cfgPunch,     0, 4,     false, 1, 1, 0);   // beat/kick impact on the whole visual

    // ---- IMAGE type controls (show=3 → shown only when Type = IMAGE). LEFT column (tab -1), like the
    //      other modulatable params, so they auto-fit the column instead of overflowing a fixed tab. ----
    addChoice("Blend", &cfgImgBlend, {"GLOW", "SOFT"}, 3); sliders.back().tab = -1;   // GLOW additive (dissolves into the dark bg) · SOFT alpha + radial feather
    add("Opacity",  &cfgImgOpacity, 0, 1,     false, 2, -1, 3);
    add("Feather",  &cfgImgFeather, 0, 1,     false, 2, -1, 3);   // SOFT edge dissolve
    add("Img Scale",&cfgImgScale,   0.3f, 3,  false, 2, -1, 3);
    add("Pan X",    &cfgImgPanX,   -1, 1,     false, 2, -1, 3);
    add("Pan Y",    &cfgImgPanY,   -1, 1,     false, 2, -1, 3);
    add("Rotate",   &cfgImgRot,    -180, 180, false, 0, -1, 3);
    add("Ken Burns",&cfgImgKen,     0, 1,     false, 2, -1, 3);   // slow zoom/pan drift per image
    add("Bright",   &cfgImgBright,  0, 2,     false, 2, -1, 3);
    add("Tint",     &cfgImgTint,    0, 1,     false, 2, -1, 3);   // 0 own colour .. 1 channel accent
    addChoice("Auto Cycle", &cfgImgAuto, {"OFF", "ON"}, 3); sliders.back().tab = -1;   // OFF: locked, click to transition · ON: rotate through the folder
    add("Hold",     &cfgImgInterval,2, 30,    false, 1, -1, 3);   // seconds per image (Auto Cycle only)
    add("Fade",     &cfgImgTrans,   0.2f, 5,  false, 1, -1, 3);   // crossfade duration
    add("Reactive", &cfgImgAudio,   0, 1,     false, 2, -1, 3);   // audio-reactive pulse

    relayout();
}

//--------------------------------------------------------------
// per-layout independent settings — swap the shared sliders when RADIAL <-> GRID
void ofApp::saveLayoutState(int i) {
    LayoutState& L = layoutState[i];
    L.scale = cfgScale; L.lineW = cfgLineW; L.fill = cfgFill; L.alpha = cfgAlpha;
    L.r = cfgR; L.g = cfgG; L.b = cfgB;
    L.rotX = cfgRotX; L.rotY = cfgRotY; L.rotZ = cfgRotZ;
    L.camRX = cfgCamX; L.camRY = cfgCamY; L.camRZ = cfgCamZ;
    L.rate = cfgRate; L.spread = cfgSpread; L.punch = cfgPunch;
    L.accX = rotationX; L.accY = rotationY; L.accZ = rotationZ; L.lfoRate = cfgLfoRate;
    L.camAX = camX; L.camAY = camY; L.camAZ = camZ; L.glow = cfgGlow;
    L.manX = camManX; L.manY = camManY; L.manZ = camManZ;
    for (int k = 0; k < 3; k++) { L.audioMod[k] = audioMod[k]; L.lfoMod[k] = lfoMod[k]; }   // modulation is per-layout too
}
void ofApp::loadLayoutState(int i) {
    LayoutState& L = layoutState[i];
    cfgScale = L.scale; cfgLineW = L.lineW; cfgFill = L.fill; cfgAlpha = L.alpha;
    cfgR = L.r; cfgG = L.g; cfgB = L.b;
    cfgRotX = L.rotX; cfgRotY = L.rotY; cfgRotZ = L.rotZ;
    cfgCamX = L.camRX; cfgCamY = L.camRY; cfgCamZ = L.camRZ;
    cfgRate = L.rate; cfgSpread = L.spread; cfgPunch = L.punch;
    rotationX = L.accX; rotationY = L.accY; rotationZ = L.accZ; cfgLfoRate = L.lfoRate;
    camX = L.camAX; camY = L.camAY; camZ = L.camAZ; cfgGlow = L.glow;
    camManX = L.manX; camManY = L.manY; camManZ = L.manZ;
    for (int k = 0; k < 3; k++) { audioMod[k] = L.audioMod[k]; lfoMod[k] = L.lfoMod[k]; }
}

bool ofApp::isModulated(int idx) const {
    for (int k = 0; k < 3; k++) if (audioMod[k].dest == idx || lfoMod[k].dest == idx) return true;
    return false;
}
void ofApp::applyMods() {
    float amp     = ofClamp(level, 0.f, 1.f);                     // audio amplitude (0..1)
    float sinFull = sinf(TWO_PI * cfgLfoRate * t);               // sine LFO (-1..1)
    float sin01   = 0.5f + 0.5f * sinFull;                       // unipolar (0..1)
    auto drive = [&](ModSlot& m, float k) {                      // base + k*(amt-base), clamped to the dest range
        if (m.dest < 0 || m.dest >= (int)sliders.size()) return;
        Slider& s = sliders[m.dest]; if (!s.val) return;
        float v = ofClamp(m.base + k * (m.amt - m.base), s.lo, s.hi);
        *s.val = s.intStep ? roundf(v) : v;
    };
    for (int k = 0; k < 3; k++) drive(audioMod[k], amp);                              // audio rides base -> amt
    for (int k = 0; k < 3; k++) drive(lfoMod[k], lfoMod[k].bipolar ? sinFull : sin01); // LFO: bipolar swings ± around base
}

bool ofApp::sliderVisible(const Slider& s) {
    bool imageType = cfgLayout >= 1.5f;          // IMAGE is Type option 2 (RADIAL · GRID · IMAGE)
    if (s.val == &cfgLayout) return true;        // the Type selector is ALWAYS available (so you can switch back)
    if (s.show == 3) return imageType;           // IMAGE-only controls
    if (imageType) return false;                 // IMAGE hides every visualizer param (incl. the Mode selector)
    if (s.show == 0) return true;
    if (s.show == 1) return cfgLayout < 0.5f;    // radial-only
    return cfgLayout >= 0.5f;                    // grid-only (image already returned above)
}

void ofApp::relayout() {
    int visL = 0; for (auto& s : sliders) if (s.tab == -1 && sliderVisible(s)) visL++;   // left column count
    float h = 13 * S, w = 320 * S;               // wider column so 3-up button rows (VEHICLE/PLATFORM) fit
    float gapR = 52 * S;
    float xL = fm + 20 * S,           yL = fm + 118 * S;          // modulatable params — left
    float xR = RW - fm - 20 * S - w,  yR = fm + 196 * S + (rightTab == 0 ? 40 * S : 0);   // right-tab content (GRAPH leaves room for its GLOBAL/PRESETS sub-tab row + the first label)
    float gapL = std::min(52.0f * S, ((RH - fm - 28 * S) - yL) / std::max(1, visL));   // fill the column: roomy but capped
    for (auto& s : sliders) {
        bool vis = sliderVisible(s) && (s.tab == -1 || (s.tab == rightTab && !(rightTab == 0 && graphSub == 1)));   // left always · right only on its tab (GRAPH sliders hidden in the PRESETS sub-section)
        if (!vis) { s.track = ofRectangle(-99999, -99999, 0, 0); s.boxes.clear(); continue; }
        bool choice = !s.opts.empty();
        float rh = choice ? 26 * S : h;
        if (s.tab == -1) { s.track = ofRectangle(xL, yL, w, rh); yL += gapL; }
        else             { s.track = ofRectangle(xR, yR, w, rh); yR += gapR; }
        if (choice) {                                        // segmented option buttons
            s.boxes.clear();
            int n = (int)s.opts.size();
            float g = 6 * S, segW = (w - g * (n - 1)) / n;
            for (int i = 0; i < n; i++) s.boxes.push_back(ofRectangle(s.track.x + i * (segW + g), s.track.y, segW, rh));
        }
    }
    if (cfgLayout < 1.5f) lastLayout = (cfgLayout >= 0.5f) ? 1 : 0;   // IMAGE (Type 2) doesn't own a layout-state slot
}

//--------------------------------------------------------------
void ofApp::setupAudio() {
    ofSoundStreamSettings s;
    auto devices = stream.getDeviceList();
    int chosen = -1;
    if (!sInputDevice.empty()) {                        // an explicit device was picked in ROUTING — try it first
        for (size_t i = 0; i < devices.size(); i++)
            if (devices[i].inputChannels > 0 && devices[i].name == sInputDevice) { chosen = (int)i; break; }
        if (chosen < 0) ofLogWarning() << "saved input device \"" << sInputDevice << "\" not found — falling back to auto-detect";
    }
    // prefer a virtual loopback device by name (cross-platform): BlackHole (mac),
    // VB-Audio Cable / VoiceMeeter (Windows), PulseAudio monitor / JACK / loopback (Linux)
    if (chosen < 0) {
        const char* keys[] = { "blackhole", "cable", "vb-audio", "voicemeeter", "loopback", "monitor", "jack" };
        for (size_t i = 0; i < devices.size() && chosen < 0; i++) {
            if (devices[i].inputChannels <= 0) continue;
            std::string nm = ofToLower(devices[i].name);
            for (const char* k : keys) if (nm.find(k) != std::string::npos) { chosen = (int)i; break; }
        }
    }
    if (chosen < 0)                                    // fallback: first available input (route your loopback as default)
        for (size_t i = 0; i < devices.size(); i++) if (devices[i].inputChannels > 0) { chosen = (int)i; break; }
    if (chosen < 0) {
        deviceName = "(no signal)";
        captureChannels = 0; activeChannelOffset = 0; recChannels = 2;
        ofLogError() << "No audio input found — set up a loopback device (see README: Audio routing).";
        return;
    }
    s.setInDevice(devices[chosen]);
    deviceName = devices[chosen].name;
    // Request the device's whole channel bus (capped) rather than just the first stereo pair, so a
    // multi-channel interface (e.g. a Zoom L-8) can have any pair selected in software below — RtAudio's
    // per-stream channel offset isn't exposed through ofSoundStreamSettings, so we take everything and slice.
    int devCh = std::max(1, (int)devices[chosen].inputChannels);
    captureChannels = std::min(devCh, 16);
    applyChannelSelection();   // sets activeChannelOffset + recChannels from sInputChannelPair
    // Prefer the current rate if this device actually advertises support for it; otherwise adopt one
    // it does support. Forcing an unsupported rate (e.g. requesting 48000 on a device that only lists
    // 44100) throws kAudioDeviceUnsupportedFormatError on CoreAudio and opens a stream that produces
    // zero audio callbacks — no error surfaces to the rest of the app, it just silently goes quiet.
    int requestRate = sampleRate;
    if (!devices[chosen].sampleRates.empty()) {
        bool supported = false;
        for (auto r : devices[chosen].sampleRates) if ((int)r == requestRate) { supported = true; break; }
        if (!supported) requestRate = (int)devices[chosen].sampleRates.front();
    }
    s.setInListener(this);
    s.sampleRate = requestRate; s.numInputChannels = captureChannels; s.numOutputChannels = 0; s.bufferSize = bufferSize;
    stream.setup(s);
    int sr = (int)stream.getSampleRate();              // adopt the device's actual rate
    sampleRate = (sr > 0) ? sr : requestRate;
    ofLogNotice() << "input: " << deviceName << " ch " << (activeChannelOffset + 1) << "-" << (activeChannelOffset + recChannels)
                  << " of " << captureChannels << " @ " << sampleRate << " Hz";
}

// ROUTING tab's "REFRESH DEVICE LIST" button. getDeviceList() already re-queries CoreAudio/RtAudio
// fresh on every call (buildFields() does this every time the settings dialog opens), but some
// CoreAudio setups only actually re-probe the hardware graph once every stream holding it open is
// released — plugging in an interface (e.g. a Zoom L-8) can otherwise stay invisible until the app
// restarts. Closing + reopening the stream forces that rescan.
void ofApp::refreshAudioDevices() {
    stream.close();
    buildFields();   // re-enumerates devices fresh into audioDeviceChoices
    setupAudio();    // reopen — same saved device/channels if still present, else auto-detect
}

// Move the read window over the already-open stream — the stream captures the device's WHOLE channel
// bus (see setupAudio), so switching which stereo pair the visualizer/recorder/broadcast hears needs
// no stream re-open (instant, no audio glitch). Only a DEVICE change needs a re-open.
void ofApp::applyChannelSelection() {
    if (captureChannels <= 0) { activeChannelOffset = 0; recChannels = 2; return; }
    int maxPair = std::max(0, (captureChannels - 1) / 2);
    int pair = ofClamp(sInputChannelPair, 0, maxPair);
    activeChannelOffset = std::min(pair * 2, std::max(0, captureChannels - 1));
    recChannels = std::min(2, captureChannels - activeChannelOffset);
}

//--------------------------------------------------------------
void ofApp::audioIn(ofSoundBuffer & input) {
    const size_t frames = input.getNumFrames(), ch = input.getNumChannels();
    if (ch == 0) return;
    // Only read the selected channel(s) — the stream may carry the device's whole bus (see setupAudio),
    // so a multi-channel interface's unused channels are simply ignored here.
    size_t c0 = std::min((size_t)activeChannelOffset, ch - 1);
    size_t c1 = std::min(c0 + (size_t)std::max(1, recChannels), ch);
    std::lock_guard<std::mutex> lock(mtx);
    float sum = 0, bufPeak = 0;
    for (size_t i = 0; i < frames; i++) {
        float m = 0;
        for (size_t c = c0; c < c1; c++) {
            float smp = input.getSample(i, c);
            m += smp;
            if (recording || broadcasting) {
                short s16 = (short)(ofClamp(smp, -1.f, 1.f) * 32767);
                if (recording)    recAudio.push_back(s16);
                if (broadcasting) broadcastAudioQueue.push_back(s16);
            }
        }
        m /= (float)(c1 - c0);
        ringBuf[writePos] = m; writePos = (writePos + 1) % N;
        sum += m * m;
        bufPeak = std::max(bufPeak, fabsf(m));
    }
    rms = sqrtf(sum / (float)frames);
    // Peak-hold with slow decay so a transient briefly holds then falls back — a musical VU-style peak.
    peak = std::max(bufPeak, peak * 0.92f);
}

//--------------------------------------------------------------
void ofApp::computeFFT(const std::vector<float>& in, std::vector<float>& outMag) {
    static std::vector<float> re, im;
    re.assign(in.begin(), in.end());
    im.assign(N, 0.0f);
    for (int i = 0; i < N; i++) re[i] *= 0.5f * (1.0f - cosf(TWO_PI * i / (N - 1)));
    int j = 0;
    for (int i = 0; i < N - 1; i++) {
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        int m = N >> 1;
        while (m >= 1 && j >= m) { j -= m; m >>= 1; }
        j += m;
    }
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -TWO_PI / len, wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < N; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                float tr = cr * re[b] - ci * im[b], ti = cr * im[b] + ci * re[b];
                re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                float ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
    outMag.resize(N / 2);
    for (int i = 0; i < N / 2; i++) outMag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]) / (N * 0.5f);
}

//--------------------------------------------------------------
void ofApp::update() {
    float dt = std::min(ofGetLastFrameTime(), 0.05);
    t += dt;
    // IMAGE type (Type == IMAGE): rescan on entry, re-pack the panel on any enter/leave, drive the slideshow.
    bool imgActive = cfgLayout >= 1.5f;
    if (imgActive != imgWasActive) {                        // entered or left IMAGE
        if (imgActive) loadImages();                       // entering → pick up newly-added files
        relayout();                                        // swap the panel between image controls and the visualizer params
    }
    imgWasActive = imgActive;
    if (imgActive && imgList.size() > 0) {
        if (imgFade < 1.0f) imgFade = std::min(1.0f, imgFade + dt / std::max(0.1f, cfgImgTrans));   // advance the crossfade
        if (cfgImgAuto >= 0.5f && imgList.size() > 1 && (t - imgHoldT) > cfgImgInterval) imageAdvance(1);  // auto-cycle (opt-in)
    }
    std::vector<float> w(N);
    { std::lock_guard<std::mutex> lock(mtx); for (int i = 0; i < N; i++) w[i] = ringBuf[(writePos + i) % N]; }
    computeFFT(w, spectrum);
    for (int i = 0; i < N / 2; i++) {
        if (spectrum[i] > spectrumSmooth[i]) spectrumSmooth[i] = spectrum[i];
        else spectrumSmooth[i] = ofLerp(spectrumSmooth[i], spectrum[i], 0.16f);
    }
    int maxBin = N / 4;
    for (int k = 0; k < BANDS; k++) {
        int lo = (int)((float)k / BANDS * maxBin), hi = (int)((float)(k + 1) / BANDS * maxBin);
        float v = 0; int c = 0;
        for (int i = lo; i <= hi && i < N / 2; i++) { v += spectrumSmooth[i]; c++; }
        if (c) v /= c;
        float tilt = 1.0f + 2.6f * powf((float)k / std::max(1, BANDS - 1), 1.25f);   // treble tilt: high-freq bands (outer rings / helix highs) react far more visibly
        v = std::min(v * fftGain * tilt, 150.0f);
        bands[k] *= cfgRate;
        if (bands[k] < v) bands[k] = v;
    }
    auto energy = [&](float fLo, float fHi, float gain) {
        int lo = std::max(1, (int)(fLo * N / sampleRate)), hi = std::min(N / 2 - 1, (int)(fHi * N / sampleRate));
        float s = 0; for (int i = lo; i <= hi; i++) s += spectrumSmooth[i];
        return ofClamp(s / std::max(1, hi - lo + 1) * gain, 0.0f, 1.0f);
    };
    level = levelE.process(ofClamp(rms * 5.0f, 0, 1));
    bass  = bassE.process(energy(30, 180, 22));
    mid   = midE.process(energy(180, 2000, 26));
    high  = highE.process(energy(1800, 15000, 85));   // hotter + wider so hats/air/filter-sweeps read clearly in the highs register
    float bInst = energy(30, 180, 22);
    if (bInst > prevBass * 1.35f && bInst > 0.16f) beatEnv = 1.0f;
    prevBass = ofLerp(prevBass, bInst, 0.10f);
    beatEnv = std::max(0.0f, beatEnv - dt / 0.45f);

    // ---- primary sub-frequency detection via autocorrelation (accurate at low pitches; FFT bins are too coarse here) ----
    if (bass > 0.06f) {
        static std::vector<float> P; P.assign(N + 1, 0.0f);              // prefix energy for O(1) normalisation
        for (int i = 0; i < N; i++) P[i + 1] = P[i] + w[i] * w[i];
        int minLag = std::max(2, (int)(sampleRate / 320.0f));           // search ~38..320 Hz
        int maxLag = std::min(N - 2, (int)(sampleRate / 38.0f));
        float best = 0.0f; int bestLag = 0;
        for (int lag = minLag; lag <= maxLag; lag++) {
            int n = N - lag; float ac = 0.0f;
            for (int i = 0; i < n; i++) ac += w[i] * w[i + lag];
            float nac = ac / (sqrtf(P[n] * (P[N] - P[lag])) + 1e-9f);    // normalised autocorrelation in [-1,1]
            if (nac > best) { best = nac; bestLag = lag; }
        }
        if (best > 0.5f && bestLag > 0) {                                // confident periodicity
            float freq = (float)sampleRate / (float)bestLag;
            subFreq = (subFreq <= 0.0f) ? freq : ofLerp(subFreq, freq, 0.20f);   // smooth the Hz readout
            static const char* NN[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
            int pc = (((int)lroundf(69.0f + 12.0f * log2f(subFreq / 440.0f)) % 12) + 12) % 12;
            subNote = NN[pc];                                            // note letter only (no octave)
        }
    }

    rotationX += cfgRotX; rotationY += cfgRotY; rotationZ += cfgRotZ;   // SPIN
    camX += cfgCamX; camY += cfgCamY; camZ += cfgCamZ;                   // CAMERA
    applyMods();
    if (cfgLayout < 1.5f) {   // RADIAL/GRID keep independent settings; IMAGE (Type 2) has none — skip the swap
        int cur = (cfgLayout >= 0.5f) ? 1 : 0;
        if (cur != lastLayout) {
            if (lastLayout >= 0 && lastLayout <= 1) saveLayoutState(lastLayout);
            loadLayoutState(cur);
            relayout();   // also sets lastLayout = cur
        }
    }

    // stars drift slowly downward; wrap from bottom back to the top (continuous)
    for (auto& s : stars) {
        s.p.y += (8.0f + s.r * 5.0f) * dt;        // very slow; bigger stars fall a touch faster
        if (s.p.y > RH) { s.p.y -= RH; s.p.x = ofRandom(RW); }
    }

    if (autoRecTest && !didRecTest) {
        if (!recording && t > 1.5f) startRecording();
        else if (recording && t > 5.5f) { stopRecording(); didRecTest = true; autoRecTest = false; }
    }

    // BROADCAST — if the ffmpeg/icecast pipe died (a source drop, usually a network blip), RECONNECT
    // rather than end the broadcast. The listener hears a brief gap; the broadcast persists.
    if (broadcasting && !icePipe && t >= iceReconnectAt) {
        // Try to relaunch. NOTE: popen "succeeding" only means ffmpeg SPAWNED — it may still fail to reach
        // Icecast and die on the next write. So DON'T reset the backoff here; only a confirmed write does.
        if (openIcePipe()) ofLogNotice() << "BROADCAST: reconnecting…";
        iceReconnectAt = t + iceReconnectDelay;
        iceReconnectDelay = std::min(iceReconnectDelay * 1.7f, 10.0f);
    }
    // Drain audioIn()'s queued PCM into a main-thread backlog EVERY tick while broadcasting — even while
    // the pipe is down and reconnecting — so the queue never grows during an outage. The backlog is
    // bounded (~8s, oldest dropped); we write it to the pipe WITHOUT blocking only when the pipe is up.
    if (broadcasting) {
        { std::lock_guard<std::mutex> lock(mtx);
          if (!broadcastAudioQueue.empty()) {
            const char* b = reinterpret_cast<const char*>(broadcastAudioQueue.data());
            iceOutBuf.insert(iceOutBuf.end(), b, b + broadcastAudioQueue.size() * sizeof(short));
            broadcastAudioQueue.clear();
          }
        }
        const size_t CAP = (size_t)sampleRate * std::max(1, recChannels) * sizeof(short) * 8;
        if (iceOutBuf.size() > CAP) iceOutBuf.erase(iceOutBuf.begin(), iceOutBuf.end() - CAP);
        bool dead = false;
        if (icePipe) {
#ifndef _WIN32
            size_t off = 0;
            while (iceFd >= 0 && off < iceOutBuf.size()) {
                ssize_t n = ::write(iceFd, iceOutBuf.data() + off, iceOutBuf.size() - off);
                if (n > 0) { off += (size_t)n; continue; }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;   // pipe full — resume next tick, UI never blocks
                if (n < 0 && errno == EINTR) continue;                            // interrupted by a signal — retry, NOT a drop
                dead = true; break;                                               // EPIPE etc. — ffmpeg gone
            }
            if (off) {   // real data flowed → healthy
                iceOutBuf.erase(iceOutBuf.begin(), iceOutBuf.begin() + off);
                iceReconnectDelay = 1.0f;
                if (iceWasDown) { ofLogNotice() << ofGetTimestampString("%H:%M:%S") << "  BROADCAST reconnected (was down)"; iceWasDown = false; }
            }
#else
            if (!iceOutBuf.empty()) {
                if (fwrite(iceOutBuf.data(), 1, iceOutBuf.size(), icePipe) < iceOutBuf.size()) dead = true;
                else { iceReconnectDelay = 1.0f; if (iceWasDown) { ofLogNotice() << ofGetTimestampString("%H:%M:%S") << "  BROADCAST reconnected"; iceWasDown = false; } }
                iceOutBuf.clear();
            }
#endif
        }
        if (dead) {   // reap the dead pipe OFF-thread (pclose can block), then decide: auth = stop, else retry
            FILE* p = icePipe; icePipe = nullptr; iceFd = -1; iceOutBuf.clear();
            if (p) std::thread([p]{ GS_PCLOSE(p); }).detach();
            if (iceAuthFailed()) {   // bad/revoked credentials — the ONLY failure retrying can't fix
                ofLogError() << ofGetTimestampString("%H:%M:%S") << "  BROADCAST AUTH FAILED (401) — stopping. Re-register in S -> REGISTER.";
                errMsg = "BROADCAST STOPPED \xE2\x80\x94 AUTH FAILED (re-register)"; errFlash = t;
                stopBroadcast();
            } else {                 // any transient failure (5xx / timeout / refused / dropped) — keep retrying forever
                bcastReconnects++; iceWasDown = true;
                ofLogError() << ofGetTimestampString("%H:%M:%S") << "  BROADCAST source dropped (reconnect #" << bcastReconnects << ") — retrying, will keep trying while ON AIR";
                iceReconnectAt = t + iceReconnectDelay;
                iceReconnectDelay = std::min(iceReconnectDelay * 1.7f, 10.0f);
            }
        }
    }
    // Heartbeat — one log line a minute so an overnight/unattended run is reviewable in the morning.
    if (broadcasting && (t - lastBcastBeat) > 60.0f) {
        lastBcastBeat = t;
        ofLogNotice() << ofGetTimestampString("%H:%M:%S") << "  BROADCAST alive — uptime=" << (int)(t - broadcastStart)
                      << "s status=" << (bcastAway ? "AWAY" : "ON DECK") << " reconnects=" << bcastReconnects
                      << (icePipe ? "" : " [link DOWN, retrying]");
    }
    if (broadcasting && (t - lastSnapshotT) > SNAPSHOT_INTERVAL) { pushSnapshot(); lastSnapshotT = t; }

    // Non-blocking registration verify (fired in setup): poll for the /whoami result file.
    if (regVerifyPending) {
        std::string outFile = gsScratch("hg_whoami.json");
        if (ofFile::doesFileExist(outFile)) {
            std::ifstream in(outFile); std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!s.empty()) {
                regVerifyPending = false;
                ofJson r; try { r = ofJson::parse(s); } catch (...) {}
                if (!r.is_null() && r.contains("registered")) {
                    if (r.value("registered", false)) {
                        sArtist = r.value("artist", sArtist);   // server is authoritative on the display name
                        sRegistered = true; regStatus = "Registered as " + sArtist;
                    } else {
                        sRegistered = false;
                        regStatus = "Not registered on this server \xE2\x80\x94 enter an invite code to register.";
                    }
                }
                ofFile::removeFile(outFile);
            }
        }
        if (t - regVerifyT > 10.0f) regVerifyPending = false;   // gave up (offline) — keep the locally-saved state
    }
}

//--------------------------------------------------------------
void ofApp::drawSpace() {
    if (cfgLayout >= 0.5f) return;                       // GRID: flat black (no centre glow)
    ofMesh g; g.setMode(OF_PRIMITIVE_TRIANGLE_FAN);
    g.addVertex(glm::vec3(RW * 0.5f, RH * 0.50f, 0)); g.addColor(ofColor(6, 6, 6));   // RADIAL: very subtle centre glow
    float rad = sqrtf((float)RW * RW + (float)RH * RH) * 0.5f;
    int segs = 96;                                       // smoother fan -> less faceting/banding
    for (int k = 0; k <= segs; k++) {
        float a = TWO_PI * k / segs;
        g.addVertex(glm::vec3(RW * 0.5f + cosf(a) * rad, RH * 0.50f + sinf(a) * rad, 0));
        g.addColor(ofColor(0, 0, 0));
    }
    g.draw();
}

void ofApp::drawStars() {
    ofEnableBlendMode(OF_BLENDMODE_ADD);
    for (auto& s : stars) {
        float tw = s.base * (0.55f + 0.45f * sinf(t * 1.2f + s.ph));
        ofSetColor(cStar, (int)(tw * 255));
        ofDrawCircle(s.p.x, s.p.y, s.r);
    }
    ofEnableBlendMode(OF_BLENDMODE_ALPHA);
}

// GRID layout — visualSynth matrixPattern/stripePattern, with our primitive + audio pulse/colour
void ofApp::drawGrid() {
    int nb = std::max(1, (int)ofClamp(cfgBands, 1.f, (float)BANDS));
    int sides = (cfgMode < 0.5f) ? 0 : (cfgMode < 1.5f ? 3 : 4);
    int cx = (int)cfgCountX, cy = (int)cfgCountY;
    const float base = 60.0f;
    int  fmask  = (int)(cfgFill + 0.5f);
    bool doFill = (fmask & 2) != 0;
    bool doLine = (fmask & 1) != 0 || fmask == 0;        // neither selected -> fall back to outline
    ofSetCircleResolution(48);                           // grid can hold thousands of cells; lighter circles than the global 120
    ofSetLineWidth(cfgLineW * S);
    ofPushMatrix();
    ofTranslate(RW * 0.5f, RH * 0.50f, 0);
    ofScale(cfgScale * S, cfgScale * S);
    ofRotateZDeg(cfgGlobalRot);
    ofRotateXDeg(camX + camManX); ofRotateYDeg(camY + camManY); ofRotateZDeg(camZ + camManZ);   // CAMERA: rigid 3D tumble of the whole grid
    float hiAdd = high * 120.0f;                                                 // high freq -> greyscale brightening
    int idx = 0;
    for (int j = -cy; j <= cy; j++) {
        ofPushMatrix();
        if (cy > 0) { float scl = ofMap(j, -cy, cy, 1.0f - cfgPinch, 1.0f); ofScale(scl, scl); }
        ofTranslate(0, j * cfgTransY);
        ofRotateZDeg(j * cfgTwistY);
        for (int i = -cx; i <= cx; i++) {
            float v = bands[idx++ % nb];
            float vn = ofClamp(v / 55.0f, 0.0f, 1.0f);             // audio: pulse + colour
            float pulse = 1.0f + vn * (0.6f + cfgSpread * 0.28f)   // Spread slider scales the audio punch
                                + beatEnv * 0.18f * cfgPunch;      // Punch = how hard the beat kicks the grid
            float mod   = vn * 230.0f;
            ofSetColor((int)ofClamp(cfgR + mod + hiAdd, 0.f, 255.f), (int)ofClamp(cfgG + mod + hiAdd, 0.f, 255.f),
                       (int)ofClamp(cfgB + mod + hiAdd, 0.f, 255.f), (int)cfgAlpha);
            float nx = (cx > 0) ? (float)i / cx : 0.0f, ny = (cy > 0) ? (float)j / cy : 0.0f;
            float d;                                                     // 0 at centre -> 1 at the edge
            if      (cfgFalloff < 0.5f) d = sqrtf(nx * nx + ny * ny);    // EUCLID  — circular contours
            else if (cfgFalloff < 1.5f) d = fabsf(nx) + fabsf(ny);       // DIAMOND — manhattan contours
            else if (cfgFalloff < 2.5f) d = std::max(fabsf(nx), fabsf(ny)); // FRAME — square contours
            else                        d = sqrtf(nx * nx + ny * ny);    // REVERSE — euclid metric, width flipped below
            d = std::min(1.0f, d);
            float gtaper = (cfgFalloff >= 2.5f) ? ofLerp(0.19f, 1.0f, d) : ofLerp(1.0f, 0.19f, d);   // REVERSE -> edge thickest, centre thinnest
            ofSetLineWidth(cfgLineW * gtaper * S);                       // Line W taper (REVERSE flips it)
            ofPushMatrix();
            ofTranslate(i * cfgTransX, 0);
            ofRotateZDeg(i * cfgTwistX);
            ofTranslate(0, cfgShiftY);
            ofRotateZDeg(cfgTwist);                       // per-cell spin (Cam Z tumbles the whole grid above)
            ofScale(cfgScaleX * pulse, cfgScaleY * pulse);
            if (sides == 0) {
                if (doFill) { ofFill();   ofDrawCircle(0, 0, base); }
                if (doLine) { ofNoFill(); ofDrawCircle(0, 0, base); }
            } else {
                float vx[4], vy[4];
                for (int k = 0; k < sides; k++) { float a = TWO_PI * k / sides - HALF_PI; vx[k] = cosf(a) * base; vy[k] = sinf(a) * base; }
                if (doFill) { ofFill(); for (int k = 1; k < sides - 1; k++) ofDrawTriangle(vx[0], vy[0], vx[k], vy[k], vx[k + 1], vy[k + 1]); }   // triangle fan — NO tessellator (the old ofEndShape(true) tessellated every cell)
                if (doLine) { ofNoFill(); ofBeginShape(); for (int k = 0; k < sides; k++) ofVertex(vx[k], vy[k]); ofEndShape(true); }
            }
            ofPopMatrix();
        }
        ofPopMatrix();
    }
    ofPopMatrix();
    ofFill(); ofSetLineWidth(1.0f); ofSetCircleResolution(120);
}

// RADIAL layout — concentric FFT shapes, RGB + audio colour-mod, cumulative 3D rotation
void ofApp::drawPortal() {
    if (cfgMode >= 2.5f) { drawHelix(); return; }      // HELIX mode replaces the layout entirely
    if (cfgLayout >= 0.5f) { drawGrid(); return; }
    ofPushMatrix();
    ofTranslate(RW * 0.5f, RH * 0.50f, 0);     // centred on screen
    ofScale(cfgScale, cfgScale);               // global scale
    ofRotateXDeg(camX + camManX); ofRotateYDeg(camY + camManY); ofRotateZDeg(camZ + camManZ);   // CAMERA: rigid tumble of the whole stack (Spin twists each ring inside the loop)
    int  fmask  = (int)(cfgFill + 0.5f);
    bool doFill = (fmask & 2) != 0;
    bool doLine = (fmask & 1) != 0 || fmask == 0;        // neither selected -> fall back to outline
    int nb = (int)ofClamp(cfgBands, 1.f, (float)BANDS);
    int sides = (cfgMode < 0.5f) ? 0 : (cfgMode < 1.5f ? 3 : 4);   // 0 = circle, 3 = triangle, 4 = square
    float hiAdd = high * 120.0f;                                   // high freq -> greyscale brightening (no hue shift)
    for (int i = 0; i < nb; i++) {
        float v = bands[i];
        float vn = ofClamp(v / 55.0f, 0.0f, 1.0f);      // normalised band energy -> drives thickness + alpha (no longer size)
        float mod = fmodf(v * 1200.0f, 255.0f);
        // opacity fade inner->outer; Falloff selects the curve (EUCLID exp · DIAMOND linear · FRAME plateau-then-edge)
        float f = (float)i / std::max(1, nb - 1);                 // 0 = inner, 1 = outermost
        float o;
        if      (cfgFalloff < 0.5f) { float k = 3.2f; o = (expf(-k * f) - expf(-k)) / (1.0f - expf(-k)); }   // EUCLID — fast early fade
        else if (cfgFalloff < 1.5f) o = 1.0f - f;                                                            // DIAMOND — even/linear fade
        else if (cfgFalloff < 2.5f) o = 1.0f - powf(f, 2.5f);                                                // FRAME — stays full, drops at the edge
        else                        { float k = 3.2f; o = (expf(-k * f) - expf(-k)) / (1.0f - expf(-k)); }   // REVERSE — EUCLID fade (the WIDTH taper flips below)
        int a = std::min(255, (int)(cfgAlpha * o * ofLerp(0.30f, 1.0f, vn)) + (int)(high * 70.0f));   // band energy drives ALPHA — rings pop when their band is loud
        int Rr = (int)ofClamp(cfgR + mod + hiAdd, 0.f, 255.f),
            Gg = (int)ofClamp(cfgG + mod + hiAdd, 0.f, 255.f),
            Bb = (int)ofClamp(cfgB + mod + hiAdd, 0.f, 255.f);
        float taper = (cfgFalloff >= 2.5f) ? ofLerp(0.19f, 1.0f, f) : ofLerp(1.0f, 0.19f, f);   // REVERSE -> outward thickest, inward thinnest
        float lw  = cfgLineW * taper * (1.0f + vn * (0.6f + cfgSpread * 0.5f) + beatEnv * 0.4f * cfgPunch) * S;   // band energy + beat drive THICKNESS (Spread = amount)
        float rad = cfgCenter * i * S;                          // rings hold their size now — audio drives thickness/alpha instead of radius
        auto primLine = [&](float r) {                           // outline stroke (used by bloom + the line pass)
            if (sides == 0) ofDrawCircle(0, 0, r);
            else { ofBeginShape(); for (int kk = 0; kk < sides; kk++) { float ang = TWO_PI * kk / sides - HALF_PI; ofVertex(cosf(ang) * r, sinf(ang) * r); } ofEndShape(true); }
        };
        auto primFill = [&](float r) {                           // filled — triangle fan, NO tessellator
            if (sides == 0) ofDrawCircle(0, 0, r);
            else { float vx[4], vy[4]; for (int kk = 0; kk < sides; kk++) { float ang = TWO_PI * kk / sides - HALF_PI; vx[kk] = cosf(ang) * r; vy[kk] = sinf(ang) * r; }
                   for (int kk = 1; kk < sides - 1; kk++) ofDrawTriangle(vx[0], vy[0], vx[kk], vy[kk], vx[kk + 1], vy[kk + 1]); }
        };
        if (cfgGlow > 0.01f && doLine) {                         // BLOOM: additive halo under the crisp stroke
            ofNoFill();
            ofEnableBlendMode(OF_BLENDMODE_ADD);
            for (int p = 3; p >= 1; p--) {
                ofSetColor(Rr, Gg, Bb, (int)(a * 0.14f * cfgGlow));
                ofSetLineWidth(lw * (1.0f + p * 2.4f * cfgGlow));
                primLine(rad);
            }
            ofEnableBlendMode(OF_BLENDMODE_ALPHA);
        }
        if (doFill) { ofFill();   ofSetColor(Rr, Gg, Bb, a); primFill(rad); }
        if (doLine) { ofNoFill(); ofSetColor(Rr, Gg, Bb, a); ofSetLineWidth(lw); primLine(rad); }
        ofRotateXDeg(rotationX); ofRotateYDeg(rotationY); ofRotateZDeg(rotationZ + cfgTwist);
    }
    ofPopMatrix();
    ofFill(); ofSetLineWidth(1.0f);
}

// HELIX mode — an accurate B-DNA double helix.
//   Geometry (literature): pitch:diameter = 1.89 (34Å/18Å), 10 base pairs per turn, and the two
//   backbones are offset ~144° (not 180°) — that asymmetry is what creates the major (22Å) /
//   minor (12Å) grooves. Rendered with painter's-order depth occlusion for a true 3D read.
//   Controls: Scale=size · Centre=turns · Twist=groove offset · Line W=spiral thickness · Fill=beads ·
//             Alpha/RGB=colour · audio reaction: GRID via per-cell size · RADIAL via alpha+glow (fixed size) · Spin*=progressive twist · Cam*/GlobRot=rigid camera.
//   RADIAL layout: Bands = number of helices (1 = centred, >1 = sunburst of spokes).
//   GRID layout:   a full matrix of helices — Count-X/Y = grid size, Trans-X/Y = spacing,
//                  Twist-X/Y = per-column/row rotation, Pinch = row taper, Shift-Y = cell offset,
//                  Scale-X/Y = per-cell scale (+ each cell bounces to its own audio band).
void ofApp::drawHelix() {
    const int   nb = std::min(64, BANDS);                                // fixed audio-sampling detail along the strands
    const float PITCH_PER_DIAM = 1.89f;                                  // B-DNA pitch 34Å / diameter 18Å
    const int   BP_PER_TURN    = 10;                                     // base pairs (rungs) per full turn
    bool  grid    = cfgLayout >= 0.5f;
    bool  dynamic = cfgMode >= 3.5f;                                     // STATIC vs DYNAMIC differ ONLY in auto-motion; both react to audio
    float turns  = ofClamp(cfgCenter * 0.75f, 1.0f, 14.0f);              // Centre -> coil tightness (single helix / grid)
    float offset = ofDegToRad(180.0f - cfgTwist);                        // strands 180° out of phase (perfectly aligned); Twist skews toward asymmetric grooves
    float spin   = dynamic ? t * 1.1f : 0.0f;                            // auto coil advance — dynamic only (the ONLY difference)
    float hiAdd  = high * 120.0f + beatEnv * 60.0f * cfgPunch;           // high-freq lift + a beat FLASH so every kick pops
    int   Rc = (int)ofClamp(cfgR + hiAdd, 0.f, 255.f), Gc = (int)ofClamp(cfgG + hiAdd, 0.f, 255.f), Bc = (int)ofClamp(cfgB + hiAdd, 0.f, 255.f);
    bool  fill = (((int)(cfgFill + 0.5f)) & 2) != 0;   // FILL bit -> draw the nucleotide beads

    // ---- unit-helix size: GRID builds at fixed local units (the matrix transform scales each cell);
    //      RADIAL builds in pixels (single helix fills the frame, or shorter spokes radiating from a gap) ----
    float axisLen, axisStart; int RES, count = 1; bool spokes = false; float lwMul;
    if (grid) {
        axisLen = 270.0f; axisStart = -axisLen * 0.5f; RES = 44; lwMul = 0.6f;   // unscaled units
    } else {
        count = (int)ofClamp(roundf(cfgBands / 4.0f), 1.f, 32.f);                // Bands -> # of helices (~1 per 4 steps)
        float fullLen = (RH - 2.0f * fm) * 0.84f * ofClamp(cfgScale, 0.2f, 2.5f);
        spokes  = count > 1;
        RES = 240; lwMul = 1.0f;
        if (spokes) {                                                            // sunburst: Centre -> inner GAP, proportional so they stay aligned at any Scale
            turns = 3.0f; axisLen = fullLen * 0.5f; axisStart = cfgCenter * 0.028f * axisLen;
        } else {                                                                 // single, centred (Centre -> coil tightness)
            axisLen = fullLen; axisStart = -fullLen * 0.5f;
        }
    }
    float R = axisLen / (turns * PITCH_PER_DIAM * 2.0f);                 // radius derived => accurate pitch:diameter
    R *= (1.0f + beatEnv * 0.24f * cfgPunch);                            // Punch -> beat radius pulse (always)

    bool prog = !grid && !spokes;   // progressive twist for the single centred radial helix (mirrors the radial rings)
    auto sample = [&](float u, float off, float& x, float& y, float& z) {
        float ph = u * turns * TWO_PI + spin + off;
        float vn = ofClamp(bands[(int)(u * nb) % nb] / 55.0f, 0.f, 1.f);
        float r  = R * (1.0f + vn * (0.06f + cfgSpread * 0.05f));                         // Spread -> audio radius push (always)
        x = r * cosf(ph); z = r * sinf(ph); y = axisStart + u * axisLen;
        if (prog) {                                                                      // base fixed, twist accumulates to the tip
            float k = u * turns, c, s, nx, ny, nz;
            float ax = ofDegToRad(rotationX * k), ay = ofDegToRad(rotationY * k), az = ofDegToRad(rotationZ * k);
            c = cosf(az); s = sinf(az); nx = x * c - y * s; ny = x * s + y * c; x = nx; y = ny;   // about Z
            c = cosf(ay); s = sinf(ay); nx = x * c + z * s; nz = -x * s + z * c; x = nx; z = nz;   // about Y
            c = cosf(ax); s = sinf(ax); ny = y * c - z * s; nz = y * s + z * c; y = ny; z = nz;   // about X
        }
    };

    struct Seg { float z, x0, y0, x1, y1; int kind; };   // kind: 0 strand · 1 rung · 2 bead
    std::vector<Seg> segs;
    for (int s = 0; s < 2; s++) {                                        // the two sugar-phosphate backbones
        float off = s * offset;
        for (int i = 0; i < RES; i++) {
            float u0 = (float)i / RES, u1 = (float)(i + 1) / RES, x0, y0, z0, x1, y1, z1;
            sample(u0, off, x0, y0, z0); sample(u1, off, x1, y1, z1);
            segs.push_back({ (z0 + z1) * 0.5f, x0, y0, x1, y1, 0 });
        }
    }
    int rungs = std::max(1, (int)roundf(turns * BP_PER_TURN));           // 10 base pairs per turn
    for (int i = 0; i <= rungs; i++) {
        float u = (float)i / rungs, xa, ya, za, xb, yb, zb;
        sample(u, 0, xa, ya, za); sample(u, offset, xb, yb, zb);
        segs.push_back({ (za + zb) * 0.5f, xa, ya, xb, yb, 1 });
        if (fill) { segs.push_back({ za, xa, ya, 0, 0, 2 }); segs.push_back({ zb, xb, yb, 0, 0, 2 }); }   // nucleotide beads
    }
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.z < b.z; });   // back -> front

    // ---- batch the unit helix into VBO meshes ONCE per frame, then draw per instance (huge draw-call cut) ----
    //  depth cue is baked into per-vertex alpha; line width is per-group (strand vs rung), set once.
    static ofVboMesh mStrand, mRung, mBead, mGlowStrand, mGlowRung;
    mStrand.clear(); mRung.clear(); mBead.clear(); mGlowStrand.clear(); mGlowRung.clear();
    mStrand.setMode(OF_PRIMITIVE_LINES); mRung.setMode(OF_PRIMITIVE_LINES); mBead.setMode(OF_PRIMITIVE_POINTS);
    mGlowStrand.setMode(OF_PRIMITIVE_LINES); mGlowRung.setMode(OF_PRIMITIVE_LINES);
    for (auto& g : segs) {                                              // segs already sorted back->front (painter's order)
        float dn = ofMap(g.z, -R, R, 0.f, 1.f, true);
        ofFloatColor c(Rc / 255.f, Gc / 255.f, Bc / 255.f, ofClamp(cfgAlpha / 255.f * ofLerp(0.20f, 1.0f, dn) * (g.kind == 1 ? 0.7f : 1.0f), 0.f, 1.f));
        if (g.kind == 2) { mBead.addVertex(glm::vec3(g.x0, g.y0, 0)); mBead.addColor(c); }
        else { ofVboMesh& m = (g.kind == 1) ? mRung : mStrand;
               m.addVertex(glm::vec3(g.x0, g.y0, 0)); m.addColor(c);
               m.addVertex(glm::vec3(g.x1, g.y1, 0)); m.addColor(c);
               ofVboMesh& gm = (g.kind == 1) ? mGlowRung : mGlowStrand;   // colour-less copies (for per-spoke alpha + glow in radial)
               gm.addVertex(glm::vec3(g.x0, g.y0, 0)); gm.addVertex(glm::vec3(g.x1, g.y1, 0));
        }
    }
    auto drawOne = [&](float aud) {                                   // aud = per-instance audio 0..1
        if (!grid) {                                                  // RADIAL helix: react via ALPHA + GLOW at fixed size (no scaling) — exactly like the rings
            int aA = (int)ofClamp(cfgAlpha * ofLerp(0.30f, 1.0f, aud), 0.f, 255.f);   // per-spoke opacity: louder band -> more opaque, quiet -> dim
            if (cfgGlow > 0.01f && mGlowStrand.getNumVertices()) {                     // bloom halo, brighter on louder spokes
                ofEnableBlendMode(OF_BLENDMODE_ADD);
                for (int p = 2; p >= 1; p--) {
                    ofSetColor(Rc, Gc, Bc, (int)(cfgAlpha * (0.05f + 0.12f * aud) * cfgGlow));
                    ofSetLineWidth(cfgLineW * lwMul * (1.0f + p * 2.2f * cfgGlow) * S);
                    mGlowStrand.draw();
                }
                ofEnableBlendMode(OF_BLENDMODE_ALPHA);
            }
            ofSetColor(Rc, Gc, Bc, (int)(aA * 0.7f));                 // rungs (dimmer, behind)
            ofSetLineWidth(std::max(1.0f, 1.3f * lwMul * S));
            if (mGlowRung.getNumVertices()) mGlowRung.draw();
            ofSetColor(Rc, Gc, Bc, aA);                               // strands — Line W
            ofSetLineWidth(cfgLineW * lwMul * S);
            if (mGlowStrand.getNumVertices()) mGlowStrand.draw();
        } else {                                                      // GRID helix: colour-graded mesh (depth cue) + per-cell size from the caller
            ofSetLineWidth(std::max(1.0f, 1.3f * lwMul * S));
            if (mRung.getNumVertices())   mRung.draw();
            ofSetLineWidth(cfgLineW * lwMul * S);
            if (mStrand.getNumVertices()) mStrand.draw();
            if (fill && mBead.getNumVertices()) { glPointSize(cfgLineW * lwMul * 2.2f * S); mBead.draw(); }
        }
    };

    ofEnableBlendMode(OF_BLENDMODE_ALPHA);                              // proper opacity + real occlusion (rings use ADD; helix does not)
    ofPushMatrix();
    ofTranslate(RW * 0.5f, RH * 0.5f, 0);
    if (grid) {                                                          // full matrix transform (mirrors drawGrid)
        ofRotateXDeg(camX + camManX); ofRotateYDeg(camY + camManY); ofRotateZDeg(camZ + camManZ + cfgGlobalRot);   // CAMERA: rigid (like grid shapes)
        int cx = (int)cfgCountX, cy = (int)cfgCountY, idx = 0;
        ofScale(cfgScale * S, cfgScale * S);                             // global scale (also scales spacing)
        for (int j = -cy; j <= cy; j++) {
            ofPushMatrix();
            if (cy > 0) { float scl = ofMap(j, -cy, cy, 1.0f - cfgPinch, 1.0f); ofScale(scl, scl); }   // Pinch -> row taper
            ofTranslate(0, j * cfgTransY);
            ofRotateZDeg(j * cfgTwistY);                                 // Twist-Y -> per-row rotation
            for (int i = -cx; i <= cx; i++) {
                float vn = ofClamp(bands[idx++ % nb] / 55.0f, 0.f, 1.f);
                float pulse = 1.0f + vn * (0.22f + cfgSpread * 0.12f);   // each cell bounces to its own band (always)
                ofPushMatrix();
                ofTranslate(i * cfgTransX, 0);
                ofRotateZDeg(i * cfgTwistX);                             // Twist-X -> per-column rotation
                ofTranslate(0, cfgShiftY);                               // Shift-Y -> per-cell offset
                ofScale(cfgScaleX * pulse, cfgScaleY * pulse);           // Scale-X/Y -> per-cell scale (+ audio)
                drawOne(vn);
                ofPopMatrix();
            }
            ofPopMatrix();
        }
    } else {                                                            // radial: one centred helix, or a sunburst of spokes
        ofRotateXDeg(camX + camManX); ofRotateYDeg(camY + camManY); ofRotateZDeg(camZ + camManZ);   // CAMERA: rigid tumble (single helix + sunburst); Spin twists the single helix inside sample()
        for (int h = 0; h < count; h++) {                               // each spoke = AVERAGE of its spectral slice -> smooth & uniform (no single bin spiking out)
            float vn;
            if (spokes) {
                int center = (int)((h + 0.5f) / count * nb), half = std::max(2, nb / 18);   // FIXED-width window regardless of count -> no single bin spikes one spoke at high Bands
                int b0 = std::max(0, center - half), b1 = std::min(nb, center + half + 1); float ssum = 0;
                for (int b = b0; b < b1; b++) ssum += bands[b];
                vn = ofClamp(powf(ssum / (float)(b1 - b0) / 55.0f, 0.6f), 0.f, 1.f);   // mean energy, gently compressed so the spokes bounce evenly
            } else vn = ofClamp(level, 0.f, 1.f);
            ofPushMatrix();
            if (spokes) ofRotateZDeg(h * 360.0f / count);
            drawOne(vn);                                               // FIXED size — vn drives alpha + glow inside drawOne (no scaling)
            ofPopMatrix();
        }
    }
    ofPopMatrix();
    ofFill(); ofSetLineWidth(1.0f);
}

void ofApp::branch(float len, float angle, int depth) {
    float f = ofClamp(depth / 9.0f, 0.0f, 1.0f);
    float lv = ofClamp(level, 0.0f, 1.0f);
    // colour is the only audio reaction: grey (quiet) -> green (mid) -> purple (loud)
    ofColor grey(110, 110, 110), green(52, 245, 166), purple(150, 70, 230);
    ofColor col = (lv < 0.5f) ? grey.getLerped(green, lv * 2.0f) : green.getLerped(purple, (lv - 0.5f) * 2.0f);
    ofSetColor(col, (int)(165 * cfgTreeOpacity));      // opacity is user-set, not audio-driven         // subtle; blends into the bg when quiet
    ofSetLineWidth(ofMap(f, 0, 1, 2.0f, 0.8f) * S);
    ofDrawLine(0, 0, 0, -len);
    ofTranslate(0, -len);
    if (len > 8.0f * S) {
        float sway = cfgSway * 0.15f * sinf(t * 1.3f + depth * 0.6f);   // gentle constant wind (not audio)
        ofPushMatrix(); ofRotateDeg(ofRadToDeg(angle + sway));  branch(len * 0.70f, angle, depth + 1); ofPopMatrix();
        ofPushMatrix(); ofRotateDeg(ofRadToDeg(-angle + sway)); branch(len * 0.70f, angle, depth + 1); ofPopMatrix();
    }
}

void ofApp::addTree(float pos, float h, float spr) {
    ofPushMatrix();
    ofTranslate(RW * pos, RH - fm);              // rooted on the bottom frame line
    branch((cfgTreeLen + h) * S, cfgAngle + spr, 0);   // size is fixed; only colour reacts to audio
    ofPopMatrix();
}

void ofApp::drawForest() {
    ofEnableBlendMode(OF_BLENDMODE_ALPHA);        // alpha so the grey blends into the bg
    addTree(0.82f, +40, 0.10f);                   // a single, subtle tree on the right
    ofSetLineWidth(1.0f);
}

// ---- IMAGE mode ------------------------------------------------------------------------------------
// A slideshow that lives INSIDE the scene FBO (so it records + broadcasts). Drop images into
// ~/.heliograph/images/. GLOW blend makes dark image regions dissolve into the deep-space background
// (bright parts glow); SOFT blend feathers the rectangle edges into the bg with a radial alpha mesh.
void ofApp::loadImages() {
    imgList.clear();
    std::string folder = (!sImgDir.empty() && ofDirectory::doesDirectoryExist(sImgDir)) ? sImgDir : gsImagesDir();
    ofDirectory dir(folder);
    dir.allowExt("png"); dir.allowExt("jpg"); dir.allowExt("jpeg"); dir.allowExt("gif"); dir.allowExt("bmp"); dir.allowExt("tif"); dir.allowExt("tiff");
    dir.listDir(); dir.sort();
    // Load as NORMALIZED (GL_TEXTURE_2D) textures so texcoords are 0..1 — the feather mesh relies on that
    // (oF defaults to ARB rectangle textures under GL 2.1, whose texcoords are in pixels).
    bool arb = ofGetUsingArbTex();
    ofDisableArbTex();
    for (size_t i = 0; i < dir.size(); i++) {
        ofImage im;
        if (im.load(dir.getPath(i))) { imgList.push_back(im); }
    }
    if (arb) ofEnableArbTex();
    imgCur = imgPrev = 0; imgFade = 1.0f; imgHoldT = t; imgKenSeed = ofRandom(1000);
    ofLogNotice() << "IMAGE: loaded " << imgList.size() << " image(s) from " << folder;
}

// CLEAR: drop every loaded image and forget the chosen folder, so ADD IMAGES starts from a clean slate.
void ofApp::clearImages() {
    imgList.clear();
    imgCur = imgPrev = 0; imgFade = 1.0f; imgHoldT = t;
    sImgDir = "";
    writeSession();
    ofLogNotice() << "IMAGE: cleared all loaded images";
}

bool ofApp::pickImagesFolder() {                               // native folder chooser -> use that folder as the image source
    std::string start = (!sImgDir.empty()) ? sImgDir : gsImagesDir();
    ofFileDialogResult r = ofSystemLoadDialog("Choose a folder of images", true, start);
    if (r.bSuccess && !r.getPath().empty()) { sImgDir = r.getPath(); loadImages(); writeSession(); return true; }
    return false;
}

// Draw one image, fit-to-cover the frame, with optional radial feather (SOFT) or plain quad (GLOW).
void ofApp::drawImageTex(ofImage& im, float cx, float cy, float fw, float fh, float ang, float alpha, bool soft, float feather, const ofColor& tint) {
    if (!im.isAllocated() || alpha <= 0.001f) return;
    float iw = im.getWidth(), ih = im.getHeight();
    if (iw < 1 || ih < 1) return;
    float cover = std::max(fw / iw, fh / ih);                 // cover the frame, preserve aspect
    float w = iw * cover, h = ih * cover, hw = w * 0.5f, hh = h * 0.5f;
    ofPushMatrix();
    ofTranslate(cx, cy);
    ofRotateZDeg(ang);
    if (!soft) {                                              // solid: let ofImage handle texturing (correct on any GL)
        ofSetColor(tint, (int)ofClamp(alpha * 255.0f, 0, 255));
        im.draw(-hw, -hh, w, h);
    } else {                                                  // feathered: per-vertex-alpha grid dissolves the edges into the bg
        ofTexture& tex = im.getTexture();
        tex.bind();
        const int N = 16;
        ofMesh m; m.setMode(OF_PRIMITIVE_TRIANGLES);
        float inner = 1.0f - ofClamp(feather, 0, 0.98f);      // radius (0..1) where the fade begins
        for (int gy = 0; gy <= N; gy++) for (int gx = 0; gx <= N; gx++) {
            float u = gx / (float)N, v = gy / (float)N;
            float px = (u - 0.5f) * w, py = (v - 0.5f) * h;
            float r = std::min(1.0f, sqrtf((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) * 2.0f);
            float a = 1.0f - ofClamp((r - inner) / std::max(0.001f, 1.0f - inner), 0, 1);   // 1 at centre → 0 at edge
            a = a * a * (3 - 2 * a);                           // smoothstep
            m.addVertex({px, py, 0}); m.addTexCoord({u, v});   // NORMALIZED texcoords (images loaded with ARB disabled)
            m.addColor(ofColor(tint, (int)ofClamp(alpha * a * 255.0f, 0, 255)));
        }
        auto idx = [&](int x, int y){ return y * (N + 1) + x; };
        for (int gy = 0; gy < N; gy++) for (int gx = 0; gx < N; gx++) {
            m.addIndex(idx(gx, gy));   m.addIndex(idx(gx + 1, gy));   m.addIndex(idx(gx, gy + 1));
            m.addIndex(idx(gx + 1, gy)); m.addIndex(idx(gx + 1, gy + 1)); m.addIndex(idx(gx, gy + 1));
        }
        m.draw();
        tex.unbind();
    }
    ofPopMatrix();
}

void ofApp::imageAdvance(int dir) {
    if (imgList.size() < 2) return;
    imageGoto(((imgCur + dir) % (int)imgList.size() + (int)imgList.size()) % (int)imgList.size());
}
void ofApp::imageGoto(int idx) {
    if (imgList.empty()) return;
    idx = ofClamp(idx, 0, (int)imgList.size() - 1);
    if (idx == imgCur && imgFade >= 1.0f) return;             // already showing it
    imgPrev = imgCur; imgCur = idx;
    imgFade = 0.0f; imgHoldT = t; imgKenSeed = ofRandom(1000);
}

void ofApp::drawImageMode() {
    // Cover the FULL frame (edge-to-edge), not an fm-inset box — otherwise, as the Ken-Burns zoom
    // breathes back toward scale 1.0 the cover stops overshooting and a black margin appears in the
    // broadcast/snapshot ("cropped inner"). Full-frame cover keeps the image full-bleed always.
    float W = RW, H = RH, cx = RW * 0.5f, cy = RH * 0.5f;
    if (imgList.empty()) return;   // no images → recorded scene stays clean (just the background); the
                                   // "add a folder" hint is drawn SCREEN-ONLY in draw(), never captured
    // Ken Burns: slow zoom + drift over the hold, varied per image via imgKenSeed.
    float held = t - imgHoldT;
    float kb   = cfgImgKen * 0.12f;
    float zoom = 1.0f + kb * (0.5f + 0.5f * sinf(held * 0.15f + imgKenSeed));
    float driftX = kb * 60 * S * sinf(held * 0.11f + imgKenSeed * 1.7f);
    float driftY = kb * 40 * S * cosf(held * 0.09f + imgKenSeed * 2.3f);
    // Audio-reactive pulse on opacity + scale.
    float pulse = 1.0f + cfgImgAudio * level;
    float baseA = ofClamp(cfgImgOpacity, 0, 1) * ofClamp(pulse, 0, 1.6f);
    float scale = cfgImgScale * zoom * (1.0f + cfgImgAudio * level * 0.15f);
    float panX  = cfgImgPanX * W * 0.5f + driftX;
    float panY  = cfgImgPanY * H * 0.5f + driftY;
    bool  soft  = cfgImgBlend >= 0.5f;
    ofEnableBlendMode(soft ? OF_BLENDMODE_ALPHA : OF_BLENDMODE_ADD);
    // Brightness + optional tint toward the channel accent.
    float br = ofClamp(cfgImgBright, 0, 2);
    ofColor own(ofClamp(255 * br, 0, 255), ofClamp(255 * br, 0, 255), ofClamp(255 * br, 0, 255));
    ofColor tinted = own.getLerped(ofColor(cNeon.r * br, cNeon.g * br, cNeon.b * br), ofClamp(cfgImgTint, 0, 1));
    float fw = W * scale, fh = H * scale;
    // Crossfade: outgoing fades out, incoming fades in.
    if (imgFade < 1.0f && imgPrev != imgCur)
        drawImageTex(imgList[imgPrev], cx + panX, cy + panY, fw, fh, cfgImgRot, baseA * (1.0f - imgFade), soft, cfgImgFeather, tinted);
    drawImageTex(imgList[imgCur], cx + panX, cy + panY, fw, fh, cfgImgRot, baseA * imgFade, soft, cfgImgFeather, tinted);
    ofEnableBlendMode(OF_BLENDMODE_ALPHA);
}

// Screen-only IMAGE UI: the ADD button + a bottom thumbnail carousel. Drawn AFTER the scene FBO (in
// draw()), so it is NEVER part of the recording, broadcast, or snapshots. Hit rects are in FBO coords
// (mousePressed maps clicks into the same space).
void ofApp::drawImageBar() {
    imgThumbBox.clear();
    float barH = 92 * S, pad = 12 * S, by = RH - fm - barH;
    // toggle handle — always shown so you can collapse/expand the carousel
    float thW = 150 * S, thH = 22 * S;
    imgBarToggle = ofRectangle(RW * 0.5f - thW * 0.5f, by - thH - 4 * S, thW, thH);
    ofSetColor(22, 25, 28, 235); ofDrawRectangle(imgBarToggle);
    ofNoFill(); ofSetLineWidth(1 * S); ofSetColor(90, 98, 94); ofDrawRectangle(imgBarToggle); ofFill();
    ofSetColor(184, 190, 186);
    std::string tlab = imgBarOpen ? "HIDE  IMAGES" : "IMAGES  (" + ofToString(imgList.size()) + ")";
    ofRectangle tb = fUI.getStringBoundingBox(tlab, 0, 0);
    fUI.drawString(tlab, floorf(imgBarToggle.x + (thW - tb.width) * 0.5f - tb.x), floorf(imgBarToggle.y + (thH - tb.height) * 0.5f - tb.y));
    imgAddBox = ofRectangle(-99999, -99999, 0, 0);   // ADD IMAGES now lives in the right panel under Type
    if (!imgBarOpen) return;
    ofSetColor(14, 16, 18, 225); ofDrawRectangle(0, by, RW, barH);
    ofSetColor(58, 64, 62); ofDrawLine(0, by, RW, by);
    float thumbH = barH - 2 * pad;
    // thumbnails — fit all in the remaining width (shrink to fit; contain each image, current one accented)
    int n = (int)imgList.size();
    if (n == 0) { ofSetColor(120, 126, 122); std::string h = "no images — add a folder from the panel (C)  ·  Type ▸ IMAGE";
                  fUI.drawString(h, fm + pad, by + barH * 0.5f + 4 * S); return; }
    float stripX = fm + pad, availW = RW - fm - pad - stripX, gap = 8 * S;
    float thumbW = ofClamp((availW - gap * (n - 1)) / n, 24 * S, 130 * S);
    float x = stripX;
    for (int i = 0; i < n; i++) {
        ofRectangle tr(x, by + pad, thumbW, thumbH);
        imgThumbBox.push_back(tr);
        ofSetColor(8, 9, 10); ofDrawRectangle(tr);                         // letterbox backing
        ofImage& im = imgList[i];
        if (im.isAllocated() && im.getWidth() > 0) {
            float con = std::min(tr.width / im.getWidth(), tr.height / im.getHeight());
            float dw = im.getWidth() * con, dh = im.getHeight() * con;
            ofSetColor(255); im.draw(tr.x + (tr.width - dw) * 0.5f, tr.y + (tr.height - dh) * 0.5f, dw, dh);
        }
        bool cur = (i == imgCur);
        ofNoFill(); ofSetLineWidth((cur ? 2.2f : 1.0f) * S); ofSetColor(cur ? cNeon : ofColor(70, 76, 74)); ofDrawRectangle(tr); ofFill();
        x += thumbW + gap;
    }
}

void ofApp::drawScene() {
    drawSpace();
    drawStars();
    if (cfgLayout >= 1.5f) { drawImageMode(); return; }   // IMAGE type replaces the portal visualizer
    ofEnableBlendMode(OF_BLENDMODE_ADD);
    drawPortal();
    ofEnableBlendMode(OF_BLENDMODE_ALPHA);
    // drawForest();   // forest disabled for now (uncomment to re-enable the tree)
}

//--------------------------------------------------------------
void ofApp::draw() {
    fboFinal.begin();
    ofClear(0, 0, 0, 255);                        // black outside the frame
    glEnable(GL_SCISSOR_TEST);                    // keep all scene content inside the frame
    glScissor((int)fm, (int)fm, (int)(RW - 2 * fm), (int)(RH - 2 * fm));
    drawScene();
    glDisable(GL_SCISSOR_TEST);
    if (showHud) drawHud();                       // 'U' toggles the broadcast HUD (also affects the recording)
    fboFinal.end();   // RECORDED content = scene + HUD only; the control menus draw to the screen below, so they're never captured

    ofRectangle r = displayRect();                       // letterboxed: preserves 16:9 at any window/fullscreen size
    ofSetColor(255);
    ofEnableBlendMode(OF_BLENDMODE_DISABLED);
    fboFinal.draw(r.x, r.y, r.width, r.height);
    ofEnableBlendMode(OF_BLENDMODE_ALPHA);

    // control menus — SCREEN ONLY, drawn in FBO space AFTER the recorded frame is finalised. They're visible live
    // but never appear in the MP4, so you can open the panel and tweak configs mid-recording without capturing it.
    ofPushMatrix();
    ofTranslate(r.x, r.y); ofScale(r.width / (float)RW, r.height / (float)RH);
    if (settingsOpen)   drawSettings();    // 'e' — edit session content
    else if (showPanel) drawPanels();      // 'g' — config panel
    if (helpMode && showPanel && !settingsOpen) drawParamHelp();   // 'i' — hover-help over the live controls
    if (cfgLayout >= 1.5f && !settingsOpen) {                 // IMAGE type: screen-only carousel + empty-state hint (never recorded)
        if (imgList.empty()) {                                // guidance shown to the artist only — kept out of the recorded frame
            ofSetColor(150, 156, 154);
            std::string h1 = "IMAGE";
            std::string h2 = "open the control panel (C) and click  + ADD IMAGES  to choose a folder";
            fTitle.drawString(h1, RW * 0.5f - fTitle.stringWidth(h1) * 0.5f, RH * 0.5f - 10 * S);
            fUI.drawString(h2, RW * 0.5f - fUI.stringWidth(h2) * 0.5f, RH * 0.5f + 24 * S);
        }
        drawImageBar();
    }
    // Broadcaster PRESENCE badge — screen-only (never recorded), top-right, always visible while ON AIR
    // so you can see your status at a glance. Toggle with 'A'.
    if (broadcasting) {
        std::string s = bcastAway ? "AWAY" : "ON DECK";
        float pad = 13 * S, dotR = 5 * S, gap = 9 * S, bh = 32 * S;
        ofRectangle tb = fUI.getStringBoundingBox(s, 0, 0);
        float bw = pad + dotR * 2 + gap + tb.width + pad;
        float bx = RW - fm - bw, by = fm;
        ofSetColor(12, 14, 16, 225); ofDrawRectRounded(bx, by, bw, bh, 5 * S);
        ofNoFill(); ofSetLineWidth(1 * S); ofSetColor(74, 80, 78); ofDrawRectRounded(bx, by, bw, bh, 5 * S); ofFill();
        ofSetColor(bcastAway ? ofColor(214, 150, 60) : ofColor(120, 210, 140));   // AWAY = amber · ON DECK = green
        ofDrawCircle(bx + pad + dotR, by + bh * 0.5f, dotR);
        ofSetColor(224, 229, 226);
        fUI.drawString(s, floorf(bx + pad + dotR * 2 + gap), floorf(by + (bh - tb.height) * 0.5f - tb.y));
    }
    if (showHelp)       drawHelp();        // 'h' — shortcuts overlay
    ofPopMatrix();

    // REC indicator — live only (not in the MP4), mapped onto the content rect
    if (recording) {
        ofPushMatrix();
        ofTranslate(r.x, r.y); ofScale(r.width / (float)RW, r.height / (float)RH);
        float ry = fm - 16 * S, bl = 0.5f + 0.5f * sinf(t * 4.0f);
        auto pd = [](int v){ return (v < 10 ? "0" : "") + ofToString(v); };
        ofSetColor(255, 80, 80, (int)(120 + 135 * bl)); ofDrawCircle(fm + 6 * S, ry - 5 * S, 6 * S);
        int secs = (int)(t - recStart);
        ofSetColor(231, 237, 232);
        fValue.drawString("REC  " + pd(secs / 60) + ":" + pd(secs % 60), fm + 22 * S, ry);
        ofPopMatrix();
    }

    // ON AIR indicator — live only (not captured), stacks below REC when both are active at once
    if (broadcasting) {
        ofPushMatrix();
        ofTranslate(r.x, r.y); ofScale(r.width / (float)RW, r.height / (float)RH);
        float ry = fm - 16 * S + (recording ? 26 * S : 0), bl = 0.5f + 0.5f * sinf(t * 4.0f);
        auto pd = [](int v){ return (v < 10 ? "0" : "") + ofToString(v); };
        ofSetColor(255, 190, 70, (int)(120 + 135 * bl)); ofDrawCircle(fm + 6 * S, ry - 5 * S, 6 * S);   // amber — distinct from REC's red
        int secs = (int)(t - broadcastStart);
        ofSetColor(231, 237, 232);
        std::string srv = sRegServer;                                    // show the server host next to ON AIR
        { size_t sc = srv.find("://"); if (sc != std::string::npos) srv = srv.substr(sc + 3);
          size_t sl = srv.find('/'); if (sl != std::string::npos) srv = srv.substr(0, sl); }
        std::string label = (srv.empty() ? "" : srv + "  \xC2\xB7  ") + "ON AIR  " + pd(secs / 60) + ":" + pd(secs % 60);
        fValue.drawString(label, fm + 22 * S, ry);
        ofPopMatrix();
    }

    // Audio level meter — 'V'. SCREEN ONLY (drawn after the FBO like the panels), so it sits in the bottom
    // margin OUTSIDE the recorded/broadcast frame and never appears in the video. A quick performance monitor.
    if (showMeter) {
        ofPushMatrix();
        ofTranslate(r.x, r.y); ofScale(r.width / (float)RW, r.height / (float)RH);
        float mw = RW * 0.5f, mh = 10 * S, mx = (RW - mw) * 0.5f, my = RH - fm * 0.5f - mh * 0.5f;
        ofSetColor(150, 156, 154); fUI.drawString("SIGNAL", mx, my - 8 * S);
        ofSetColor(30, 34, 36); ofDrawRectangle(mx, my, mw, mh);           // track
        float lvl = ofClamp(level, 0.0f, 1.0f);
        ofSetColor(lvl > 0.02f ? cNeon : ofColor(70, 76, 74));
        if (lvl > 0.001f) ofDrawRectangle(mx, my, mw * lvl, mh);           // fill
        ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(92, 100, 96); ofDrawRectangle(mx, my, mw, mh); ofFill();
        ofPopMatrix();
    }

    // "file exists" overwrite warning (window-only; recording hasn't started)
    if (recWarn) {
        float bl = 0.5f + 0.5f * sinf(t * 5.0f);
        std::string w = "FILE EXISTS — press R to overwrite (retake)   ·   ESC to cancel";
        float tw = fValue.stringWidth(w), bx = r.x + r.width * 0.5f, by = r.y + r.height * 0.5f;
        ofSetColor(0, 0, 0, 210); ofDrawRectangle(bx - tw * 0.5f - 24, by - 34, tw + 48, 60);
        ofSetColor(255, 150, 90, (int)(180 + 75 * bl)); fValue.drawString(w, bx - tw * 0.5f, by + 6);
    }

    if (recording && vidPipe) {
        // capture the (downscaled) record frame once per app-frame
        fboRec.begin();
        ofClear(0, 0, 0, 255);
        ofSetColor(255); ofEnableBlendMode(OF_BLENDMODE_DISABLED);
        fboFinal.draw(0, 0, REC_W, REC_H);                 // 4K -> 1440p downscale
        ofEnableBlendMode(OF_BLENDMODE_ALPHA);
        fboRec.end();
        fboRec.readToPixels(recPixels);
        // keep the constant-30fps video timeline locked to wall-clock so it stays synced with the real-time audio
        long target = (long)((t - recStart) * 30.0);
        int guard = 0;
        while (recFrames < target && guard++ < 5) {        // dupe frames to catch up if we briefly lag (no extra readback)
            fwrite(recPixels.getData(), 1, (size_t)REC_W * REC_H * 4, vidPipe);
            recFrames++;
        }
    }
    if (autoShot && t > 8.0f) { ofSaveScreen(gsScratch("heliograph_frame.png")); autoShot = false; }
}

//--------------------------------------------------------------
void ofApp::drawHud() {
    const int W = RW, H = RH;
    const float m = fm, k = 16 * S;
    ofColor lab(110, 122, 116), val(231, 237, 232), note(150, 170, 158);

    // corner ticks = the content frame
    ofSetColor(96, 112, 104, 170); ofSetLineWidth(1.5f * S);
    ofDrawLine(m, m, m + k, m);                 ofDrawLine(m, m, m, m + k);
    ofDrawLine(W - m, m, W - m - k, m);         ofDrawLine(W - m, m, W - m, m + k);
    ofDrawLine(m, H - m, m + k, H - m);         ofDrawLine(m, H - m, m, H - m - k);
    ofDrawLine(W - m, H - m, W - m - k, H - m); ofDrawLine(W - m, H - m, W - m, H - m - k);

    float ins = 18 * S;                                                       // inset off the frame edges
    auto rx  = [&](ofTrueTypeFont& f, const std::string& s){ return W - m - ins - f.stringWidth(s); };
    auto pad = [&](int v, int w){ std::string s = ofToString(v); while ((int)s.size() < w) s = "0" + s; return s; };
    // ===== top bar: SIGNAL (left) · TRANSMISSION (right, large) — wordmark now lives bottom-right =====
    float yTop = m + fTitle.getAscenderHeight() + 8 * S;
    float lx = m + ins;

    // All the SIGNAL + session-metadata text below is gated on showMeta ('T', default OFF): the frame
    // that gets recorded AND pushed to the client stays a clean visualizer, and HGReceptorMk1 renders
    // the telemetry itself from the metadata heliograph sends. Only the corner ticks above are always drawn.
    if (showMeta) {
    // SIGNAL — NONE (no audio · dim, static) overrules · AUDITION (audio, grey, static) · LIVE (audio+recording/broadcasting, white, pulsing) — top-left
    {
        float gap = 24 * S;
        bool audio = level > 0.008f;                          // is any audio actually coming in?
        bool live  = audio && (recording || broadcasting);    // NONE overrules: no audio => NONE even while recording/broadcasting
        const char* st = !audio ? "NONE" : (live ? "LIVE" : "AUDITION");
        ofColor stateCol = !audio ? ofColor(96, 100, 98) : (live ? ofColor(240, 244, 241) : ofColor(140, 146, 143));
        ofSetColor(238, 242, 239); fValue.drawString("SIGNAL", lx, yTop);
        float wS = fValue.stringWidth("SIGNAL");
        float pulse = live ? (0.30f + 0.70f * (0.5f + 0.5f * sinf(t * 2.6f))) : 1.0f;   // dot pulses only while LIVE
        ofSetColor(stateCol, (int)(255 * pulse));
        ofDrawCircle(lx + wS + gap * 0.5f, yTop - fValue.getAscenderHeight() * 0.32f, 6 * S);
        ofSetColor(stateCol); fValue.drawString(st, lx + wS + gap, yTop);
    }
    // TRANSMISSION Nº 001 — top-right, large; artist · date beneath it
    {
        std::string s = ofToUpper(sChannel) + " Nº " + pad(sTransmission, 3);   // Channel + Episode (both user-set)
        ofSetColor(val); fTitle.drawString(s, W - m - ins - fTitle.stringWidth(s), yTop);
        std::string meta = ofToUpper(sArtist) + "   ·   " + sDate;
        ofSetColor(lab); fValue.drawString(meta, W - m - ins - fValue.stringWidth(meta), yTop + 32 * S);
    }

    // ===== bottom-right: telemetry — ONLY while recording — KV pairs, fully right-aligned to the border =====
    if (recording) {
        long  ds = (long)(t - recStart);                                   // seconds recorded, shown as LY
        float lh = fValue.getLineHeight() * 1.32f;
        float r2 = H - m - 46 * S, r1 = r2 - lh, r0 = r1 - lh;             // three rows; bottom row (DIST) sits above the wordmark
        // HDG shown in degrees but derived from the detected sub frequency (pitch -> 0..360° bearing, wraps each octave)
        float pitch = 69.0f + 12.0f * log2f(std::max(1.0f, subFreq) / 440.0f);
        float hdgDeg = fmodf(fmodf(pitch, 12.0f) + 12.0f, 12.0f) / 12.0f * 360.0f;
        std::string vWpt = sWaypoint, vHdg = ofToString(hdgDeg, 0) + "\xC2\xB0 (" + subNote + ")", vDist = ofToString(ds) + " LY";
        float valR = W - m - ins;                                          // value column right edge = the frame border (flush)
        float maxV = std::max(std::max(fValue.stringWidth(vWpt), fValue.stringWidth(vHdg)), fValue.stringWidth(vDist));
        float keyR = valR - maxV - 28 * S;                                 // key column right edge, a fixed gap left of the values
        auto row = [&](float ry, const std::string& key, const std::string& val) {
            ofSetColor(150, 160, 156); fValue.drawString(key, keyR - fValue.stringWidth(key), ry);   // key (right-aligned)
            ofSetColor(238, 244, 240); fValue.drawString(val, valR - fValue.stringWidth(val), ry);   // value (right-aligned, flush to border)
        };
        row(r0, "WPT",  vWpt);
        row(r1, "HDG",  vHdg);
        row(r2, "DIST", vDist);

        // ===== bottom-left: title + note (only while recording) =====
        float lx2 = m + ins;
        ofSetColor(231, 237, 232); fNote.drawString(sTitle, lx2, r1);
        ofSetColor(150, 170, 158); fValue.drawString(sNote,  lx2, r2);
    }

    // ===== bottom-right wordmark (inside the border, always bottom-most): "HelioGraph Mk1" — ☉ replaces the o in heli·O·graph =====
    {
        std::string pre = "Heli", post = "Graph Mk1";
        float oPad = fBrand.stringWidth("o") * 0.62f;                           // extra air on each side of the sun
        float slot = fBrand.stringWidth("o") + 2.0f * oPad;                     // the slot the sun occupies (o-width + padding)
        float wPre = fBrand.stringWidth(pre), wPost = fBrand.stringWidth(post);
        ofRectangle ob = fBrand.getStringBoundingBox("o", 0, 0);                // x-height -> sun size
        ofRectangle cb = fBrand.getStringBoundingBox("H", 0, 0);                // cap height -> vertical centre (caps midline)
        float by = H - m - 14 * S;                                              // INSIDE the bottom border — always the bottom-most element
        float oCy = by + cb.y + cb.height * 0.5f;                               // shared vertical centre for the sun AND the CC mark
        float ccR = fUI.stringWidth("cc") * 0.62f, ccGap = 14 * S;              // CC mark sized to hold "cc"
        float total = ccR * 2 + ccGap + wPre + slot + wPost;                    // [cc] + Heli + ☉ + Graph Mk1
        float gx = W - m - ins - total;                                         // left edge of the whole group (right-aligned to the frame)
        float ccCx = gx + ccR, bx = gx + ccR * 2 + ccGap;                       // CC centre · wordmark start
        float oCx = bx + wPre + slot * 0.5f, oR = ob.height * 0.60f;
        // CC (Creative Commons) mark — ring + "cc", aligned with the sun's centre
        ofSetColor(198, 204, 201);
        ofNoFill(); ofSetLineWidth(1.6f * S); ofDrawCircle(ccCx, oCy, ccR); ofFill();
        { ofRectangle bb = fUI.getStringBoundingBox("cc", 0, 0);
          fUI.drawString("cc", floorf(ccCx - bb.width * 0.5f - bb.x), floorf(oCy - bb.height * 0.5f - bb.y)); }
        // wordmark
        fBrand.drawString(pre,  bx, by);
        fBrand.drawString(post, bx + wPre + slot, by);
        ofNoFill(); ofSetLineWidth(1.6f * S); ofDrawCircle(oCx, oCy, oR);        // ☉ ring
        ofFill(); ofDrawCircle(oCx, oCy, oR * 0.20f);                           // ☉ centre dot
    }
    }   // end if (showMeta)

    ofSetLineWidth(1.0f);
}

//--------------------------------------------------------------
void ofApp::drawPanels() {
    float rw = 320 * S, rx = RW - fm - 20 * S - rw;          // right panel column

    auto chip = [&](const ofRectangle& b, const std::string& tx, bool hot) {
        ofSetColor(hot ? ofColor(58, 70, 64) : ofColor(34, 38, 42)); ofDrawRectangle(b);
        ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(hot ? ofColor(206, 212, 208) : ofColor(92, 100, 96)); ofDrawRectangle(b); ofFill();
        ofSetColor(hot ? ofColor(236, 240, 237) : ofColor(186, 194, 190));
        ofRectangle bb = fUI.getStringBoundingBox(tx, 0, 0);
        fUI.drawString(tx, floorf(b.x + (b.width - bb.width) * 0.5f - bb.x), floorf(b.y + (b.height - bb.height) * 0.5f - bb.y));
    };
    auto miniSlider = [&](const ofRectangle& box, const std::string& name, float val, float lo, float hi, int prec, const std::string& unit) {
        ofSetColor(150, 156, 154); fUI.drawString(name, box.x, box.y - 6 * S);
        std::string vs = ofToString(val, prec) + unit;
        ofSetColor(214, 218, 216); fUI.drawString(vs, box.x + box.width - fUI.stringWidth(vs), box.y - 6 * S);
        ofSetColor(48, 50, 52); ofDrawRectangle(box);
        float fr = ofMap(val, lo, hi, 0, box.width, true);
        ofSetColor(150, 156, 154, 235); ofDrawRectangle(box.x, box.y, fr, box.height);
    };

    auto drawSlider = [&](const Slider& s) {
        if (!s.opts.empty()) {                                       // segmented radio buttons
            ofSetColor(158, 164, 162); fUI.drawString(s.name, s.track.x, s.track.y - 8 * S);
            int sel = (int)roundf(*s.val);
            for (size_t i = 0; i < s.boxes.size(); i++) {
                bool on = s.toggleMask ? (((sel >> (int)i) & 1) != 0) : ((int)i == sel);
                ofSetColor(on ? ofColor(6, 7, 9) : ofColor(46, 50, 54));
                ofDrawRectangle(s.boxes[i]);
                ofNoFill(); ofSetLineWidth(1.0f * S);
                ofSetColor(on ? ofColor(206, 212, 208) : ofColor(92, 100, 96));
                ofDrawRectangle(s.boxes[i]); ofFill();
                ofSetColor(on ? ofColor(242, 246, 243) : ofColor(150, 158, 154));
                if (s.icons) {                                                      // primitive shape icons ○ △ ▢ + helix
                    float cx = s.boxes[i].x + s.boxes[i].width * 0.5f, cy = s.boxes[i].y + s.boxes[i].height * 0.5f;
                    float r = s.boxes[i].height * 0.30f;
                    ofFill();
                    if (i == 0) ofDrawCircle(cx, cy, r);
                    else if (i == 1) { ofBeginShape(); for (int k = 0; k < 3; k++) { float a = TWO_PI * k / 3 - HALF_PI; ofVertex(cx + cosf(a) * r * 1.12f, cy + sinf(a) * r * 1.12f); } ofEndShape(true); }
                    else if (i == 2) { float h2 = r * 0.92f; ofDrawRectangle(cx - h2, cy - h2, h2 * 2, h2 * 2); }
                    else {
                        ofNoFill(); ofSetLineWidth(1.6f * S);
                        ofPolyline pa, pb; int K = 16;
                        for (int k = 0; k <= K; k++) { float ff = (float)k / K, yy = cy - r * 1.25f + ff * 2.5f * r, th = ff * TWO_PI; pa.addVertex(cx + sinf(th) * r * 0.8f, yy); pb.addVertex(cx + sinf(th + PI) * r * 0.8f, yy); }
                        pa.draw(); pb.draw();
                        if (i == 4) { ofSetLineWidth(1.3f * S); for (int m = 0; m < 2; m++) { float yy = cy - r * 0.45f + m * r * 0.9f; ofDrawLine(cx - r * 2.1f, yy, cx - r * 1.35f, yy); } }
                        ofFill();
                    }
                } else {
                    const std::string& o = s.opts[i];
                    ofRectangle bb = fUI.getStringBoundingBox(o, 0, 0);
                    fUI.drawString(o, floorf(s.boxes[i].x + (s.boxes[i].width - bb.width) * 0.5f - bb.x), floorf(s.boxes[i].y + (s.boxes[i].height - bb.height) * 0.5f - bb.y));
                }
            }
            return;
        }
        // modulated? then the static GREY fill shows the set value (base); a bright WHITE marker shows the live value
        int idx = (int)(&s - sliders.data());
        float base = *s.val; bool modded = false;
        for (int k = 0; k < 3; k++) { if (audioMod[k].dest == idx) { base = audioMod[k].base; modded = true; } if (lfoMod[k].dest == idx) { base = lfoMod[k].base; modded = true; } }
        ofSetColor(158, 164, 162); fUI.drawString(s.name, s.track.x, s.track.y - 8 * S);
        std::string vs = ofToString(modded ? base : *s.val, s.prec);     // show the set value (stable) when modulated
        ofSetColor(214, 218, 216); fUI.drawString(vs, s.track.x + s.track.width - fUI.stringWidth(vs), s.track.y - 8 * S);
        ofSetColor(48, 50, 52); ofDrawRectangle(s.track);
        if (s.lo < 0) {                                      // grey fill of the base value (bipolar from centre)
            float zero = ofMap(0, s.lo, s.hi, 0, s.track.width, true), fr = ofMap(base, s.lo, s.hi, 0, s.track.width, true);
            ofSetColor(150, 156, 154, 235); ofDrawRectangle(s.track.x + std::min(zero, fr), s.track.y, fabsf(fr - zero), s.track.height);
        } else {
            float fr = ofMap(base, s.lo, s.hi, 0, s.track.width, true);
            ofSetColor(150, 156, 154, 235); ofDrawRectangle(s.track.x, s.track.y, fr, s.track.height);
        }
        if (modded) {                                        // bright white live marker that moves with the modulation
            float frLive = ofMap(*s.val, s.lo, s.hi, 0, s.track.width, true);
            ofSetColor(242, 246, 243); ofDrawRectangle(s.track.x + ofClamp(frLive - 1.5f * S, 0.f, s.track.width - 3 * S), s.track.y - 3 * S, 3 * S, s.track.height + 6 * S);
        }
        if (modPickKind >= 0 && s.tab == -1 && !isModulated(idx)) {       // pulsing highlight on bindable params while picking
            float pl = 0.5f + 0.5f * sinf(t * 4.0f);
            ofNoFill(); ofSetLineWidth(2.0f * S); ofSetColor(214, 220, 216, (int)(70 + 150 * pl));
            ofDrawRectangle(s.track.x - 5 * S, s.track.y - 22 * S, s.track.width + 10 * S, s.track.height + 30 * S); ofFill();
        }
    };

    // ===== LEFT column: the modulatable parameters (always shown) =====
    float minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9; bool lany = false;
    for (auto& s : sliders) if (s.tab == -1 && sliderVisible(s)) { lany = true; minx = std::min(minx, s.track.x); miny = std::min(miny, s.track.y); maxx = std::max(maxx, s.track.x + s.track.width); maxy = std::max(maxy, s.track.y + s.track.height); }
    if (lany) {
        float px = 16 * S, pyT = 60 * S, pyB = 14 * S;
        ofSetColor(8, 9, 11, 255); ofDrawRectangle(minx - px, miny - pyT, (maxx - minx) + px * 2, (maxy - miny) + pyT + pyB);
        ofSetColor(190, 196, 193); fUI.drawString("GRAPH CONFIGURATION", minx, miny - 40 * S);
        for (auto& s : sliders) if (s.tab == -1 && sliderVisible(s)) drawSlider(s);
    }

    // ===== RIGHT panel: layout pass (compute content bottom + MOD boxes), then bg, tab bar, content =====
    float tby = fm + 118 * S, tbh = 30 * S;
    for (int i = 0; i < 6; i++) { modDestBox[i] = modClearBox[i] = modAmtBox[i] = ofRectangle(-99999, -99999, 0, 0); }
    for (int i = 0; i < 3; i++) modBipBox[i] = ofRectangle(-99999, -99999, 0, 0);
    lfoRateBox = resetBox = ofRectangle(-99999, -99999, 0, 0);
    float contentBottom = tby + tbh;
    presetAddBox = presetNameBox = ofRectangle(-99999, -99999, 0, 0);
    graphSubBox[0] = graphSubBox[1] = ofRectangle(-99999, -99999, 0, 0);
    camPadBox = camZBox = imgPanelAddBox = imgPanelClearBox = ofRectangle(-99999, -99999, 0, 0);
    presetBox.clear(); presetDelBox.clear();
    if (rightTab == 0) {                                     // GRAPH: GLOBAL / PRESETS sub-tabs
        float subG = 8 * S, subW = (rw - subG) * 0.5f, subY = fm + 162 * S;   // sub-tab sits just under the GRAPH/AUDIO/MOD tab bar
        graphSubBox[0] = ofRectangle(rx, subY, subW, 30 * S);
        graphSubBox[1] = ofRectangle(rx + subW + subG, subY, subW, 30 * S);
        if (graphSub == 0) {                                 // GLOBAL: choices + camera-angle pad + RESET
            for (auto& s : sliders) if (s.tab == 0 && sliderVisible(s)) contentBottom = std::max(contentBottom, s.track.y + s.track.height);
            if (cfgLayout >= 1.5f) {                          // IMAGE type: ADD + CLEAR buttons under the Type selector (camera pad is irrelevant here)
                float bg = 8 * S, bw = (rw - bg) * 0.5f, byy = contentBottom + 34 * S;
                imgPanelAddBox   = ofRectangle(rx, byy, bw, 44 * S);
                imgPanelClearBox = ofRectangle(rx + bw + bg, byy, bw, 44 * S);
                contentBottom = imgPanelAddBox.getMaxY();
            } else {
                float padSz = 116 * S, padY = contentBottom + 36 * S;
                camPadBox = ofRectangle(rx, padY, padSz, padSz);                                              // XY pad: camera X (vert) / Y (horiz) angle
                camZBox   = ofRectangle(rx + padSz + 20 * S, padY + padSz - 12 * S, rw - padSz - 20 * S, 13 * S);   // Z roll slider beside the pad
                contentBottom = padY + padSz;
                resetBox = ofRectangle(rx, contentBottom + 28 * S, rw, 42 * S); contentBottom = resetBox.getMaxY();
            }
        } else {                                             // PRESETS: list (load/delete) + "save current as preset" (with name entry)
            float y = subY + 30 * S + 48 * S, rh = 32 * S, g = 8 * S;   // leave room below the "PRESETS — click to load" hint
            presetBox.assign(presetList.size(), ofRectangle());
            presetDelBox.assign(presetList.size(), ofRectangle());
            if (presetNaming) {
                presetNameBox = ofRectangle(rx, y, rw, 38 * S);
                presetAddBox  = ofRectangle(rx, y + 38 * S + 12 * S, rw, 42 * S);
            } else {
                float bw = 30 * S;
                for (size_t i = 0; i < presetList.size(); i++) {
                    presetBox[i]    = ofRectangle(rx, y, rw - bw - g, rh);
                    presetDelBox[i] = ofRectangle(rx + rw - bw, y, bw, rh);
                    y += rh + g;
                }
                presetAddBox = ofRectangle(rx, y + 8 * S, rw, 42 * S);
            }
            contentBottom = presetAddBox.getMaxY();
        }
    } else if (rightTab == 1) {                              // AUDIO
        for (auto& s : sliders) if (s.tab == 1 && sliderVisible(s)) contentBottom = std::max(contentBottom, s.track.y + s.track.height);
    } else if (rightTab == 2) {                              // MODULATION: Source sub-tab, then the active source's 3 slots
        float subG = 8 * S, subW = (rw - subG) * 0.5f, subY = tby + tbh + 54 * S;
        sourceBox[0] = ofRectangle(rx, subY, subW, 30 * S);
        sourceBox[1] = ofRectangle(rx + subW + subG, subY, subW, 30 * S);
        float y = subY + 30 * S + 28 * S;
        int sec = modSource;
        if (sec == 1) {                                          // LFO rate slider + octave stepper row
            lfoRateBox = ofRectangle(rx, y + 18 * S, rw, 13 * S);
            float obY = lfoRateBox.getMaxY() + 12 * S, obW = 42 * S, obH = 24 * S;
            lfoOctPrevBox = ofRectangle(rx, obY, obW, obH);
            lfoOctNextBox = ofRectangle(rx + rw - obW, obY, obW, obH);
            y = obY + obH + 30 * S;
        }
        for (int k = 0; k < 3; k++) {
            int gi = sec * 3 + k; ModSlot& m = (sec == 0) ? audioMod[k] : lfoMod[k];
            bool lfo = (sec == 1), bound = (m.dest >= 0);
            float bw = 30 * S, g = 8 * S;
            if (bound) {
                modClearBox[gi] = ofRectangle(rx + rw - bw, y, bw, 30 * S);
                if (lfo) { modBipBox[k] = ofRectangle(rx + rw - 2 * bw - g, y, bw, 30 * S); modDestBox[gi] = ofRectangle(rx, y, rw - 2 * bw - 2 * g, 30 * S); }
                else       modDestBox[gi] = ofRectangle(rx, y, rw - bw - g, 30 * S);
            } else modDestBox[gi] = ofRectangle(rx, y, rw, 30 * S);
            if (bound) { y += 30 * S + 24 * S; modAmtBox[gi] = ofRectangle(rx, y, rw, 13 * S); y += 13 * S + 26 * S; }
            else y += 30 * S + 26 * S;
        }
        contentBottom = y;
    }

    ofSetColor(8, 9, 11, 255);                               // background, sized to content but clamped inside the frame
    float bgBottom = std::min(contentBottom + 26 * S, RH - fm - 6 * S);
    ofDrawRectangle(rx - 16 * S, tby - 22 * S, rw + 32 * S, bgBottom - (tby - 22 * S));

    const char* tnames[3] = { "GRAPH", "AUDIO", "MOD" };    // tab bar
    float tbg = 6 * S, tbw = (rw - 2 * tbg) / 3.0f;
    for (int i = 0; i < 3; i++) { tabBox[i] = ofRectangle(rx + i * (tbw + tbg), tby, tbw, tbh); chip(tabBox[i], tnames[i], rightTab == i); }

    if (rightTab == 0) {                                     // GRAPH: GLOBAL / PRESETS sub-tabs
        chip(graphSubBox[0], "GLOBAL", graphSub == 0); chip(graphSubBox[1], "PRESETS", graphSub == 1);
        if (graphSub == 0) {
            for (auto& s : sliders) if (s.tab == 0 && sliderVisible(s)) drawSlider(s);
            if (cfgLayout >= 1.5f) {                          // IMAGE type: ADD IMAGES button (folder picker) under the Type selector
                ofSetColor(150, 156, 154);
                fUI.drawString(imgList.empty() ? "no images yet — add a folder" : ofToString(imgList.size()) + " image" + (imgList.size() == 1 ? "" : "s") + " loaded",
                               imgPanelAddBox.x, imgPanelAddBox.y - 10 * S);
                chip(imgPanelAddBox, "+  ADD IMAGES", false);
                chip(imgPanelClearBox, "CLEAR", false);
            } else {
            // ---- camera angle: XY pad (X = vertical, Y = horizontal) + Z roll slider ----
            ofSetColor(158, 164, 162); fUI.drawString("Camera Angle  (drag)", camPadBox.x, camPadBox.y - 8 * S);
            ofSetColor(22, 25, 28); ofDrawRectangle(camPadBox);
            ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(70, 76, 74); ofDrawRectangle(camPadBox);
            ofSetColor(46, 50, 54);
            ofDrawLine(camPadBox.getCenter().x, camPadBox.y, camPadBox.getCenter().x, camPadBox.getMaxY());
            ofDrawLine(camPadBox.x, camPadBox.getCenter().y, camPadBox.getMaxX(), camPadBox.getCenter().y);
            ofFill();
            float hx = ofMap(camManY, -180, 180, camPadBox.x, camPadBox.getMaxX(), true);
            float hy = ofMap(camManX, -180, 180, camPadBox.y, camPadBox.getMaxY(), true);
            ofSetColor(232, 237, 234); ofDrawCircle(hx, hy, 5 * S);
            ofSetColor(150, 156, 154); fUI.drawString("Z  roll", camZBox.x, camZBox.y - 8 * S);
            ofSetColor(34, 38, 42); ofDrawRectangle(camZBox);
            float zf = ofMap(camManZ, -180, 180, 0, camZBox.width, true);
            ofSetColor(232, 237, 234); ofDrawRectangle(camZBox.x + ofClamp(zf - 1.5f * S, 0.f, camZBox.width - 3 * S), camZBox.y - 3 * S, 3 * S, camZBox.height + 6 * S);
            chip(resetBox, "RESET", false);
            }
        }
        else {                                               // PRESETS list + save-as (with name entry)
            if (presetNaming) {
                ofSetColor(150, 156, 154); fUI.drawString("NAME THIS PRESET", presetNameBox.x, presetNameBox.y - 10 * S);
                ofSetColor(26, 30, 32); ofDrawRectangle(presetNameBox);
                ofSetColor(220, 226, 222); fValue.drawString(presetNameBuf, presetNameBox.x + 12 * S, presetNameBox.y + 26 * S);
                if (fmodf(t, 1.0f) < 0.55f) { float cx = presetNameBox.x + 14 * S + fValue.stringWidth(presetNameBuf); ofSetColor(cNeon); ofDrawRectangle(cx, presetNameBox.y + 7 * S, 2 * S, 24 * S); }
                chip(presetAddBox, "SAVE   ·   [enter]   ·   [esc] cancel", true);
            } else {
                ofSetColor(150, 156, 154); fUI.drawString(presetList.empty() ? "no presets yet — save one below" : "PRESETS — click to load", graphSubBox[0].x, graphSubBox[0].getMaxY() + 22 * S);
                for (size_t i = 0; i < presetList.size() && i < presetBox.size(); i++) {
                    ofRectangle& b = presetBox[i];
                    ofSetColor(40, 44, 48); ofDrawRectangle(b);
                    ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(92, 100, 96); ofDrawRectangle(b); ofFill();
                    ofSetColor(228, 233, 230); fUI.drawString(presetList[i], b.x + 12 * S, b.y + b.height * 0.5f + 4.5f * S);
                    chip(presetDelBox[i], "x", false);
                }
                chip(presetAddBox, "+  SAVE CURRENT AS PRESET", false);
            }
        }
    }
    if (rightTab == 1) { for (auto& s : sliders) if (s.tab == 1 && sliderVisible(s)) drawSlider(s); }
    if (rightTab == 2) {
        ofSetColor(132, 150, 140); fUI.drawString("SOURCE", rx, sourceBox[0].y - 14 * S);   // Source sub-tab
        chip(sourceBox[0], "AUDIO", modSource == 0); chip(sourceBox[1], "LFO", modSource == 1);
        int sec = modSource;
        if (sec == 1) {   // LFO rate — Schumann (7.83 Hz) down 10 octaves to 7.83/1024 Hz; log slider + < OCT n > stepper
            const float SCHU = 7.83f, FMIN = SCHU / 1024.0f;
            int oct = (int)ofClamp(roundf(log2f(SCHU / std::max(FMIN, cfgLfoRate))), 0.f, 10.f);
            bool onOct = fabsf(cfgLfoRate - SCHU / powf(2.0f, oct)) / (SCHU / powf(2.0f, oct)) < 0.02f;
            ofSetColor(150, 156, 154); fUI.drawString("Rate", lfoRateBox.x, lfoRateBox.y - 6 * S);
            int rp = cfgLfoRate >= 1.0f ? 2 : (cfgLfoRate >= 0.1f ? 3 : (cfgLfoRate >= 0.02f ? 4 : 6));   // more decimals at the low octaves (lowest = 0.007646 Hz)
            std::string rs = ofToString(cfgLfoRate, rp) + " Hz";
            if (onOct) rs += (oct == 0) ? "  ·  SCHUMANN" : "  ·  SCHUMANN OCT";
            float pad = onOct ? 18 * S : 0;                                  // room for the earth icon
            ofSetColor(214, 218, 216); fUI.drawString(rs, lfoRateBox.x + lfoRateBox.width - pad - fUI.stringWidth(rs), lfoRateBox.y - 6 * S);
            if (onOct) {                                                     // little earth icon (⊕) after the label
                float er = 5 * S, ex = lfoRateBox.x + lfoRateBox.width - er, ey = lfoRateBox.y - 6 * S - fUI.getAscenderHeight() * 0.32f;
                ofNoFill(); ofSetLineWidth(1.2f * S); ofSetColor(214, 218, 216);
                ofDrawCircle(ex, ey, er); ofDrawLine(ex - er, ey, ex + er, ey); ofDrawLine(ex, ey - er, ex, ey + er); ofFill();
            }
            ofSetColor(48, 50, 52); ofDrawRectangle(lfoRateBox);
            float frac = ofClamp(log2f(cfgLfoRate / FMIN) / 10.0f, 0.f, 1.f);  // log scale => octaves evenly spaced
            ofSetColor(150, 156, 154, 235); ofDrawRectangle(lfoRateBox.x, lfoRateBox.y, frac * lfoRateBox.width, lfoRateBox.height);
            chip(lfoOctPrevBox, "<", false); chip(lfoOctNextBox, ">", false);
            std::string octs = "OCT " + ofToString(oct);
            ofSetColor(190, 196, 193); ofRectangle ob = fUI.getStringBoundingBox(octs, 0, 0);
            fUI.drawString(octs, floorf((lfoOctPrevBox.getMaxX() + lfoOctNextBox.x) * 0.5f - ob.width * 0.5f - ob.x), floorf(lfoOctPrevBox.y + (lfoOctPrevBox.height - ob.height) * 0.5f - ob.y));
        }
        for (int k = 0; k < 3; k++) {
            int gi = sec * 3 + k; ModSlot& m = (sec == 0) ? audioMod[k] : lfoMod[k];
            bool picking = (modPickKind == sec && modPickSlot == k);
            chip(modDestBox[gi], picking ? "PICK…" : (m.dest >= 0 ? ofToUpper(sliders[m.dest].name) : "— bind —"), picking);
            if (m.dest >= 0) {
                chip(modClearBox[gi], "x", false);
                if (sec == 1) chip(modBipBox[k], "\xC2\xB1", m.bipolar);         // ± bipolar toggle (LFO only)
                Slider& d = sliders[m.dest]; miniSlider(modAmtBox[gi], "Target Value", m.amt, d.lo, d.hi, d.prec, "");
            }
        }
    }

    ofSetColor(120, 126, 124);   // help hint — bottom-right, OUTSIDE the frame, aligned to the right border
    { std::string h = "press  H  for shortcuts"; fUI.drawString(h, RW - fm - 18 * S - fUI.stringWidth(h), RH - fm + 32 * S); }   // below the frame, under the bottom-right wordmark

    if (modPickKind >= 0) {                                  // picking a destination — explain modulation in the helper card
        bool audio = (modPickKind == 0);
        std::string body = audio
            ? "Audio modulation links the live signal's loudness to a parameter. The value rides from its current setting (the base) toward the Target Value you choose: louder audio pushes it closer to the target, quiet lets it fall back. The result is a parameter that breathes with the music, hands-free.\n\nPick a destination by clicking any pulsing control in the left column, then set how far it travels with Target Value."
            : "LFO modulation sweeps a parameter with a slow sine wave, gliding it back and forth between the base and the Target Value on its own. Set the speed with LFO Rate (locked to Schumann octaves), and use the +/- bipolar toggle to swing either side of the base instead of only one direction.\n\nPick a destination by clicking any pulsing control in the left column, then set the travel with Target Value.";
        drawHelpCard("MODULATION", audio ? "AUDIO MOD" : "LFO MOD",
                     "click a highlighted control on the left to bind it",
                     body, "one parameter per slot   ·   ESC to cancel");
    }
    if (t - errFlash < 2.5f) {                               // error toast — bottom, outside the frame, fades out
        float a = ofClamp((2.5f - (t - errFlash)) / 0.5f, 0.f, 1.f);
        ofSetColor(236, 238, 237, (int)(235 * a));
        fUI.drawString(errMsg, RW * 0.5f - fUI.stringWidth(errMsg) * 0.5f, RH - fm + 36 * S);
    }
}

//--------------------------------------------------------------
//  In-app SETTINGS editor — edit session content live (no JSON editing)
//--------------------------------------------------------------
std::string ofApp::fieldText(const Field& f) {
    if (f.header) return "";                                   // section divider — no value to show
    if (f.folder && f.sp) {                                    // show the resolved target folder (no disk side-effects)
        if (f.sp->empty()) return "(default)  " + gsVideosDir() + "HelioRecordings";
        std::string p = *f.sp; if (!p.empty() && p.back() == '/') p.pop_back();
        return p + "/HelioRecordings";
    }
    if (!f.choices.empty() && f.ip) {
        int idx = ofClamp(*f.ip, 0, (int)f.choices.size() - 1);
        if (f.op && idx == (int)f.choices.size() - 1) return f.op->empty() ? f.choices[idx] : *f.op;   // OTHER -> the custom text
        return f.choices[idx];
    }
    if (f.sp) return *f.sp;
    if (f.ip) return ofToString(*f.ip);
    return ofToString(*f.fp, 1);
}
void ofApp::commitField(Field& f) {
    if (f.header) return;                                      // section divider — nothing to commit
    if (f.op && !f.choices.empty() && f.ip && *f.ip == (int)f.choices.size() - 1) *f.op = f.buf;   // editing the OTHER free-text
    else if (f.sp) *f.sp = f.buf;
    else if (f.ip) *f.ip = ofToInt(f.buf);
    else if (f.fp) *f.fp = ofToFloat(f.buf);
}
// Capture the current field values as the baseline (dialog open + after SAVE). ROUTING device/channel are
// a LIVE audition, not saved config — they apply immediately and are excluded from the snapshot.
void ofApp::snapshotFields() {
    snapS.clear(); snapI.clear(); snapF.clear();
    for (auto& f : fields) {
        if (f.sp) snapS[f.sp] = *f.sp;
        else if (f.ip) { if (f.ip != &sInputDeviceIdx && f.ip != &sInputChannelPair) snapI[f.ip] = *f.ip; }
        else if (f.fp) snapF[f.fp] = *f.fp;
    }
}
// Restore the baseline — ESC discards unsaved edits (settings never autosave).
void ofApp::revertFields() {
    for (auto& kv : snapS) *kv.first = kv.second;
    for (auto& kv : snapI) *kv.first = kv.second;
    for (auto& kv : snapF) *kv.first = kv.second;
}
void ofApp::buildFields() {
    fields.clear();
    // Each add* helper tags the field with curTab, so drawSettings()/mousePressed() can show only
    // the active tab's fields — this is what keeps the dialog's height bounded regardless of how
    // many total settings exist across SESSION/ROUTING/BROADCAST (see drawSettings()).
    int curTab = 0;   // 0 SESSION · 1 ROUTING · 2 BROADCAST — bumped below as each section starts
    auto addS = [&](std::string l, std::string* p){ Field f; f.label = l; f.sp = p; f.tab = curTab; fields.push_back(f); };
    auto addI = [&](std::string l, int* p){ Field f; f.label = l; f.ip = p; f.tab = curTab; fields.push_back(f); };
    auto addC = [&](std::string l, int* p, std::vector<std::string> c, std::string* op){ Field f; f.label = l; f.ip = p; f.choices = c; f.op = op; f.tab = curTab; fields.push_back(f); };
    auto addF = [&](std::string l, std::string* p){ Field f; f.label = l; f.sp = p; f.folder = true; f.tab = curTab; fields.push_back(f); };
    auto addSecret = [&](std::string l, std::string* p){ Field f; f.label = l; f.sp = p; f.secret = true; f.tab = curTab; fields.push_back(f); };
    // SESSION = branding for THIS broadcast/show — the parts that change episode to episode.
    // (Channel-wide branding — channel name, artist, font, colours — lives on the CHANNEL tab.)
    addI("Episode",          &sTransmission);  // the number after the channel
    addS("Artifact Title",   &sArtifact);      // filename brand (e.g. HelioGraph)
    addS("Series Shorthand", &sShorthand);     // filename code (e.g. TxN)
    addS("Title",            &sTitle);         // shows bottom-left while recording
    addS("Note",             &sNote);          // shows bottom-left while recording
    addF("Recordings", &sRecDir);    // folder where recordings are saved — click opens a folder picker (local only)
    // Waypoint / Heading / Distance are not editable — they're auto-generated telemetry.

    // ---- ROUTING: pick the audio input device + which channel pair to listen to ----
    // Rebuilt every time this dialog opens (see the 'E' key handler) so a freshly plugged-in
    // interface (e.g. a Zoom L-8) shows up without restarting the app.
    curTab = 1;
    audioDeviceChoices = { "Auto" };
    auto devices = stream.getDeviceList();
    for (auto& d : devices) if (d.inputChannels > 0) audioDeviceChoices.push_back(d.name);
    sInputDeviceIdx = 0;
    for (size_t i = 1; i < audioDeviceChoices.size(); i++)
        if (audioDeviceChoices[i] == sInputDevice) { sInputDeviceIdx = (int)i; break; }
    addC("Input Device", &sInputDeviceIdx, audioDeviceChoices, nullptr);

    // Channel-pair choices reflect whichever device is currently selected (or, for "Auto",
    // whatever's actively streaming right now) — not a device the artist has merely highlighted
    // but not saved yet.
    int chForPairs = captureChannels > 0 ? captureChannels : 2;
    if (sInputDeviceIdx > 0)
        for (auto& d : devices) if (d.name == audioDeviceChoices[sInputDeviceIdx]) { chForPairs = std::max(1, (int)d.inputChannels); break; }
    std::vector<std::string> chChoices;
    for (int p = 0; p * 2 < chForPairs; p++) chChoices.push_back(ofToString(p * 2 + 1) + "-" + ofToString(std::min(p * 2 + 2, chForPairs)));
    if (chChoices.empty()) chChoices.push_back("1-2");
    sInputChannelPair = ofClamp(sInputChannelPair, 0, (int)chChoices.size() - 1);
    addC("Channels", &sInputChannelPair, chChoices, nullptr);
    sInputChannelPairAtOpen = sInputChannelPair;   // snapshot — writeSession() diffs against this to know whether to hot-swap the stream

    // ---- REGISTER: get an artist account on the server with an invite code. The server hands back the
    // broadcast config (mount/password/snapshot URL) — the artist never types raw hosts. Click REGISTER
    // (drawn below the fields in drawSettings); then 'B' broadcasts to your own channel.
    curTab = 2;
    addS("Registration Server", &sRegServer);   // defaults to radio.stackmate.org; or your own heliod stack
    addS("Invite Code",         &sInviteCode);  // single-use code from the station admin (consumed on register)
    addS("Artist Name",         &sArtist);      // your channel's display name (same field as CHANNEL → Artist)

    // ---- CHANNEL: channel-wide branding (the basics) — channel name + artist + the client's UI font +
    // accent colour. These ride in the shared snapshot metadata, so SAVE re-themes your live channel on
    // the listener client within one snapshot (~15s while broadcasting). Font preview drawn below.
    curTab = 3;
    addS("Channel Name", &sChannel);   // the big title word on the client (e.g. TRANSMISSION)
    addS("Artist",       &sArtist);    // your name on the client (same field as REGISTER → Artist Name)
    { std::vector<std::string> fl(kFontLabels, kFontLabels + kNumFonts);
      addC("Font", &sFontIdx, fl, nullptr); }   // client UI font — click to cycle; preview below
    addS("Accent", &sAccent);          // client accent colour, hex (e.g. #ff3b30) — swatch shown beside it
    // Donations — a bc1 Bitcoin and/or lq1 Liquid address. Only broadcast once the wallet is confirmed
    // backed up (the client shows them in a room behind a "verify with the artist" gate).
    { Field h; h.header = true; h.label = "Donations"; h.tab = curTab; fields.push_back(h); }
    addC("Wallet Backed Up", &sWalletBackedUp, {"NO", "YES"}, nullptr);   // MUST be YES before the address fields unlock
    addS("Lightning Address", &sLnAddress);   // user@domain (highest preference)
    addS("Bitcoin Address",   &sBtcAddress);  // bc1…
    addS("Liquid Address",    &sLqAddress);    // lq1…
    // Protection: the address fields are greyed out + read-only until the artist confirms the wallet is
    // backed up — you can't add a donation address for a wallet you might not control/recover.
    for (auto& f : fields) if (f.sp == &sLnAddress || f.sp == &sBtcAddress || f.sp == &sLqAddress) f.gateBackup = true;

    // ---- PUBLISH: publish the local recordings as a named collection on the registered server (parity with
    // the `helio` CLI). The artist types a collection name; PUBLISH (drawn below the field) POSTs /collections
    // then uploads each recording's extracted audio. Needs an account (see REGISTER).
    curTab = 4;
    addS("Collection Name", &sCollectionName);   // the collection your recordings are published under
    { std::vector<std::string> al(kArtStyles, kArtStyles + kNumArtStyles);
      addC("Track Art", &sCollectionArtIdx, al, nullptr); }   // generative art style for this collection — click to cycle

    // Once registered, the artist name IS the channel identity (it hashes to the channel id) — lock every
    // artist-name field so it can't be changed. Pure local-recording users (not registered) stay editable.
    for (auto& f : fields) if (f.sp == &sArtist) f.locked = sRegistered;
}
void ofApp::writeSession() {
    // ROUTING: translate the UI index back to a persisted device name ("" = Auto), then hot-swap
    // the live audio stream if the artist actually changed device/channel selection this session —
    // avoids restarting audio on every save when only, say, the Artist field changed.
    std::string newDevice = (sInputDeviceIdx <= 0 || sInputDeviceIdx >= (int)audioDeviceChoices.size())
                             ? "" : audioDeviceChoices[sInputDeviceIdx];
    bool routingChanged = (newDevice != sInputDevice) || (sInputChannelPair != sInputChannelPairAtOpen);
    sInputDevice = newDevice;
    if (routingChanged) {
        stream.close();
        setupAudio();
        sInputChannelPairAtOpen = sInputChannelPair;
    }

    ofJson j;
    { std::ifstream in(gsSessionPath());   // keep any _help block
      if (in) { try { in >> j; } catch (...) {} } }
    j["transmission"] = sTransmission;
    j["title"]   = sTitle;
    j["date"]    = sDate;
    j["artist"]  = sArtist;
    j["channel"] = sChannel;
    j["note"]    = sNote;
    j["font"]    = kFontKeys[(int)ofClamp(sFontIdx, 0, kNumFonts - 1)];   // CHANNEL branding
    j["accent"]  = sAccent;
    j["donations"]["ln"]  = sLnAddress;  j["donations"]["btc"] = sBtcAddress;
    j["donations"]["lq"]  = sLqAddress;  j["donations"]["backedUp"] = sWalletBackedUp;
    j["artifact"]  = sArtifact;
    j["shorthand"] = sShorthand;
    j["recDir"]    = sRecDir;
    j["imgDir"]    = sImgDir;
    j["inputDevice"]      = sInputDevice;
    j["inputChannelPair"] = sInputChannelPair;
    j["broadcast"]["iceHost"]       = sIceHost;
    j["broadcast"]["icePort"]       = sIcePort;
    j["broadcast"]["iceMount"]      = sIceMount;
    j["broadcast"]["icePassword"]   = sIcePassword;
    j["broadcast"]["snapshotUrl"]   = sSnapshotUrl;
    j["broadcast"]["snapshotToken"] = sSnapshotToken;
    j["registration"]["server"]     = sRegServer;
    j["registration"]["channelId"]  = sChannelId;
    j["registration"]["registered"] = sRegistered;
    j["collection"] = sCollectionName;   // PUBLISH tab: persisted collection name
    j["collectionArt"] = kArtStyles[(int)ofClamp(sCollectionArtIdx, 0, kNumArtStyles - 1)];   // PUBLISH tab: track-art style
    j["coordinates"]["waypoint"] = sWaypoint;
    j["coordinates"]["heading"]  = sHeading;
    j["coordinates"]["distance"] = sDist;
    std::ofstream o(gsSessionPath());
    if (o) { o << j.dump(2); o.close(); saveFlash = t; snapshotFields(); ofLogNotice() << "session.json saved"; }   // SAVE is the new baseline (so a later ESC won't revert saved values)
    if (broadcasting) lastSnapshotT = -100;   // SAVE → push a fresh snapshot NOW so config changes (incl. cleared donation addresses when Wallet Backed Up flips to NO) reach the client immediately
    else   { ofLogError() << "session.json: could not open for writing"; }
}

//--------------------------------------------------------------
//  PRESETS — each is a named .json file in presets/. A preset is the full VISUAL config (every slider incl. layout
//  type, the LFO rate, and modulation). It does NOT include session settings (artist/title/etc — those live in session.json).
void ofApp::scanPresets() {
    presetList.clear();
    ofDirectory dir(gsPresetsDir());
    dir.allowExt("json");
    dir.listDir();
    for (auto& f : dir.getFiles()) presetList.push_back(f.getBaseName());   // name without ".json"
    std::sort(presetList.begin(), presetList.end());
}
void ofApp::savePresetNamed(const std::string& name) {
    std::string safe; for (char c : name) safe += (isalnum((unsigned char)c) || c == '_' || c == '-' || c == ' ') ? c : '_';
    while (!safe.empty() && safe.front() == ' ') safe.erase(safe.begin());
    while (!safe.empty() && safe.back() == ' ') safe.pop_back();
    if (safe.empty()) safe = "preset";
    auto modJson = [&](const ModSlot& m) {
        ofJson j; j["dest"] = (m.dest >= 0 && m.dest < (int)sliders.size()) ? sliders[m.dest].name : "";   // dest by NAME (survives reordering)
        j["base"] = m.base; j["amt"] = m.amt; j["bip"] = m.bipolar; return j;
    };
    ofJson p;
    for (auto& s : sliders) p["s"][s.name] = *s.val;          // every slider (Type=layout, Mode, colours, params, audio…) — NO session fields
    p["lfoRate"] = cfgLfoRate;
    p["camMan"] = { camManX, camManY, camManZ };              // manual camera angle (not a slider)
    for (int k = 0; k < 3; k++) { p["audio"].push_back(modJson(audioMod[k])); p["lfo"].push_back(modJson(lfoMod[k])); }
    std::ofstream o(gsPresetsDir() + safe + ".json");
    if (o) { o << p.dump(2); o.close(); ofLogNotice() << "preset saved: " << safe; }
    scanPresets();
    saveFlash = t;
}
void ofApp::loadPresetFile(const std::string& name) {
    std::ifstream in(gsPresetsDir() + name + ".json");
    if (!in) return;
    ofJson p; try { in >> p; } catch (...) { return; }
    for (auto& s : sliders) if (p.contains("s") && p["s"].contains(s.name)) *s.val = p["s"][s.name].get<float>();
    cfgLfoRate = p.value("lfoRate", cfgLfoRate);
    if (p.contains("camMan") && p["camMan"].size() == 3) { camManX = p["camMan"][0].get<float>(); camManY = p["camMan"][1].get<float>(); camManZ = p["camMan"][2].get<float>(); }
    auto idxByName = [&](const std::string& nm) -> int {
        if (nm.empty()) return -1;
        for (size_t i = 0; i < sliders.size(); i++) if (sliders[i].name == nm) return (int)i;
        return -1;
    };
    auto readMod = [&](ModSlot& m, const ofJson& j) {
        m = ModSlot(); m.dest = idxByName(j.value("dest", std::string("")));
        m.base = j.value("base", 0.0f); m.amt = j.value("amt", 0.0f); m.bipolar = j.value("bip", false);
    };
    for (int k = 0; k < 3; k++) {
        if (p.contains("audio") && k < (int)p["audio"].size()) readMod(audioMod[k], p["audio"][k]); else audioMod[k] = ModSlot();
        if (p.contains("lfo")   && k < (int)p["lfo"].size())   readMod(lfoMod[k],   p["lfo"][k]);   else lfoMod[k]   = ModSlot();
    }
    modPickKind = -1; activeModAmt = -1;
    if (cfgLayout < 1.5f) {                  // presets store the RADIAL/GRID visualizer state (IMAGE has none)
        lastLayout = (int)cfgLayout;         // lock the loaded layout so the swap doesn't clobber it
        saveLayoutState((int)cfgLayout);     // store the preset into the active layout's slot
    }
    relayout();
    saveFlash = t;
}
void ofApp::deletePresetFile(const std::string& name) {
    ofFile::removeFile(gsPresetsDir() + name + ".json");
    scanPresets();
}
void ofApp::drawSettings() {
    ofSetColor(0, 0, 0, 185); ofDrawRectangle(0, 0, RW, RH);          // dim everything
    // Dialog height is bounded by the ACTIVE tab's field count only, not the total across all three —
    // otherwise SESSION+ROUTING+BROADCAST combined would be taller than the frame itself (RH).
    // Row height is computed by ONE function, used both to size the dialog (here, before py is even
    // known) and to lay out fields while drawing (below) — so they can never drift out of sync again.
    // This dialog has already overlapped its own footer twice from a hand-tuned constant that wasn't
    // updated when a new row's height changed (once for the tab bar, once for the Channels signal
    // meter) — a shared source of truth removes that whole bug class instead of re-guessing a number.
    auto rowHeight = [](const Field& f) -> float {
        if (f.header) return 14 + 44;
        float extra = 0;
        if (f.label == "Note") extra = 30;           // Title/Note info line
        if (f.label == "Channels") extra = 34;       // live signal meter
        return 64 + extra;
    };
    float contentH = 0;
    for (auto& f : fields) if (f.tab == settingsTab) contentH += rowHeight(f);
    if (settingsTab == 1) contentH += 64;    // ROUTING draws a REFRESH DEVICE LIST button below the fields (must be counted here or it collides with the footer)
    if (settingsTab == 2) contentH += 104;   // REGISTER draws a button + status line + subline below the fields (same footer-overlap trap)
    if (settingsTab == 3) contentH += 148;   // CHANNEL draws a font PREVIEW box + hint + donations note below the fields (footer-overlap trap)
    if (settingsTab == 4) contentH += 104;   // PUBLISH draws a button + status line + subline below the field (same footer-overlap trap)
    float FY_START = 158, FOOTER_RESERVE = 126;   // FY_START must match tby+tbh+46 below; footer = gap + RECORDING/SAVE/hint block
    float pw = 1200 * S, ph = (FY_START + contentH + FOOTER_RESERVE) * S;
    float px = (RW - pw) * 0.5f, py = (RH - ph) * 0.5f;
    ofSetColor(12, 14, 16, 248); ofDrawRectangle(px, py, pw, ph);
    ofSetColor(210, 216, 212); fKick.drawString("SETTINGS", px + 40 * S, py + 56 * S);

    // tab bar — SESSION / ROUTING / REGISTER / CHANNEL / PUBLISH (same chip styling as the right panel's GRAPH/AUDIO/MOD)
    const char* tabNames[5] = { "SESSION", "ROUTING", "REGISTER", "CHANNEL", "PUBLISH" };
    float tby = py + 78 * S, tbh = 34 * S, tbg = 8 * S, tbw = (pw - 80 * S - 4 * tbg) / 5.0f;
    for (int i = 0; i < 5; i++) {
        settingsTabBox[i] = ofRectangle(px + 40 * S + i * (tbw + tbg), tby, tbw, tbh);
        bool hot = (settingsTab == i);
        ofSetColor(hot ? ofColor(58, 70, 64) : ofColor(34, 38, 42)); ofDrawRectangle(settingsTabBox[i]);
        ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(hot ? ofColor(206, 212, 208) : ofColor(92, 100, 96)); ofDrawRectangle(settingsTabBox[i]); ofFill();
        ofSetColor(hot ? ofColor(236, 240, 237) : ofColor(186, 194, 190));
        ofRectangle bb = fUI.getStringBoundingBox(tabNames[i], 0, 0);
        fUI.drawString(tabNames[i], floorf(settingsTabBox[i].x + (settingsTabBox[i].width - bb.width) * 0.5f - bb.x), floorf(settingsTabBox[i].y + (settingsTabBox[i].height - bb.height) * 0.5f - bb.y));
    }

    float fy = tby + tbh + 46 * S;
    for (size_t i = 0; i < fields.size(); i++) {
        Field& f = fields[i];
        if (f.tab != settingsTab) continue;                           // only the active tab's fields occupy layout space
        if (f.header) {                                               // section divider — no control, just a label
            fy += 14 * S;
            ofSetColor(64, 70, 68); ofDrawLine(px + 40 * S, fy - 20 * S, px + pw - 40 * S, fy - 20 * S);
            ofSetColor(190, 196, 192); fLabel.drawString(ofToUpper(f.label), px + 40 * S, fy);
            fy += 44 * S;
            continue;
        }
        bool gateOff = f.gateBackup && !sWalletBackedUp;                 // donation field, wallet not yet confirmed backed up
        bool inert   = f.locked || gateOff;                              // read-only + greyed
        f.box = ofRectangle(px + 320 * S, fy - 30 * S, pw - 360 * S, 44 * S);
        ofSetColor(inert ? ofColor(80, 84, 88) : ofColor(150, 160, 156)); fLabel.drawString(ofToUpper(f.label), px + 40 * S, fy - 2 * S);
        bool foc = ((int)i == editingField);
        ofSetColor(inert ? ofColor(20, 22, 24) : (foc ? ofColor(44, 48, 52) : ofColor(26, 30, 32))); ofDrawRectangle(f.box);   // inert fields sit darker/greyed (grey focus highlight otherwise, no green)
        // secret fields (Icecast/snapshot credentials): mask at rest so a glance at the screen or a
        // screen-share doesn't leak them — but fieldText() itself stays unmasked (mousePressed uses it
        // to seed f.buf when you click in to edit; masking there would let "********" overwrite the real value).
        bool isBech = (f.label == "Bitcoin Address" || f.label == "Liquid Address");   // bc1/lq1 → shown 4-char grouped
        std::string txt = foc ? f.buf
                        : (f.secret ? (f.sp && !f.sp->empty() ? "********" : "(not set)")
                        : (isBech && f.sp && !f.sp->empty() ? gsGroup4(*f.sp) : fieldText(f)));
        // In-field PASTE / CLEAR buttons on every editable text field (not selectors/folder/locked/gated).
        f.pasteBox = ofRectangle(); f.clearBox = ofRectangle();          // reset each frame (hidden unless drawn)
        bool textEditable = f.sp && f.choices.empty() && !f.folder && !inert;
        float btnReserve = 0;
        if (textEditable) {
            float bh = 26 * S, cy = f.box.y + f.box.height * 0.5f, gap = 7 * S;
            float wc = fUI.stringWidth("CLEAR") + 18 * S, wp = fUI.stringWidth("PASTE") + 18 * S;
            f.clearBox = ofRectangle(f.box.getMaxX() - 10 * S - wc, cy - bh * 0.5f, wc, bh);
            f.pasteBox = ofRectangle(f.clearBox.x - gap - wp, cy - bh * 0.5f, wp, bh);
            btnReserve = f.box.getMaxX() - f.pasteBox.x + 12 * S;
        }
        // Keep long values (e.g. a Liquid address) INSIDE the box — no overflow past the panel. While
        // editing, scroll to show the TAIL (caret stays visible); at rest, truncate the end with an ellipsis.
        float maxTxtW = f.box.width - 30 * S - btnReserve;
        std::string vis = txt;
        if (foc) { while (vis.size() > 1 && fValue.stringWidth(vis) > maxTxtW) vis = vis.substr(1); }
        else if (fValue.stringWidth(vis) > maxTxtW) {
            std::string ell = "\xE2\x80\xA6";
            while (vis.size() > 1 && fValue.stringWidth(vis + ell) > maxTxtW) vis.pop_back();
            vis += ell;
        }
        ofSetColor(inert ? ofColor(90, 94, 96) : ofColor(220, 226, 222)); fValue.drawString(gateOff ? "" : vis, f.box.x + 14 * S, fy);
        if (f.locked)  { ofSetColor(120, 126, 124); std::string h = "[locked while registered]"; fUI.drawString(h, f.box.x + f.box.width - 16 * S - fUI.stringWidth(h), fy - 2 * S); }
        if (gateOff)   { ofSetColor(120, 126, 124); std::string h = "set Wallet Backed Up = YES to enter"; fUI.drawString(h, f.box.x + f.box.width - 16 * S - fUI.stringWidth(h), fy - 2 * S); }
        if (!f.choices.empty()) { ofSetColor(120, 126, 124); std::string h = "click to toggle"; fUI.drawString(h, f.box.x + f.box.width - 16 * S - fUI.stringWidth(h), fy - 2 * S); }
        // PASTE / CLEAR buttons (drawn inside the field, right-aligned).
        if (textEditable) {
            auto drawBtn = [&](const ofRectangle& b, const std::string& label){
                ofSetColor(40, 44, 48); ofDrawRectangle(b);
                ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(96, 104, 100); ofDrawRectangle(b); ofFill();
                ofRectangle bb = fUI.getStringBoundingBox(label, 0, 0);
                ofSetColor(186, 192, 188); fUI.drawString(label, floorf(b.x + (b.width - bb.width) * 0.5f - bb.x), floorf(b.y + (b.height - bb.height) * 0.5f - bb.y));
            };
            drawBtn(f.pasteBox, "PASTE"); drawBtn(f.clearBox, "CLEAR");
        }
        // Address validity — a small green/red dot (green = will be broadcast), left of the buttons.
        if (f.sp && !f.sp->empty() && (isBech || f.label == "Lightning Address")) {
            bool v = (f.label == "Lightning Address") ? gsValidLightning(*f.sp) : gsValidAddr(*f.sp);
            float dotX = (f.pasteBox.width > 0 ? f.pasteBox.x - 12 * S : f.box.getMaxX() - 12 * S);
            ofSetColor(v ? ofColor(120, 200, 150) : ofColor(212, 120, 110)); ofDrawCircle(dotX, fy - 8 * S, 4 * S);
        }
        if (f.folder)           { ofSetColor(120, 126, 124); std::string h = "click to choose folder"; fUI.drawString(h, f.box.x + f.box.width - 16 * S - fUI.stringWidth(h), fy - 2 * S); }
        if (foc && fmodf(t, 1.0f) < 0.55f) {                          // blinking cursor — tracks the visible (scrolled) tail
            float cx = f.box.x + 16 * S + fValue.stringWidth(vis);
            ofSetColor(cNeon); ofDrawRectangle(cx, fy - 20 * S, 2 * S, 26 * S);
        }
        if (f.label == "Note") { ofSetColor(112, 118, 116); std::string in = "Title & Note appear in the bottom-left of your live recording"; fUI.drawString(in, f.box.getMaxX() - fUI.stringWidth(in), fy + 32 * S); }   // info line (right-aligned to the field edge)
        if (f.label == "Channels") {   // live signal meter — reflects the input being auditioned RIGHT NOW
                                       // (device/channel clicks apply live, see mousePressed). Driven by
                                       // `level`, the same smoothed RMS the visualizer/HUD use, so it goes
                                       // quiet exactly when audioIn() isn't receiving signal on this pair —
                                       // cycle Channels and watch for the meter to jump to find your source.
            float mx = f.box.x, my = fy + 18 * S, mw = f.box.width, mh = 12 * S;
            ofSetColor(120, 126, 124); fUI.drawString("SIGNAL", px + 40 * S, my + 9 * S);
            ofSetColor(30, 34, 36); ofDrawRectangle(mx, my, mw, mh);   // meter track
            float lvl = ofClamp(level, 0.0f, 1.0f);
            ofSetColor(lvl > 0.02f ? cNeon : ofColor(70, 76, 74));     // bright once signal is present, dim while silent
            if (lvl > 0.001f) ofDrawRectangle(mx, my, mw * lvl, mh);
            ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(92, 100, 96); ofDrawRectangle(mx, my, mw, mh); ofFill();
            ofSetColor(112, 118, 116);   // tip: this meter can also live on the home screen while performing
            fUI.drawString("tip: press  V  to show this meter along the bottom of the screen (never recorded)", mx, my + mh + 20 * S);
        }
        fy += rowHeight(f) * S;
    }
    if (settingsTab == 1) {   // ROUTING — see refreshAudioDevices() for why this needs to close + reopen the stream
        refreshDevicesBox = ofRectangle(px + 40 * S, fy + 10 * S, 300 * S, 44 * S);
        ofSetColor(34, 38, 42); ofDrawRectangle(refreshDevicesBox);
        ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(92, 100, 96); ofDrawRectangle(refreshDevicesBox); ofFill();
        ofSetColor(206, 212, 208);
        std::string rlabel = "REFRESH DEVICE LIST";
        ofRectangle bb = fUI.getStringBoundingBox(rlabel, 0, 0);
        fUI.drawString(rlabel, floorf(refreshDevicesBox.x + (refreshDevicesBox.width - bb.width) * 0.5f - bb.x), floorf(refreshDevicesBox.y + (refreshDevicesBox.height - bb.height) * 0.5f - bb.y));
        ofSetColor(120, 126, 124);
        std::string hint = "plugged something in? click to rescan";
        fUI.drawString(hint, refreshDevicesBox.getMaxX() + 16 * S, refreshDevicesBox.y + 28 * S);
    }
    if (settingsTab == 2) {   // REGISTER — button that POSTs to the server + a status line
        registerBox = ofRectangle(px + 40 * S, fy + 10 * S, 200 * S, 44 * S);
        ofSetColor(sRegistered ? ofColor(40, 60, 46) : ofColor(34, 38, 42)); ofDrawRectangle(registerBox);
        ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(sRegistered ? ofColor(140, 200, 160) : ofColor(92, 100, 96)); ofDrawRectangle(registerBox); ofFill();
        ofSetColor(206, 212, 208);
        std::string rlabel = sRegistered ? "RE-REGISTER" : "REGISTER";
        ofRectangle bb = fUI.getStringBoundingBox(rlabel, 0, 0);
        fUI.drawString(rlabel, floorf(registerBox.x + (registerBox.width - bb.width) * 0.5f - bb.x), floorf(registerBox.y + (registerBox.height - bb.height) * 0.5f - bb.y));
        // status / result line (fades in on the last attempt, then stays)
        if (!regStatus.empty()) {
            bool fresh = (t - regFlash) < 4.0f;
            ofSetColor(sRegistered ? ofColor(150, 210, 170) : ofColor(210, 150, 120), fresh ? 255 : 170);
            fUI.drawString(regStatus, registerBox.getMaxX() + 18 * S, registerBox.y + 28 * S);
        }
        // current channel line
        ofSetColor(120, 126, 124);
        std::string sub = sRegistered ? ("broadcasting as " + sArtist + "  \xC2\xB7  " + sRegServer)
                                      : "enter an invite code + artist name, then REGISTER to broadcast";
        fUI.drawString(sub, px + 40 * S, registerBox.getMaxY() + 26 * S);
    }
    if (settingsTab == 3) {   // CHANNEL — live preview of the font + accent the listener client will use
        int fi = ofClamp(sFontIdx, 0, kNumFonts - 1);
        float bx = px + 40 * S, by = fy + 10 * S, bw = pw - 80 * S, bh = 64 * S;
        ofSetColor(20, 23, 25); ofDrawRectangle(bx, by, bw, bh);   // preview panel
        ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(70, 76, 74); ofDrawRectangle(bx, by, bw, bh); ofFill();
        // Accent shown LIVE: while the Accent field is being edited, preview the in-progress buffer (not the
        // committed value) so the colour tracks each keystroke; otherwise use the saved sAccent.
        std::string accStr = sAccent;
        if (editingField >= 0 && editingField < (int)fields.size() && fields[editingField].label == "Accent") accStr = fields[editingField].buf;
        ofColor acc(255, 59, 48); bool accValid = false;
        if (accStr.size() == 7 && accStr[0] == '#') {
            try { acc = ofColor(std::stoi(accStr.substr(1, 2), nullptr, 16), std::stoi(accStr.substr(3, 2), nullptr, 16), std::stoi(accStr.substr(5, 2), nullptr, 16)); accValid = true; } catch (...) {}
        }
        // Mirror the client EXACTLY: channel name + episode are neutral; ONLY the "º" carries the accent.
        // No colour block — the client shows no swatch. Drawn in the artist's chosen font.
        ofColor ink(226, 232, 228);
        std::string chan = (sChannel.empty() ? std::string("CHANNEL") : ofToUpper(sChannel)) + "  ";
        std::string deg  = "\xC2\xBA";   // º only (U+00BA) — no "N"
        std::string epi  = "  001";
        float tx = bx + 24 * S, ty = by + bh * 0.5f + 8 * S;
        // Normalized: translate to the shared baseline, scale this face to the common cap-height, draw at origin.
        ofPushMatrix();
        ofTranslate(tx, ty); ofScale(fPreviewScale[fi], fPreviewScale[fi]);
        ofSetColor(ink); fPreview[fi].drawString(chan, 0, 0);
        float x2 = fPreview[fi].stringWidth(chan);
        ofSetColor(accValid ? acc : ink); fPreview[fi].drawString(deg, x2, 0);         // the º — accent
        float x3 = x2 + fPreview[fi].stringWidth(deg);
        ofSetColor(ink); fPreview[fi].drawString(epi, x3, 0);
        ofPopMatrix();
        ofSetColor(120, 126, 124);
        std::string hint = "this is how your channel looks on the listener client \xC2\xB7 SAVE applies it to your live channel (~15s)";
        fUI.drawString(hint, px + 40 * S, by + bh + 26 * S);
        ofSetColor(150, 156, 154);
        fUI.drawString("Donations broadcast only after Wallet Backed Up = YES \xC2\xB7 no wallet? set one up at wallet.bullbitcoin.com", px + 40 * S, by + bh + 50 * S);
    }
    if (settingsTab == 4) {   // PUBLISH — button that POSTs recordings to the server as a collection + a status line
        publishBox = ofRectangle(px + 40 * S, fy + 10 * S, 200 * S, 44 * S);
        bool ready = sRegistered && !sCollectionName.empty();
        ofSetColor(ready ? ofColor(40, 60, 46) : ofColor(34, 38, 42)); ofDrawRectangle(publishBox);
        ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(ready ? ofColor(140, 200, 160) : ofColor(92, 100, 96)); ofDrawRectangle(publishBox); ofFill();
        ofSetColor(206, 212, 208);
        std::string plabel = "PUBLISH";
        ofRectangle bb = fUI.getStringBoundingBox(plabel, 0, 0);
        fUI.drawString(plabel, floorf(publishBox.x + (publishBox.width - bb.width) * 0.5f - bb.x), floorf(publishBox.y + (publishBox.height - bb.height) * 0.5f - bb.y));
        // status / progress line — updated from the detached upload thread (guarded by publishMtx)
        std::string ps; float pf;
        { std::lock_guard<std::mutex> lk(publishMtx); ps = publishStatus; pf = publishFlash; }
        if (!ps.empty()) {
            bool fresh = (t - pf) < 4.0f;
            ofSetColor(ofColor(150, 210, 170), fresh ? 255 : 170);
            fUI.drawString(ps, publishBox.getMaxX() + 18 * S, publishBox.y + 28 * S);
        }
        ofSetColor(120, 126, 124);
        std::string sub = sRegistered ? ("publishes your recordings to " + sRegServer + " under this collection name")
                                      : "register on a server first (REGISTER tab), then PUBLISH your recordings";
        fUI.drawString(sub, px + 40 * S, publishBox.getMaxY() + 26 * S);
    }
    bool saved = (t - saveFlash) < 1.6f;
    saveBox = ofRectangle(px + pw - 240 * S, py + ph - 72 * S, 200 * S, 46 * S);
    ofSetColor(saved ? ofColor(96, 102, 100) : ofColor(54, 60, 58)); ofDrawRectangle(saveBox);   // greyscale (no green)
    ofSetColor(cNeon); { std::string s = saved ? "SAVED" : "SAVE"; fValue.drawString(s, saveBox.x + saveBox.width * 0.5f - fValue.stringWidth(s) * 0.5f, saveBox.y + 31 * S); }
    if (settingsTab == 0)   // recording filename is a SESSION concern — don't repeat it under every tab
        { ofSetColor(150, 156, 154); fUI.drawString("RECORDING:   " + recBaseName() + ".mp4", px + 40 * S, py + ph - 104 * S); }
    ofSetColor(120, 126, 124); fUI.drawString("click a field to edit   \xC2\xB7   Cmd+V paste / Cmd+A clear   \xC2\xB7   [tab]/[enter] next   \xC2\xB7   [s]ave / [esc] close", px + 40 * S, py + ph - 40 * S);
}

//--------------------------------------------------------------
//  SHORTCUTS overlay ('h')
//--------------------------------------------------------------
void ofApp::drawHelp() {
    ofSetColor(0, 0, 0, 210); ofDrawRectangle(0, 0, RW, RH);
    static const std::pair<std::string, std::string> keys[] = {
        {"C", "show / hide control panels"},
        {"H", "show / hide this help"},
        {"I", "parameter help — hover any control"},
        {"S", "settings (session / routing / register / channel)"},
        {"R", "record — local capture only"},
        {"B", "broadcast + record (needs a server login)"},
        {"M", "cycle mode (orbit / vehicle / platform / helix / image)"},
        {"< >", "IMAGE mode: previous / next image"},
        {"TAB", "toggle layout (radial / grid)"},
        {"X", "reset all settings to defaults"},
        {"U", "show / hide the broadcast HUD"},
        {"T", "show / hide metadata + telemetry (default off)"},
        {"V", "show / hide the audio level meter (screen only)"},
        {"F", "fullscreen"},
        {"P", "save a screenshot to /tmp"},
    };
    int n = sizeof(keys) / sizeof(keys[0]);
    float pw = 900 * S, ph = (n * 52 + 168) * S, px = (RW - pw) * 0.5f, py = (RH - ph) * 0.5f;
    ofSetColor(12, 14, 16, 248); ofDrawRectangle(px, py, pw, ph);
    ofSetColor(210, 216, 212); fKick.drawString("SHORTCUTS", px + 44 * S, py + 60 * S);
    for (int i = 0; i < n; i++) {
        float y = py + 128 * S + i * 52 * S;
        ofRectangle chip(px + 44 * S, y - 27 * S, 96 * S, 40 * S);
        ofSetColor(34, 38, 42); ofDrawRectangle(chip);
        ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(92, 100, 96); ofDrawRectangle(chip); ofFill();
        ofSetColor(228, 234, 230);
        fValue.drawString(keys[i].first, chip.x + chip.width * 0.5f - fValue.stringWidth(keys[i].first) * 0.5f, y);   // key cap
        ofSetColor(176, 184, 180); fValue.drawString(keys[i].second, px + 164 * S, y);
    }
    ofSetColor(120, 126, 124); fUI.drawString("press  H  or  ESC  to close", px + 44 * S, py + ph - 38 * S);
}

//--------------------------------------------------------------
// parameter help ('i') — Bitwig-style: hover a control, read what it does + the gotchas
std::string ofApp::hoverParamKey() {
    ofRectangle r = displayRect();
    float fx = (ofGetMouseX() - r.x) * RW / r.width;
    float fy = (ofGetMouseY() - r.y) * RH / r.height;
    for (auto& s : sliders) {
        bool vis = sliderVisible(s) && (s.tab == -1 || (s.tab == rightTab && !(rightTab == 0 && graphSub == 1)));
        if (!vis) continue;
        if (!s.opts.empty()) {                                   // choice row: label + option buttons
            ofRectangle row = s.track; row.y -= 22 * S; row.height += 30 * S;
            for (auto& b : s.boxes) row = row.getUnion(b);
            if (row.inside(fx, fy)) return s.name;
        } else {
            ofRectangle hit = s.track; hit.y -= 22 * S; hit.height += 30 * S;
            if (hit.inside(fx, fy)) return s.name;
        }
    }
    if (camPadBox.inside(fx, fy)) return "Camera Angle";         // GRAPH/GLOBAL extras
    if (camZBox.inside(fx, fy))   return "Camera Z";
    if (lfoRateBox.inside(fx, fy)) return "LFO Rate";            // MOD/LFO
    for (int gi = 0; gi < 6; gi++) {                             // MODULATION slot controls (off-screen when not on the MOD tab)
        if (modAmtBox[gi].inside(fx, fy))  return "Target Value";
        if (modDestBox[gi].inside(fx, fy)) return "Mod Slot";
    }
    for (int k = 0; k < 3; k++) if (modBipBox[k].inside(fx, fy)) return "Bipolar";
    if (sourceBox[0].inside(fx, fy) || sourceBox[1].inside(fx, fy)) return "Mod Source";
    return "";
}

std::pair<std::string, std::string> ofApp::paramHelp(const std::string& k) {
    static const std::vector<std::pair<std::string, std::pair<std::string, std::string>>> H = {
        {"Type",     {"GRAPH TYPE", "Switches the whole visualizer between two independent engines.\n\nRADIAL stacks concentric FFT shapes from the centre out (a tunnel / portal). GRID lays the shape out in a matrix you build with Count / Trans / Twist.\n\nEach Type keeps its OWN full configuration — colours, mods, camera, everything — so switching back and forth never disturbs the other. (Shortcut: TAB.)"}},
        {"Mode",     {"SHAPE MODE", "The primitive that gets repeated: Orbit = circle, Vehicle = triangle, Platform = square, plus Helix-S (static) and Helix-D (dynamic) double-helix DNA.\n\nNote: rotation-based controls (Twist, Twist-X/Y) are invisible on Orbit/circles — a rotated circle looks identical. Use Vehicle or Platform to see them. (Shortcut: M.)"}},
        {"Fill",     {"FILL", "Two independent toggles. OUTLINE draws just the shape's edge (line art); FILL paints it solid. Select BOTH for a filled shape WITH a crisp outline on top.\n\nHeads-up: filled shapes are far heavier than outlines — at very high grid Count they overlap into big alpha masses and can slow things down. Line W and Glow act on the OUTLINE, so they need OUTLINE enabled."}},
        {"Falloff",  {"FALLOFF", "How line-width and fade taper from the centre outward.\n\nEUCLID = circular contours, DIAMOND = manhattan, FRAME = square contours, REVERSE = flips the taper so the OUTER edge is thickest and the centre thinnest. Shapes the silhouette of both layouts."}},
        {"Scale",    {"GLOBAL SCALE", "Uniform size of the entire visual. Purely visual zoom — it scales the whole stack/matrix about the centre and doesn't change the audio mapping."}},
        {"Glob Rot", {"GLOBAL ROTATE", "A fixed, static rotation of the whole grid (degrees). GRID-only — RADIAL ignores it.\n\nUnlike Cam/Spin this does not animate; it just sets a resting tilt. For continuous motion use the Cam rates."}},
        {"Line W",   {"LINE WIDTH", "Base stroke thickness of the shapes. Falloff tapers it across the stack, and audio modulates it per-band (in RADIAL, treble pushes the outer rings thicker).\n\nHas no visible effect with Fill set to FILL."}},
        {"Alpha",    {"ALPHA", "Base opacity (0–255) of the shapes. Audio raises it on top of this, so set the resting level here and let the signal bring the peaks up. Audio-reactive."}},
        {"Red",      {"RED", "Red channel of the base colour (0–255). The palette is greyscale by default (R=G=B); audio brightens all three together. Audio-reactive."}},
        {"Green",    {"GREEN", "Green channel of the base colour (0–255). Keep equal to Red/Blue for neutral grey; offset it for a tint. Audio-reactive."}},
        {"Blue",     {"BLUE", "Blue channel of the base colour (0–255). Keep equal to Red/Green for neutral grey; offset it for a tint. Audio-reactive."}},
        {"Spin X",   {"SPIN X  (radial)", "Per-ring accumulating twist about the X axis — each ring is rotated a little more than the one before, so the stack screws into a tunnel. RADIAL-only.\n\nThis is the soft, progressive rotate. For a rigid whole-object tumble use Cam X instead. Tiny values (±0.12/frame) go a long way."}},
        {"Spin Y",   {"SPIN Y  (radial)", "Per-ring accumulating twist about the Y axis (progressive, builds along the stack). RADIAL-only. Pair with Spin X/Z for a corkscrew. For a rigid tumble use Cam Y."}},
        {"Spin Z",   {"SPIN Z  (radial)", "Per-ring accumulating roll about the Z axis (progressive). RADIAL-only. This is the classic radial fan/twist. For a rigid roll of the whole image use Cam Z."}},
        {"Cam X",    {"CAM X", "Rigid camera tumble about the X axis — rotates the ENTIRE object as one, like turning a camera, with no internal twisting. Works in both layouts.\n\nThis accumulates (deg/frame) for continuous motion. To set a fixed angle instead, use the Camera Angle pad in GRAPH ▸ GLOBAL."}},
        {"Cam Y",    {"CAM Y", "Rigid camera tumble about the Y axis (whole object, no internal twist). Both layouts. Accumulates for continuous spin; for a static pose use the Camera Angle pad."}},
        {"Cam Z",    {"CAM Z", "Rigid camera roll about the Z axis (whole object). Both layouts. Accumulates for continuous roll; for a static pose use the Camera Angle Z slider."}},
        {"Centre",   {"CENTRE  (radial)", "Spacing between the concentric rings — effectively ring size / how far apart the stack sits. RADIAL-only. Small values pack the rings tight; large values spread them into a wide tunnel."}},
        {"Bands",    {"BANDS", "How many rings/cells the spectrum is split into (4–128). More bands = finer frequency detail but thinner, busier shapes.\n\nEach band maps to a slice of the FFT, low frequencies near the centre, highs toward the edge."}},
        {"Twist",    {"TWIST", "RADIAL: per-ring fan rotation (degrees) — visibly skews Triangle/Square stacks. GRID: a per-cell local spin. Invisible on circles (Orbit).\n\nIn the helix it offsets the two strands' groove."}},
        {"Glow",     {"GLOW  (radial)", "Additive bloom halo around the lines — extra passes drawn wider and fainter for a soft light. RADIAL-only. Audio drives the bloom, so it blooms on peaks. 0 = off."}},
        {"Count-X",  {"COUNT-X  (grid)", "Number of columns in the matrix (mirrored ±, so the row is 2·N+1 wide). GRID-only.\n\nGOTCHA: with Trans-X at 0 every column sits on top of the others at the centre — you'll see ONE shape no matter how high Count-X is. Raise Trans-X to spread them apart and reveal the count."}},
        {"Count-Y",  {"COUNT-Y  (grid)", "Number of rows in the matrix (mirrored ±). GRID-only.\n\nGOTCHA: like Count-X, rows stack invisibly on top of each other until Trans-Y spreads them vertically. Count sets how many, Trans sets how far apart."}},
        {"Trans-X",  {"TRANS-X  (grid)", "Horizontal spacing between columns. GRID-only. This is what makes Count-X visible — without it the columns overlap at the centre. Increase to fan the matrix out sideways."}},
        {"Trans-Y",  {"TRANS-Y  (grid)", "Vertical spacing between rows. GRID-only. Makes Count-Y visible — rows stack on each other until you raise this. Together with Trans-X it sets the matrix's overall footprint."}},
        {"Twist-X",  {"TWIST-X  (grid)", "Per-column rotation: each column is rotated a bit more than the last. GRID-only.\n\nMost visible on Platform/Vehicle (squares/triangles) once Trans-X has spread the columns out. On Orbit (circles) a rotation is invisible. Pair with Trans-X."}},
        {"Twist-Y",  {"TWIST-Y  (grid)", "Per-row rotation: each row sweeps a bit more than the last, around the grid centre. GRID-only. Spread rows with Trans-Y first so the sweep reads clearly."}},
        {"Pinch",    {"PINCH  (grid)", "Tapers row scale from one edge to the other, squeezing the matrix into a wedge/perspective shape. GRID-only. Needs Count-Y > 0 and some Trans-Y to be visible."}},
        {"Shift-Y",  {"SHIFT-Y  (grid)", "Offsets each cell vertically inside its own slot before it's drawn. GRID-only. Combined with Twist-X/Y it throws cells off-axis for woven, off-grid patterns."}},
        {"Scale-X",  {"SCALE-X  (grid)", "Per-cell horizontal scale (cell width). GRID-only. Audio pulses this on top. Stretch wide for bars, shrink for dots."}},
        {"Scale-Y",  {"SCALE-Y  (grid)", "Per-cell vertical scale (cell height). GRID-only. Audio pulses this on top. Combine with Scale-X for the cell's aspect ratio."}},
        {"Rate",     {"RATE  (audio)", "FFT smoothing / trail length. 0 = snappy and instant; ~0.95 = long, smeared trails that ease between frames.\n\nNeeds live audio to show any effect — it shapes how the rings react, not their resting look."}},
        {"Spread",   {"SPREAD  (audio)", "How far the audio pushes each ring/cell — the depth of the reaction. Low = subtle breathing; high = big jumps on every band."}},
        {"Punch",    {"PUNCH  (audio)", "How hard the detected beat/kick slams the whole visual. 0 = ignore beats (steady); higher = the entire image jumps on each kick. Drives the beat envelope, separate from per-band Spread."}},
        {"Camera Angle", {"CAMERA ANGLE  (pad)", "Drag to set a FIXED camera pose: left/right = Y angle, up/down = X angle. Unlike the Cam rates (which spin continuously) this is a static tilt.\n\nIt's stored in presets, so you can save a look from exactly the angle you framed. Use the Z slider beside it for roll."}},
        {"Camera Z",     {"CAMERA Z ROLL", "Static roll angle of the camera (the Z partner of the XY pad). Sets a fixed tilt rather than a continuous spin. Saved into presets along with the pad."}},
        {"LFO Rate",     {"LFO RATE", "Frequency of the LFO that drives LFO-Mod routes. The scale is the Schumann resonance (7.83 Hz) stepped down in octaves — use < / > to lock to clean octaves.\n\nVery slow rates (low octaves) give long, glacial sweeps; faster rates wobble. Bind it to a parameter in the MOD tab."}},
        {"Target Value", {"TARGET VALUE", "The far end of a modulation's travel. The bound parameter rides between its current value (signal at rest) and this Target Value (full drive).\n\nIt starts equal to the current value — no movement — so binding never causes a jump. Drag it away to open up the range; drag it BELOW the current value to push the parameter down instead of up."}},
        {"Bipolar",      {"BIPOLAR  (LFO)", "Switches the LFO swing between one-sided and two-sided. Unipolar (off) moves the parameter from its base in a single direction toward the Target Value. Bipolar (the ± toggle, on) swings it EQUALLY to both sides of the base, dipping below as well as rising above.\n\nUse bipolar for a symmetric wobble around a centre; unipolar for a one-directional pump."}},
        {"Mod Slot",     {"MODULATION SLOT", "One modulation route: a destination parameter plus its Target Value. Click to pick (or re-pick) which control it drives — any pulsing control in the left column. The × clears it, leaving the parameter frozen where it is. A parameter can be driven by only one slot at a time."}},
        {"Mod Source",   {"MOD SOURCE", "Chooses the engine for this tab's three slots: AUDIO follows the live signal's loudness; LFO follows an internal sine wave (set its speed with LFO Rate). Each source keeps its own independent three slots, shown one source at a time."}},
    };
    for (auto& e : H) if (e.first == k) return e.second;
    return {k, "No description available for this control yet."};
}

// the centred card shared by parameter help ('i') and the modulation pick-mode — slots into the gap
// BETWEEN the left config column and the right tab panel so it never covers either set of controls.
void ofApp::drawHelpCard(const std::string& eyebrow, const std::string& title, const std::string& tag,
                         const std::string& body, const std::string& footer) {
    float colW = 320 * S, gutter = 16 * S, margin = 44 * S;
    float leftEnd    = fm + 20 * S + colW + gutter;                  // right edge of the left GRAPH-CONFIG panel
    float rightStart = (RW - fm - 20 * S - colW) - gutter;           // left edge of the right tab panel
    float px = leftEnd + margin, pw = (rightStart - margin) - px;    // centred in the middle band
    float pad = 48 * S, maxw = pw - pad * 2;
    float lh = fValue.getLineHeight() * 1.34f;
    std::vector<std::string> lines; { std::string line, word;       // word-wrap (honours explicit \n)
        auto flushWord = [&]() {
            if (word.empty()) return;
            std::string trial = line.empty() ? word : line + " " + word;
            if (fValue.stringWidth(trial) > maxw && !line.empty()) { lines.push_back(line); line = word; }
            else line = trial;
            word.clear();
        };
        for (char c : body) {
            if (c == '\n') { flushWord(); lines.push_back(line); line.clear(); }
            else if (c == ' ') flushWord();
            else word += c;
        }
        flushWord(); if (!line.empty()) lines.push_back(line);
    }
    bool hasTag = !tag.empty();
    float headH = hasTag ? 150 * S : 118 * S, footH = footer.empty() ? 30 * S : 56 * S;
    float ph = headH + lines.size() * lh + footH;
    float py = (RH - ph) * 0.5f;

    ofSetColor(9, 11, 13, 250); ofDrawRectangle(px, py, pw, ph);                  // card
    ofNoFill(); ofSetLineWidth(1.0f * S); ofSetColor(78, 86, 82); ofDrawRectangle(px, py, pw, ph); ofFill();
    ofSetColor(150, 156, 154); ofDrawRectangle(px, py, 4 * S, ph);                // accent spine

    ofSetColor(120, 130, 126); fUI.drawString(eyebrow, px + pad, py + 46 * S);    // eyebrow
    ofSetColor(232, 238, 234); fTitle.drawString(title, px + pad, py + 92 * S);   // title
    if (hasTag) { ofSetColor(132, 140, 137); fUI.drawString(tag, px + pad, py + 122 * S); }   // scope / range / hint

    float ty = py + headH + fValue.getAscenderHeight();                           // body
    for (auto& ln : lines) { ofSetColor(192, 200, 196); fValue.drawString(ln, px + pad, ty); ty += lh; }

    if (!footer.empty()) { ofSetColor(118, 124, 122); fUI.drawString(footer, px + pad, py + ph - 24 * S); }
}

void ofApp::drawParamHelp() {
    if (modPickKind >= 0) return;                                // the modulation card (drawn in drawPanels) takes precedence
    if (activeSlider >= 0 || activeModAmt >= 0 || camDrag != 0) return;   // dragging: show nothing, let the art breathe
    std::string key = hoverParamKey();
    if (key.empty()) return;                                     // not hovering a control: show nothing at all
    auto info = paramHelp(key);
    Slider* sp = nullptr; for (auto& s : sliders) if (s.name == key) { sp = &s; break; }   // for range / value / scope tag
    std::string tag;
    if (sp) {
        if (sp->show == 1) tag = "RADIAL ONLY";
        else if (sp->show == 2) tag = "GRID ONLY";
        else tag = "BOTH LAYOUTS";
        if (sp->opts.empty()) tag += "      RANGE  " + ofToString(sp->lo, sp->prec) + "  -  " + ofToString(sp->hi, sp->prec)
                                    + "      NOW  " + ofToString(*sp->val, sp->prec);
    }
    drawHelpCard("PARAMETER", info.first, tag, info.second, "press  i  to exit help");
}

//--------------------------------------------------------------
std::string ofApp::recBaseName() const {
    auto pad = [](int v, int w){ std::string s = ofToString(v); while ((int)s.size() < w) s = "0" + s; return s; };
    std::string raw = sArtifact + "-" + sShorthand + pad(sTransmission, 3) + "-" + sArtist + "-" + sTitle;   // e.g. HelioGraph-TxN001-artist-Deuce
    std::string safe; for (char c : raw) {                                                    // lowercase + sanitise
        if (isalnum((unsigned char)c))      safe += (char)tolower((unsigned char)c);
        else if (c == '_' || c == '-')      safe += c;
        else                                safe += '_';
    }
    return safe;   // -> heliograph-txn001-artist
}
std::string ofApp::recDir() {
    if (!sRecDir.empty()) {                                    // user-chosen parent -> always a HelioRecordings/ subfolder inside it
        std::string d = sRecDir;
        if (d.empty() || d.back() != '/') d += '/';
        d += "HelioRecordings/";
        ofDirectory::createDirectory(d, false, true);
        return d;
    }
    return gsRecDir();                                         // default ~/.heliograph/recordings/
}
bool ofApp::pickRecDir() {                                     // native folder chooser
    std::string start = sRecDir.empty() ? gsVideosDir() : sRecDir;
    ofFileDialogResult r = ofSystemLoadDialog("Choose a folder for your recordings", true, start);
    if (r.bSuccess && !r.getPath().empty()) { sRecDir = r.getPath(); return true; }
    return false;
}
void ofApp::startRecording() {
    if (recording) return;
    if (sRecDir.empty()) { if (pickRecDir()) writeSession(); }   // first record: ask where to save (cancel -> default ~/.heliograph/recordings)
    std::string dir = recDir();
    std::string safe = recBaseName();   // artifact + shorthand + episode + artist (lowercase); changing any makes a new file
    recFinalPath = dir + safe + ".mp4";

    if (!recWarn && ofFile::doesFileExist(recFinalPath)) { recWarn = true; return; }   // warn once before overwriting
    recWarn = false;

    { std::lock_guard<std::mutex> lock(mtx); recAudio.clear(); }
    std::string vidTmp = dir + safe + ".video.mp4", log = gsScratch("gs_ffmpeg.log");
    std::string cmd = gsFFmpeg() + " -y -f rawvideo -pixel_format rgba -video_size " +
        ofToString(REC_W) + "x" + ofToString(REC_H) + " -framerate 30 -i - -an "
        GS_VCODEC " \"" + vidTmp + "\" 2>>\"" + log + "\"";
    vidPipe = GS_POPEN(cmd.c_str(), GS_PIPEMODE);
    recFrames = 0; recStart = t; recording = true;
    ofLogNotice() << "REC start -> " << recFinalPath;
}

void ofApp::stopRecording() {
    if (!recording) return;
    recording = false;
    // Hand everything off and FINALIZE ON A BACKGROUND THREAD. The mux was a synchronous system() call
    // on the main thread — it froze the whole UI for the length of the recording (and, while broadcasting,
    // backed up the live pipe until it blocked too). The UI now stays responsive; the MP4 lands a moment later.
    FILE* vp = vidPipe; vidPipe = nullptr;
    std::string finalPath = recFinalPath;
    std::vector<short> audio;
    { std::lock_guard<std::mutex> lock(mtx); audio.swap(recAudio); }   // take the buffer under lock; do NOT hold it during I/O
    int chans = recChannels, rate = sampleRate;
    std::thread([vp, finalPath, audio = std::move(audio), chans, rate]() mutable {
        if (vp) GS_PCLOSE(vp);                                         // flush + wait for the video encoder to finish
        std::string base = finalPath.substr(0, finalPath.size() - 4);
        std::string vidTmp = base + ".video.mp4";
        std::string wavTmp = base + ".audio.wav";                      // per-recording temp — no shared-scratch collision
        std::string log = gsScratch("gs_ffmpeg.log");
        writeWav(wavTmp, audio, chans, rate);
        std::string mux = gsFFmpeg() + " -y -i \"" + vidTmp + "\" -i \"" + wavTmp +
            "\" -c:v copy -c:a aac -b:a 320k -shortest \"" + finalPath + "\" 2>>\"" + log + "\"";
        system(mux.c_str());
        ofFile::removeFile(vidTmp);   // portable cleanup (no shell 'rm')
        ofFile::removeFile(wavTmp);
        ofLogNotice() << "REC saved -> " << finalPath;
    }).detach();
}

//--------------------------------------------------------------
//  BROADCAST — live Icecast forwarding (audio) + periodic snapshot push (visual), independent of local
//  recording. Config lives in the settings editor's BROADCAST section (see buildFields/writeSession).
//--------------------------------------------------------------
// Launch (or relaunch) the ffmpeg -> icecast pipe and make it non-blocking. Used by startBroadcast AND
// the auto-reconnect path in update(). Returns false if ffmpeg couldn't be spawned.
bool ofApp::openIcePipe() {
    std::string port  = sIcePort.empty()  ? "8000"     : sIcePort;
    std::string mount = sIceMount.empty() ? "live.mp3" : sIceMount;
    std::string url = "icecast://source:" + sIcePassword + "@" + sIceHost + ":" + port + "/" + mount;
    std::string log = gsScratch("gs_broadcast.log");
    // 320 kbps CBR MP3 via LAME at the source's native rate + 16-bit — universal browser <audio> playback.
    // -hide_banner -loglevel warning -nostats: no per-second progress spam; the log holds only the CURRENT
    // attempt's warnings/errors (single '>' truncates), so iceAuthFailed() can read a small, relevant log.
    std::string cmd = gsFFmpeg() + " -hide_banner -loglevel warning -nostats -y -f s16le -ar " + ofToString(sampleRate) +
        " -ac " + ofToString(std::max(1, recChannels)) +
        " -i - -c:a libmp3lame -b:a 320k -f mp3 -content_type audio/mpeg " + gsShQuote(url) + " >" + gsShQuote(log) + " 2>&1";
    icePipe = GS_POPEN(cmd.c_str(), GS_PIPEMODE);
    if (!icePipe) return false;
    iceOutBuf.clear();
#ifndef _WIN32
    // Non-blocking: if Icecast stalls, ffmpeg stops draining stdin; a blocking write would hang the UI.
    iceFd = fileno(icePipe);
    if (iceFd >= 0) { int fl = fcntl(iceFd, F_GETFL, 0); if (fl != -1) fcntl(iceFd, F_SETFL, fl | O_NONBLOCK); }
#endif
    return true;
}

// True ONLY for a credential rejection (bad/revoked Icecast source password) — the one failure retrying
// can't fix. Reads the current attempt's ffmpeg log (truncated per spawn). Everything else — 5xx,
// timeouts, "connection refused/reset", "network unreachable", 403 mount-in-use — returns false → retry.
bool ofApp::iceAuthFailed() {
    std::ifstream in(gsScratch("gs_broadcast.log"));
    if (!in) return false;
    std::string all, line;
    while (std::getline(in, line)) all += line + "\n";
    for (char& c : all) if (c >= 'A' && c <= 'Z') c += 32;   // lowercase
    // Match only GENUINE auth signatures. Never match a bare "401": ffmpeg prints pointer
    // addresses like "0x933040180" that contain "401", which would false-positive and
    // permanently kill a perfectly recoverable broadcast (e.g. an Icecast broken pipe on a
    // network blip). Require the actual word or a real 401 phrase instead.
    return all.find("unauthorized") != std::string::npos
        || all.find("authentication failed") != std::string::npos
        || all.find("http error 401") != std::string::npos
        || all.find("server returned 401") != std::string::npos;
}

void ofApp::startBroadcast() {
    if (broadcasting) return;
    if (!sRegistered || sIceMount.empty() || sIcePassword.empty()) {
        // gate broadcasting behind a real registration — surface it to the artist, don't fail silently
        errMsg = "You need an account on this server to broadcast \xE2\x80\x94 register in S \xE2\x86\x92 REGISTER"; errFlash = t;
        ofLogError() << "BROADCAST: not registered — register (E -> REGISTER) with an invite code first";
        return;
    }
    { std::lock_guard<std::mutex> lock(mtx); broadcastAudioQueue.clear(); }
    if (!openIcePipe()) { ofLogError() << "BROADCAST: failed to launch ffmpeg"; return; }
    broadcasting = true;
    ofBufferToFile(gsHome() + "broadcasting.flag", ofBuffer("1", 1));   // resilience sentinel — survives a crash/kill so the next launch auto-resumes
    bcastAway = false;   // you just hit B — you're at the console (ON DECK) until you press A
    broadcastStart = t;
    iceReconnectDelay = 1.0f; iceReconnectAt = 0;
    bcastReconnects = 0; iceWasDown = false; lastBcastBeat = t;
    lastSnapshotT = -100;   // push a snapshot on the very next update() tick
    // B and R are INDEPENDENT: broadcasting never touches local recording. Press R to also record.
    ofLogNotice() << ofGetTimestampString("%H:%M:%S") << "  BROADCAST start -> " << sIceHost << ":" << sIcePort << "/" << sIceMount;
}

void ofApp::stopBroadcast() {
    if (!broadcasting) return;
    broadcasting = false;
    ofFile::removeFile(gsHome() + "broadcasting.flag", false);   // deliberate/auth stop — do NOT auto-resume on next launch
    iceFd = -1; iceOutBuf.clear();
    // Reap the ffmpeg pipe OFF the main thread: pclose() waits for ffmpeg to exit, and if it's wedged on
    // a stalled Icecast socket that never happens — a synchronous close here froze the UI on stop. The
    // detached reaper closes the fd (EOF → ffmpeg finishes, or Icecast times out the source) without
    // ever blocking the UI.
    FILE* p = icePipe; icePipe = nullptr;
    if (p) std::thread([p]{ GS_PCLOSE(p); }).detach();
    { std::lock_guard<std::mutex> lock(mtx); broadcastAudioQueue.clear(); }
    // Recording is independent — a manual recording (R) keeps running if the artist started one.
    ofLogNotice() << ofGetTimestampString("%H:%M:%S") << "  BROADCAST stop (reconnects this session=" << bcastReconnects << ")";
}

// On startup, confirm we're still registered server-side (the server could have been reset). NON-BLOCKING:
// fire a detached curl to /whoami that writes to a scratch file; update() polls the file so launch never
// stalls on the network. If unreachable, we keep the locally-saved registration.
void ofApp::verifyRegistration() {
    if (!sRegistered || sChannelId.empty()) return;
    std::string server = sRegServer;
    while (!server.empty() && server.back() == '/') server.pop_back();
    if (server.empty()) return;
    std::string outFile = gsScratch("hg_whoami.json");
    ofFile::removeFile(outFile);
    std::string cmd = "curl -s -m 8 " + gsShQuote(server + "/whoami?channelId=" + sChannelId) +
                      " -o " + gsShQuote(outFile) + " 2>" + gsNullDev() + " &";
    system(cmd.c_str());
    regVerifyPending = true; regVerifyT = t;
}

// Register this artist with the server: POST {inviteCode, artist} to <sRegServer>/register. On success
// the server hands back the full broadcast config (icecast host/port/mount, shared source password,
// per-channel snapshot URL) which we save — so the artist never touches raw hosts, and the single-use
// invite is consumed. Errors (bad code, name taken, unreachable) are surfaced in regStatus.
void ofApp::registerArtist() {
    std::string server = sRegServer;
    while (!server.empty() && server.back() == '/') server.pop_back();
    if (server.empty())      { regStatus = "Enter the registration server URL."; regFlash = t; return; }
    if (sInviteCode.empty()) { regStatus = "Enter your invite code."; regFlash = t; return; }
    if (sArtist.empty())     { regStatus = "Enter your artist name."; regFlash = t; return; }

    ofJson body; body["inviteCode"] = sInviteCode; body["artist"] = sArtist;
    std::string bodyFile = gsScratch("hg_register_body.json");
    { std::ofstream o(bodyFile); o << body.dump(); }
    // -w prints the HTTP status as a trailing line after the response body, so we can read both from popen.
    std::string cmd = "curl -s -m 15 -w " + gsShQuote("\n%{http_code}") +
        " -X POST -H " + gsShQuote("Content-Type: application/json") +
        " --data-binary @" + gsShQuote(bodyFile) + " " + gsShQuote(server + "/register") + " 2>" + gsNullDev();
    std::string out;
    if (FILE* p = GS_POPEN(cmd.c_str(), "r")) {
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
        GS_PCLOSE(p);
    }
    ofFile::removeFile(bodyFile);

    std::string httpCode, respBody = out;
    size_t nl = out.find_last_of('\n');
    if (nl != std::string::npos) { httpCode = out.substr(nl + 1); respBody = out.substr(0, nl); }
    while (!httpCode.empty() && !isdigit((unsigned char)httpCode.back())) httpCode.pop_back();

    regFlash = t;
    if (httpCode.empty()) { regStatus = "Could not reach " + server + " — check the URL and your connection."; sRegistered = false; return; }

    ofJson resp;
    try { resp = ofJson::parse(respBody); } catch (...) {}

    if (httpCode == "200" && resp.value("ok", false)) {
        sChannelId     = resp.value("channelId", std::string(""));
        ofJson ice     = resp.contains("icecast") ? resp["icecast"] : ofJson::object();
        sIceHost       = ice.value("host", server);
        sIcePort       = ofToString(ice.value("port", 8000));
        sIceMount      = ice.value("mount", sChannelId);
        sIcePassword   = ice.value("password", std::string(""));
        sSnapshotUrl   = resp.value("snapshotUrl", std::string(""));
        sSnapshotToken = resp.value("snapshotToken", std::string(""));
        sRegistered    = true;
        sInviteCode    = "";   // single-use — consumed
        regStatus = "Registered as " + sArtist + "  \xC2\xB7  channel " + sChannelId.substr(0, std::min<size_t>(8, sChannelId.size()));
        writeSession();
        ofLogNotice() << "REGISTERED as " << sArtist << " channel=" << sChannelId << " mount=" << sIceMount;
    } else {
        std::string err = resp.value("error", std::string(""));
        if (err.empty()) err = "registration failed (HTTP " + (httpCode.empty() ? std::string("?") : httpCode) + ")";
        regStatus = err;
        sRegistered = false;
        ofLogError() << "REGISTER failed: " << err;
    }
}

// Thread-safe setter for publishStatus: publishCollection() runs its upload on a detached thread, so both
// that thread and the main thread (drawSettings) touch this string — guard every write under publishMtx.
void ofApp::setPublishStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(publishMtx);
    publishStatus = s;
    publishFlash  = t;
}

// Publish the local recordings as a named collection on the registered server — parity with the `helio`
// CLI, using the app's EXISTING shell-out architecture (curl + ffmpeg via system()/popen(); NO addon, NO
// in-process HTTP). First POSTs {server}/collections to create/reuse the collection (parses the returned
// id), then for each *.mp4 in recDir() extracts audio to a temp mp3 (ffmpeg) and POSTs it to
// {server}/collections/{id}/tracks with the Bearer token + X-Track-Name/X-Track-Ext headers. The whole
// upload runs on a DETACHED thread so a slow/large transfer never blocks the UI; progress is surfaced via
// setPublishStatus() (guarded) and shown in the PUBLISH tab.
void ofApp::publishCollection() {
    if (!sRegistered)             { setPublishStatus("Register on a server first (S \xE2\x86\x92 REGISTER)."); return; }
    if (sCollectionName.empty())  { setPublishStatus("Enter a Collection Name."); return; }
    if (sSnapshotToken.empty())   { setPublishStatus("No auth token \xE2\x80\x94 re-register on the server."); return; }
    std::string server = sRegServer;
    while (!server.empty() && server.back() == '/') server.pop_back();
    if (server.empty())           { setPublishStatus("Enter the registration server URL."); return; }

    // Snapshot the values the thread needs now (fields could change while the upload runs).
    std::string token = sSnapshotToken, artist = sArtist, name = sCollectionName, dir = recDir();
    std::string artStyle = kArtStyles[(int)ofClamp(sCollectionArtIdx, 0, kNumArtStyles - 1)];
    setPublishStatus("Publishing\xE2\x80\xA6");
    // Persist the collection name up front (main thread), now that a publish was started.
    writeSession();

    std::thread([this, server, token, artist, name, dir, artStyle]() {
        // 1) Create/reuse the collection. Mirror registerArtist(): write the JSON body to a scratch file,
        //    POST it, read stdout via popen, split the trailing -w '%{http_code}' line, parse {id}.
        // NOTE (Windows caveat — see report): gsShQuote() single-quotes args, which POSIX sh honors but
        // cmd.exe does NOT. This whole shell-out layer (snapshot/register/ffmpeg AND these collection calls)
        // needs a cross-platform quoting fix (double-quote + '^'-escape) before it will run on Windows.
        ofJson body; body["artist"] = artist; body["name"] = name; body["artStyle"] = artStyle;
        std::string bodyFile = gsScratch("hg_collection_body.json");
        { std::ofstream o(bodyFile); o << body.dump(); }
        std::string cmd = "curl -s -m 20 -w " + gsShQuote("\n%{http_code}") +
            " -X POST -H " + gsShQuote("Authorization: Bearer " + token) +
            " -H " + gsShQuote("Content-Type: application/json") +
            " --data-binary @" + gsShQuote(bodyFile) + " " + gsShQuote(server + "/collections") + " 2>" + gsNullDev();
        std::string out;
        if (FILE* p = GS_POPEN(cmd.c_str(), "r")) {
            char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
            GS_PCLOSE(p);
        }
        ofFile::removeFile(bodyFile);

        std::string httpCode, respBody = out;
        size_t nl = out.find_last_of('\n');
        if (nl != std::string::npos) { httpCode = out.substr(nl + 1); respBody = out.substr(0, nl); }
        while (!httpCode.empty() && !isdigit((unsigned char)httpCode.back())) httpCode.pop_back();

        std::string id;
        try { ofJson r = ofJson::parse(respBody); if (r.value("ok", false)) id = r.value("id", std::string("")); } catch (...) {}
        if (id.empty()) {
            setPublishStatus("Could not create collection (HTTP " + (httpCode.empty() ? std::string("?") : httpCode) + ").");
            ofLogError() << "PUBLISH: create collection failed, http=" << httpCode << " body=" << respBody;
            return;
        }

        // 2) Enumerate the recordings folder for *.mp4.
        ofDirectory d(dir); d.allowExt("mp4"); d.listDir();
        int total = (int)d.size();
        if (total == 0) { setPublishStatus("No recordings (*.mp4) found in " + dir); return; }

        // 3) For each recording: extract audio-only mp3 (ffmpeg), then POST it as a track. Skip failures
        //    and continue; report the count at the end.
        std::string log = gsScratch("gs_publish.log");
        std::string respTmp = gsScratch("hg_pub_resp.bin");   // discard the upload response body (portable — not /dev/null)
        int done = 0;
        for (int i = 0; i < total; i++) {
            std::string mp4  = d.getPath(i);
            std::string stem = ofFilePath::getBaseName(mp4);   // track name = the mp4's filename stem
            setPublishStatus("Uploading " + ofToString(i + 1) + "/" + ofToString(total) + "\xE2\x80\xA6");
            std::string mp3 = gsScratch("hg_pub_" + ofToString(i) + ".mp3");
            ofFile::removeFile(mp3);
            std::string ff = gsFFmpeg() + " -y -i " + gsShQuote(mp4) + " -vn -c:a libmp3lame -b:a 320k " +
                             gsShQuote(mp3) + " 2>>" + gsShQuote(log);
            system(ff.c_str());
            if (!ofFile::doesFileExist(mp3)) { ofLogError() << "PUBLISH: audio extract failed for " << mp4; continue; }
            // Generous timeout: tracks are large. Cap the CONNECT time only (no total -m cap) so a big but
            // healthy upload isn't killed mid-transfer. -w prints just the HTTP status for the result check.
            std::string up = "curl -s --connect-timeout 20 -o " + gsShQuote(respTmp) + " -w " + gsShQuote("%{http_code}") +
                " -X POST -H " + gsShQuote("Authorization: Bearer " + token) +
                " -H " + gsShQuote("X-Track-Name: " + gsBase64(stem)) +
                " -H " + gsShQuote("X-Track-Ext: mp3") +
                " -H " + gsShQuote("Content-Type: application/octet-stream") +
                " --data-binary @" + gsShQuote(mp3) + " " + gsShQuote(server + "/collections/" + id + "/tracks") +
                " 2>>" + gsShQuote(log);
            std::string code;
            if (FILE* p = GS_POPEN(up.c_str(), "r")) { char b[64]; size_t n; while ((n = fread(b, 1, sizeof(b), p)) > 0) code.append(b, n); GS_PCLOSE(p); }
            ofFile::removeFile(mp3);
            if (!code.empty() && code[0] == '2') { done++; ofLogNotice() << "PUBLISH: uploaded " << stem; }
            else ofLogError() << "PUBLISH: upload failed for " << stem << " (HTTP " << code << ")";
        }
        ofFile::removeFile(respTmp);
        setPublishStatus("Published " + ofToString(done) + "/" + ofToString(total) +
                         " track" + (total == 1 ? "" : "s") + " to \"" + name + "\".");
        ofLogNotice() << "PUBLISH: done " << done << "/" << total << " -> collection " << id;
    }).detach();
}

// Grabs the last fully-rendered frame (fboFinal, one app-frame stale at most — irrelevant at a 1.5s
// interval), downscales it, and hands it to `curl` as a detached background process so a slow/stalled
// upload never blocks rendering.
void ofApp::pushSnapshot() {
    if (sSnapshotUrl.empty()) return;
    // Broadcast frame = the CLEAN scene only. Re-render drawScene() into fboSnap rather than copying
    // fboFinal (which carries heliograph's local HUD — the always-on corner ticks and, if showMeta is on,
    // the metadata text). The client draws its OWN corner markers + metadata from the fields we send, so
    // baking them here just doubled them (and heliograph's baked font aliased). fboSnap is RW×RH, same as
    // the scene, so drawScene() fills it 1:1.
    fboSnap.begin();
    ofClear(0, 0, 0, 255);
    ofSetColor(255);
    drawScene();
    ofEnableBlendMode(OF_BLENDMODE_ALPHA);
    fboSnap.end();
    ofPixels px; fboSnap.readToPixels(px);
    // PNG, not JPEG — the snapshot is a crisp HUD/vector-ish frame where JPEG's blocky lossy artifacts
    // are very visible. PNG is lossless (no compression artifacts at all); BEST just picks the smallest
    // lossless zlib level. Infrequent pushes (every SNAPSHOT_INTERVAL s) keep the larger file affordable.
    std::string path = gsScratch("heliograph_snapshot.png");
    ofSaveImage(px, path, OF_IMAGE_QUALITY_BEST);

    // Full transmission metadata rides with the frame so the client can show the same telemetry as the
    // on-screen HUD (channel/episode/artist/title/note/movement/HDG/waypoint/uptime). base64 header so
    // unicode titles/notes survive; one request so we only need the single configured snapshot URL.
    ofJson meta;
    meta["channel"]  = sChannel;
    meta["episode"]  = sTransmission;
    meta["artist"]   = sArtist;
    meta["title"]    = sTitle;
    meta["note"]     = sNote;
    meta["font"]     = kFontKeys[(int)ofClamp(sFontIdx, 0, kNumFonts - 1)];   // CHANNEL branding — client themes to match
    meta["accent"]   = sAccent;
    // Donation addresses — only broadcast once the artist confirmed the wallet is backed up, and only if valid.
    if (sWalletBackedUp) {
        if (gsValidLightning(sLnAddress))                                 meta["ln"]  = sLnAddress;
        if (gsValidAddr(sBtcAddress) && sBtcAddress.rfind("bc1", 0) == 0) meta["btc"] = sBtcAddress;
        if (gsValidAddr(sLqAddress)  && sLqAddress.rfind("lq1", 0) == 0)  meta["lq"]  = sLqAddress;
    }
    meta["level"]    = ofClamp(level, 0.0f, 1.0f);   // smoothed RMS loudness — drives the client console meter fill
    meta["peak"]     = ofClamp(peak,  0.0f, 1.0f);   // decaying peak-hold — the peak marker on the client console meter
    meta["waypoint"] = sWaypoint;
    meta["hdgFreq"]  = subFreq;                 // detected primary sub-frequency (Hz) — HUD's HDG value
    meta["hdgNote"]  = subNote;                 // its note letter
    meta["date"]     = sDate;
    meta["uptime"]   = (int)(t - broadcastStart);   // seconds on air
    meta["status"]   = bcastAway ? "AWAY" : "ON DECK";   // broadcaster presence — client shows it in place of LISTENING
    std::string metaB64 = gsBase64(meta.dump());

    std::string log = gsScratch("gs_snapshot.log");
    std::string auth = sSnapshotToken.empty() ? "" : (" -H " + gsShQuote("Authorization: Bearer " + sSnapshotToken));
    std::string cmd = "curl -s -m 5 -T " + gsShQuote(path) + auth + " -H \"Content-Type: image/png\""
        " -H " + gsShQuote("X-Captured-At: " + ofToString(gsEpochMs())) +
        " -H " + gsShQuote("X-Transmission: " + metaB64) + " " +
        gsShQuote(sSnapshotUrl) + " >>" + gsShQuote(log) + " 2>&1 &";   // trailing & backgrounds it — fire and forget
    system(cmd.c_str());
}

// Inject the current wall-clock epoch-ms into the Icecast stream's ICY metadata (StreamTitle). Because
// this rides INSIDE the audio stream, the client reads it exactly when that audio hits the playhead, so
// (clientNow - timestamp) is the TRUE end-to-end audio lag — buffering and all. (Assumes the artist and
// listener clocks are NTP-synced; the residual offset is tiny next to a multi-second stream lag.)
// Uses the source credentials against Icecast's admin/metadata endpoint — same host/port as the stream.
void ofApp::pushStreamTimestamp() {
    if (sIceHost.empty() || sIcePassword.empty()) return;
    std::string port  = sIcePort.empty()  ? "8000"     : sIcePort;
    std::string mount = sIceMount.empty() ? "live.mp3" : sIceMount;
    std::string url = "http://" + sIceHost + ":" + port + "/admin/metadata?mount=/" + mount +
                      "&mode=updinfo&song=" + ofToString(gsEpochMs());
    std::string log = gsScratch("gs_streamts.log");
    std::string cmd = "curl -s -m 3 -u " + gsShQuote("source:" + sIcePassword) + " " + gsShQuote(url) +
                      " >>" + gsShQuote(log) + " 2>&1 &";   // backgrounded — fire and forget
    system(cmd.c_str());
}

//--------------------------------------------------------------
void ofApp::resetConfig() {
    cfgLayout = 0; cfgMode = 0; cfgScale = 1.0f; cfgGlobalRot = 0;
    cfgCenter = 4.0f; cfgBands = 40; cfgTwist = 0;
    cfgLineW = 2.6f; cfgFill = 1;
    cfgAlpha = 185; cfgR = 175; cfgG = 175; cfgB = 175;
    cfgRotX = 0; cfgRotY = 0; cfgRotZ = 0; cfgCamX = 0; cfgCamY = 0; cfgCamZ = 0; cfgRate = 0.90f; cfgSpread = 1.5f; cfgPunch = 1.0f; cfgLfoRate = 1.9575f;
    cfgCountX = 6; cfgCountY = 3; cfgTransX = 0; cfgTransY = 0;
    cfgTwistX = 0; cfgTwistY = 0; cfgPinch = 0; cfgShiftY = 0; cfgScaleX = 0.5f; cfgScaleY = 0.5f; cfgFalloff = 0; cfgGlow = 0.5f;
    for (int k = 0; k < 3; k++) { audioMod[k] = ModSlot(); lfoMod[k] = ModSlot(); }   // clear all modulation
    modPickKind = -1; activeModAmt = -1;
    rotationX = rotationY = rotationZ = 0;   // visual back to its original orientation
    camX = camY = camZ = 0;
    camManX = camManY = camManZ = 0;
    saveLayoutState(0); saveLayoutState(1);  // reset both layouts' independent settings
    lastLayout = -1;                         // force a clean re-sync next frame
}

//--------------------------------------------------------------
void ofApp::keyPressed(int key) {
    if (presetNaming) {                                   // typing a preset name captures all keys
        if (key == OF_KEY_ESC)                presetNaming = false;
        else if (key == OF_KEY_RETURN)      { if (!presetNameBuf.empty()) savePresetNamed(presetNameBuf); presetNaming = false; relayout(); }
        else if (key == OF_KEY_BACKSPACE)   { if (!presetNameBuf.empty()) presetNameBuf.pop_back(); }
        else if (key >= 32 && key < 127 && presetNameBuf.size() < 40) presetNameBuf += (char)key;
        return;
    }
    if (settingsOpen) {                                   // editor captures all keys
        if (key == OF_KEY_ESC) { revertFields(); settingsOpen = false; editingField = -1; }   // ESC = discard unsaved edits
        else if (editingField >= 0) {
            Field& f = fields[editingField];
            bool cmdHeld = ofGetKeyPressed(OF_KEY_LEFT_SUPER) || ofGetKeyPressed(OF_KEY_RIGHT_SUPER) ||
                           ofGetKeyPressed(OF_KEY_LEFT_CONTROL) || ofGetKeyPressed(OF_KEY_RIGHT_CONTROL);
            if ((cmdHeld && (key == 'v' || key == 'V')) || key == 22) {            // Cmd/Ctrl+V — paste (e.g. an address)
                std::string clip = gsClipboard();
                for (char c : clip) if ((unsigned char)c >= 32 && (unsigned char)c < 127) f.buf += c;   // printable only, single-line
                commitField(f);
            }
            else if (cmdHeld && (key == 'a' || key == 'A' || key == 1 || key == OF_KEY_BACKSPACE)) {    // Cmd/Ctrl+A or Cmd/Ctrl+Backspace — CLEAR the field (then paste to replace)
                f.buf.clear(); commitField(f);
            }
            else if (key == OF_KEY_BACKSPACE) { if (!f.buf.empty()) f.buf.pop_back(); commitField(f); }
            else if (key == OF_KEY_RETURN || key == OF_KEY_TAB) {                  // commit + jump to next (never land on a section header)
                commitField(f);
                do { editingField = (editingField + 1) % (int)fields.size(); } while (fields[editingField].header || fields[editingField].tab != settingsTab);
                fields[editingField].buf = fieldText(fields[editingField]);
            }
            else if (key >= 32 && key < 127) { f.buf += (char)key; commitField(f); }
        }
        else if (key == 's' || key == 'S' || key == OF_KEY_RETURN) { writeSession(); settingsOpen = false; editingField = -1; }  // save + close
        return;
    }
    if (key == OF_KEY_ESC) { if (recWarn) recWarn = false; else if (modPickKind >= 0) modPickKind = -1; else if (helpMode) helpMode = false; else showHelp = false; return; }
    if (key == 's' || key == 'S') { buildFields(); snapshotFields(); settingsOpen = true; editingField = -1; settingsTab = 0; return; }   // 'S' = settings — rebuild fields + snapshot the baseline (ESC reverts to it)
    if (key == 'c' || key == 'C') showPanel = !showPanel;                        // 'C' = controls (config panels)
    else if (key == 'h' || key == 'H') showHelp = !showHelp;                     // help overlay (all shortcuts)
    else if (key == 'i' || key == 'I') helpMode = !helpMode;                     // parameter help: hover any control for an explainer
    else if (key == 'u' || key == 'U') showHud = !showHud;                       // hide/show the broadcast HUD (all of it, incl. corner ticks)
    else if (key == 't' || key == 'T') showMeta = !showMeta;                     // show/hide the SIGNAL + metadata/telemetry text (default off — clean frame; client shows the metadata)
    else if (key == 'v' || key == 'V') showMeter = !showMeter;                   // show/hide the screen-only audio level meter along the bottom (never recorded)
    else if (key == 'f' || key == 'F') ofToggleFullscreen();
    else if (key == 'r' || key == 'R') { if (recording) stopRecording(); else startRecording(); }
    else if (key == 'b' || key == 'B') { if (broadcasting) stopBroadcast(); else startBroadcast(); }   // live icecast + snapshot push (independent of local recording)
    else if (key == 'a' || key == 'A') {                                         // broadcaster presence: toggle ON DECK <-> AWAY
        bcastAway = !bcastAway;
        errMsg = bcastAway ? "STATUS: AWAY" : "STATUS: ON DECK"; errFlash = t;
        ofLogNotice() << ofGetTimestampString("%H:%M:%S") << "  STATUS -> " << (bcastAway ? "AWAY" : "ON DECK");
        if (broadcasting) lastSnapshotT = -100;                                  // push a fresh snapshot NOW so the status reaches listeners immediately
    }
    else if (key == 'm' || key == 'M') {                                         // cycle modes (count comes from the Mode options — add a mode without touching this)
        for (auto& sl : sliders) if (sl.val == &cfgMode && !sl.opts.empty()) { cfgMode = fmodf(cfgMode + 1.0f, (float)sl.opts.size()); break; }
    }
    else if (key == OF_KEY_LEFT  && cfgLayout >= 1.5f) imageAdvance(-1);         // IMAGE type: previous image
    else if (key == OF_KEY_RIGHT && cfgLayout >= 1.5f) imageAdvance(+1);         // IMAGE type: next image
    else if (key == OF_KEY_TAB)        cfgLayout = (cfgLayout < 0.5f) ? 1 : 0;   // toggle RADIAL / GRID (not IMAGE)
    else if (key == 'x' || key == 'X') resetConfig();                            // reset to init settings
    else if (key == 'p' || key == 'P') ofSaveScreen(gsScratch("heliograph_frame.png"));   // screenshot (moved off 'S', now settings)
}

ofRectangle ofApp::displayRect() {
    float ww = ofGetWidth(), wh = ofGetHeight();
    float fa = (float)RW / RH, wa = ww / wh;
    float dw, dh;
    if (wa > fa) { dh = wh; dw = dh * fa; } else { dw = ww; dh = dw / fa; }
    return ofRectangle((ww - dw) * 0.5f, (wh - dh) * 0.5f, dw, dh);
}

void ofApp::mousePressed(int x, int y, int button) {
    ofRectangle r = displayRect();
    float fx = (x - r.x) * RW / r.width, fy = (y - r.y) * RH / r.height;   // window -> FBO coords
    if (settingsOpen) {
        for (int i = 0; i < 5; i++) if (settingsTabBox[i].inside(fx, fy)) { if (settingsTab != i) { settingsTab = i; editingField = -1; } return; }
        if (settingsTab == 1 && refreshDevicesBox.inside(fx, fy)) { refreshAudioDevices(); return; }
        if (settingsTab == 2 && registerBox.inside(fx, fy)) { if (editingField >= 0) { commitField(fields[editingField]); editingField = -1; } registerArtist(); return; }
        if (settingsTab == 4 && publishBox.inside(fx, fy)) { if (editingField >= 0) { commitField(fields[editingField]); editingField = -1; } publishCollection(); return; }
        for (size_t i = 0; i < fields.size(); i++) {
            if (fields[i].tab != settingsTab) continue;                // hidden tab — its .box is stale from when it was last drawn
            if (fields[i].header) continue;                            // section dividers aren't clickable
            // In-field PASTE / CLEAR buttons (take precedence over entering edit mode).
            if (fields[i].pasteBox.width > 0 && fields[i].pasteBox.inside(fx, fy)) {
                if (editingField >= 0 && editingField != (int)i) commitField(fields[editingField]);
                editingField = (int)i; fields[i].buf = fieldText(fields[i]);
                std::string clip = gsClipboard();
                for (char c : clip) if ((unsigned char)c >= 32 && (unsigned char)c < 127) fields[i].buf += c;
                commitField(fields[i]); return;
            }
            if (fields[i].clearBox.width > 0 && fields[i].clearBox.inside(fx, fy)) {
                if (editingField >= 0 && editingField != (int)i) commitField(fields[editingField]);
                editingField = (int)i; fields[i].buf.clear(); commitField(fields[i]); return;
            }
            if (fields[i].box.inside(fx, fy)) {
                if (fields[i].locked || (fields[i].gateBackup && !sWalletBackedUp)) { editingField = -1; return; }   // read-only (registered name; or donation field before Wallet Backed Up = YES)
                if (fields[i].folder) { pickRecDir(); editingField = -1; }   // folder field: open the native chooser (SAVE persists it)
                else if (!fields[i].choices.empty() && fields[i].ip) {   // selector: click cycles
                    int* ip = fields[i].ip;
                    *ip = (*ip + 1) % (int)fields[i].choices.size();
                    // ROUTING is a LIVE audition panel: device + channel changes take effect the instant
                    // you click, so the SIGNAL meter and the visualizer react immediately — you can hunt
                    // for the pair carrying signal without SAVE-ing after every click. (SAVE only persists
                    // the choice to session.json.) Before this, a click just changed the dropdown text
                    // while the stream stayed on whatever opened at launch — so picking the L-8 looked
                    // like "no signal" because you were still auditioning the old device.
                    if (ip == &sInputDeviceIdx) {
                        sInputDevice = (sInputDeviceIdx <= 0 || sInputDeviceIdx >= (int)audioDeviceChoices.size()) ? std::string() : audioDeviceChoices[sInputDeviceIdx];
                        sInputChannelPair = 0;   // new device — its channel layout differs; start at the first pair
                        stream.close(); setupAudio();
                        buildFields();           // rebuild the channel-pair choices for the new device's channel count
                        editingField = -1;
                        return;                  // fields[] was just rebuilt — stop iterating it
                    }
                    if (ip == &sInputChannelPair) {
                        applyChannelSelection();                       // instant — moves the read window, no reopen
                        sInputChannelPairAtOpen = sInputChannelPair;   // already applied live; don't re-open again on SAVE
                        editingField = -1;
                        return;
                    }
                    if (fields[i].op && *ip == (int)fields[i].choices.size() - 1) { editingField = (int)i; fields[i].buf = *fields[i].op; }   // landed on OTHER -> type a custom value
                    else editingField = -1;
                }
                else { editingField = (int)i; fields[i].buf = fieldText(fields[i]); }
                return;
            }
        }
        if (saveBox.inside(fx, fy)) { writeSession(); settingsOpen = false; editingField = -1; }   // SAVE saves AND closes the dialog
        return;
    }
    // IMAGE type: the screen-only carousel is clickable even when the panels are hidden.
    if (cfgLayout >= 1.5f) {
        if (imgBarToggle.width > 0 && imgBarToggle.inside(fx, fy)) { imgBarOpen = !imgBarOpen; return; }
        if (imgBarOpen)
            for (size_t i = 0; i < imgThumbBox.size(); i++) if (imgThumbBox[i].inside(fx, fy)) { imageGoto((int)i); return; }
    }
    if (!showPanel) return;   // panels are interactive mid-recording too (they're screen-only, never captured)
    // ---- tab bar ----
    for (int i = 0; i < 3; i++) if (tabBox[i].inside(fx, fy)) { if (rightTab != i) { rightTab = i; modPickKind = -1; presetNaming = false; relayout(); } return; }
    if (resetBox.inside(fx, fy)) { resetConfig(); return; }                       // RESET (GRAPH/GLOBAL)
    if (imgPanelAddBox.width > 0 && imgPanelAddBox.inside(fx, fy)) { pickImagesFolder(); return; }   // ADD IMAGES (GRAPH/GLOBAL, IMAGE type)
    if (imgPanelClearBox.width > 0 && imgPanelClearBox.inside(fx, fy)) { clearImages(); return; }     // CLEAR loaded images
    // ---- GRAPH sub-tabs (GLOBAL / PRESETS) + the preset list ----
    if (rightTab == 0) {
        for (int i = 0; i < 2; i++) if (graphSubBox[i].inside(fx, fy)) { if (graphSub != i) { graphSub = i; presetNaming = false; relayout(); } return; }
        if (graphSub == 0) {                                    // camera-angle pad + Z slider
            if (camPadBox.inside(fx, fy)) { camDrag = 1; mouseDragged(x, y, button); return; }
            if (camZBox.inside(fx, fy))   { camDrag = 2; mouseDragged(x, y, button); return; }
        }
        if (graphSub == 1) {
            if (presetNaming) { if (presetAddBox.inside(fx, fy)) { if (!presetNameBuf.empty()) savePresetNamed(presetNameBuf); presetNaming = false; relayout(); } return; }
            for (size_t i = 0; i < presetList.size(); i++) {
                if (i < presetDelBox.size() && presetDelBox[i].inside(fx, fy)) { deletePresetFile(presetList[i]); relayout(); return; }
                if (i < presetBox.size()    && presetBox[i].inside(fx, fy))    { loadPresetFile(presetList[i]); return; }
            }
            if (presetAddBox.inside(fx, fy)) { presetNaming = true; presetNameBuf = ""; relayout(); return; }
            // no return: clicks that miss the preset widgets fall through to the left-column sliders, so you can tweak config while in PRESETS
        }
    }
    // ---- MODULATION tab widgets ----
    if (rightTab == 2) {
        if (sourceBox[0].inside(fx, fy)) { modSource = 0; modPickKind = -1; return; }   // Source sub-tab: AUDIO
        if (sourceBox[1].inside(fx, fy)) { modSource = 1; modPickKind = -1; return; }   // Source sub-tab: LFO
        if (lfoRateBox.inside(fx, fy)) { activeModAmt = 6; mouseDragged(x, y, button); return; }   // drag LFO rate
        if (lfoOctPrevBox.inside(fx, fy) || lfoOctNextBox.inside(fx, fy)) {                         // step Schumann octaves
            int n = (int)ofClamp(roundf(log2f(7.83f / std::max(7.83f / 1024.0f, cfgLfoRate))), 0.f, 10.f);
            n = lfoOctPrevBox.inside(fx, fy) ? std::min(10, n + 1) : std::max(0, n - 1);   // '<' down an octave · '>' up
            cfgLfoRate = 7.83f / powf(2.0f, (float)n); return;
        }
        for (int gi = 0; gi < 6; gi++) {
            ModSlot& m = (gi < 3) ? audioMod[gi] : lfoMod[gi - 3];
            if (m.dest >= 0 && modClearBox[gi].inside(fx, fy)) {                  // delete a slot — FREEZE the dest at its current (last modulated) value, no jump back to base
                m.dest = -1; return;   // applyMods stops touching it once dest<0, so *s.val just stays where the modulation left it
            }
            if (gi >= 3 && m.dest >= 0 && modBipBox[gi - 3].inside(fx, fy)) { lfoMod[gi - 3].bipolar = !lfoMod[gi - 3].bipolar; return; }   // toggle LFO bipolar
            if (modDestBox[gi].inside(fx, fy)) { modPickKind = (gi < 3) ? 0 : 1; modPickSlot = gi % 3; return; }   // pick / re-pick
            if (m.dest >= 0 && modAmtBox[gi].inside(fx, fy)) { activeModAmt = gi; mouseDragged(x, y, button); return; }   // drag amount
        }
    }
    // ---- pick mode: bind a highlighted left-column control to the chosen slot ----
    if (modPickKind >= 0) {
        for (size_t i = 0; i < sliders.size(); i++) {
            Slider& s = sliders[i];
            if (s.tab != -1 || !sliderVisible(s)) continue;                     // only left-column params
            ofRectangle hit = s.track; hit.y -= 22 * S; hit.height += 30 * S;
            if (hit.inside(fx, fy)) {
                if (isModulated((int)i)) { errMsg = "THIS PARAMETER IS ALREADY BEING MODULATED"; errFlash = t; return; }   // can't double-modulate
                ModSlot& m = (modPickKind == 0) ? audioMod[modPickSlot] : lfoMod[modPickSlot];
                m.dest = (int)i; m.base = *s.val; m.amt = *s.val;                // start with target == current value (no jump); drag Target Value to open it up
                modPickKind = -1; return;
            }
        }
        return;   // consume the click while picking
    }
    // ---- normal slider / choice clicks (left params + the active right tab) ----
    for (size_t i = 0; i < sliders.size(); i++) {
        Slider& s = sliders[i];
        if (!sliderVisible(s) || !(s.tab == -1 || s.tab == rightTab)) continue;
        if (!s.opts.empty()) {                                       // option buttons: radio (set index) or toggle-mask (flip a bit)
            for (size_t b = 0; b < s.boxes.size(); b++)
                if (s.boxes[b].inside(fx, fy)) {
                    if (s.toggleMask) *s.val = (float)(((int)roundf(*s.val)) ^ (1 << (int)b));   // independent on/off bit
                    else              *s.val = (float)b;
                    return;
                }
            continue;
        }
        ofRectangle hit = s.track; hit.y -= 12 * S; hit.height += 24 * S;
        if (hit.inside(fx, fy)) {
            if (lastClickSlider == (int)i && (t - lastClickT) < 0.35f) {          // double-click -> restore this fader's default
                *s.val = s.intStep ? roundf(s.def) : s.def;
                for (int k = 0; k < 3; k++) { if (audioMod[k].dest == (int)i) audioMod[k].base = *s.val; if (lfoMod[k].dest == (int)i) lfoMod[k].base = *s.val; }   // keep any mod on it re-based to the new value
                lastClickSlider = -1; activeSlider = -1; return;
            }
            lastClickT = t; lastClickSlider = (int)i;
            activeSlider = (int)i; mouseDragged(x, y, button); return;
        }
    }
}
void ofApp::mouseDragged(int x, int y, int button) {
    ofRectangle r = displayRect();
    float fx = (x - r.x) * RW / r.width;
    if (camDrag == 1) {                                              // XY pad -> manual camera X (vert) / Y (horiz) angle
        float fy = (y - r.y) * RH / r.height;
        camManY = ofClamp(ofMap(fx, camPadBox.x, camPadBox.getMaxX(), -180.f, 180.f), -180.f, 180.f);
        camManX = ofClamp(ofMap(fy, camPadBox.y, camPadBox.getMaxY(), -180.f, 180.f), -180.f, 180.f);
        return;
    }
    if (camDrag == 2) { camManZ = ofClamp(ofMap(fx, camZBox.x, camZBox.x + camZBox.width, -180.f, 180.f), -180.f, 180.f); return; }   // Z roll
    if (activeModAmt >= 0) {                                          // dragging a modulation amount / LFO rate
        if (activeModAmt == 6) { float frac = ofClamp((fx - lfoRateBox.x) / lfoRateBox.width, 0.f, 1.f); cfgLfoRate = (7.83f / 1024.0f) * powf(2.0f, 10.0f * frac); }   // log: octaves evenly spaced
        else { int gi = activeModAmt; ModSlot& m = (gi < 3) ? audioMod[gi] : lfoMod[gi - 3];
               if (m.dest >= 0 && m.dest < (int)sliders.size()) { Slider& d = sliders[m.dest];
                   float v = ofMap(fx, modAmtBox[gi].x, modAmtBox[gi].x + modAmtBox[gi].width, d.lo, d.hi, true); m.amt = d.intStep ? roundf(v) : v; } }
        return;
    }
    if (activeSlider < 0) return;
    Slider& s = sliders[activeSlider];
    float v = ofMap(fx, s.track.x, s.track.x + s.track.width, s.lo, s.hi, true);
    *s.val = s.intStep ? roundf(v) : v;
    for (int k = 0; k < 3; k++) {                                    // dragging a modulated dest re-bases it
        if (audioMod[k].dest == activeSlider) audioMod[k].base = *s.val;
        if (lfoMod[k].dest == activeSlider) lfoMod[k].base = *s.val;
    }
}
void ofApp::mouseReleased(int x, int y, int button) { activeSlider = -1; activeModAmt = -1; camDrag = 0; }

void ofApp::windowResized(int w, int h) {}
void ofApp::exit() { if (recording) stopRecording(); if (broadcasting) stopBroadcast(); stream.close(); }

#include "ofApp.h"
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

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
    fboFinal.getTexture().setTextureMinMagFilter(GL_LINEAR, GL_LINEAR);
    fboRec.getTexture().setTextureMinMagFilter(GL_LINEAR, GL_LINEAR);

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
    const char* reg = "fonts/Saira-Variable.ttf";
    loadFont(fKick,   reg, (int)(18 * S)); fKick.setLetterSpacing(1.35f);   // (legacy kick font; wordmark now uses fBrand)
    loadFont(fTitle,  reg, (int)(26 * S));
    loadFont(fLabel,  reg, (int)(10 * S)); fLabel.setLetterSpacing(1.42f);
    loadFont(fValue,  reg, (int)(15 * S));
    loadFont(fNote,   reg, (int)(16 * S));
    loadFont(fUI,     reg, (int)(13 * S));
    loadFont(fBrand,  reg, (int)(15 * S)); fBrand.setLetterSpacing(1.5f);   // HelioGraph Mk1 wordmark (usual Saira font, roomy letterspacing)
    ofSetEscapeQuitsApp(false);   // ESC closes the settings/help overlay — it must NOT quit the app

    seedUserData();               // first launch: create ~/.heliograph/ (session.json + factory presets) from bin/data
    loadSession();
    if (!ofFile::doesFileExist(gsSessionPath())) writeSession();   // belt-and-suspenders: if no factory file shipped, persist defaults
    buildFields();
    buildSliders();
    saveLayoutState(0); saveLayoutState(1);   // both layouts start from the same defaults, then diverge independently
    scanPresets();
    setupAudio();
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
            sMovement = j.value("movement", 0);
            sMovementText = j.value("movementText", sMovementText);
            sArtifact  = j.value("artifact", sArtifact);
            sShorthand = j.value("shorthand", sShorthand);
            sRecDir    = j.value("recDir", sRecDir);
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
    addChoice("Type",   &cfgLayout, {"RADIAL", "GRID"}, 0);                   // 0 Radial · 1 Grid (or press TAB)
    addChoice("Mode",   &cfgMode,   {"ORBIT", "VEHICLE", "PLATFORM", "HELIX-S", "HELIX-D"}, 0); sliders.back().icons = true;   // ○ △ ▢ + static/dynamic helix (or 'm')
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
    if (s.show == 0) return true;
    if (s.show == 1) return cfgLayout < 0.5f;   // radial-only
    return cfgLayout >= 0.5f;                    // grid-only
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
    lastLayout = (cfgLayout >= 0.5f) ? 1 : 0;
}

//--------------------------------------------------------------
void ofApp::setupAudio() {
    ofSoundStreamSettings s;
    auto devices = stream.getDeviceList();
    int chosen = -1;
    // prefer a virtual loopback device by name (cross-platform): BlackHole (mac),
    // VB-Audio Cable / VoiceMeeter (Windows), PulseAudio monitor / JACK / loopback (Linux)
    const char* keys[] = { "blackhole", "cable", "vb-audio", "voicemeeter", "loopback", "monitor", "jack" };
    for (size_t i = 0; i < devices.size() && chosen < 0; i++) {
        if (devices[i].inputChannels <= 0) continue;
        std::string nm = ofToLower(devices[i].name);
        for (const char* k : keys) if (nm.find(k) != std::string::npos) { chosen = (int)i; break; }
    }
    if (chosen < 0)                                    // fallback: first available input (route your loopback as default)
        for (size_t i = 0; i < devices.size(); i++) if (devices[i].inputChannels > 0) { chosen = (int)i; break; }
    if (chosen < 0) {
        deviceName = "(no signal)";
        ofLogError() << "No audio input found — set up a loopback device (see README: Audio routing).";
        return;
    }
    s.setInDevice(devices[chosen]);
    deviceName = devices[chosen].name;
    recChannels = std::min(2, std::max(1, (int)devices[chosen].inputChannels));
    s.setInListener(this);
    s.sampleRate = sampleRate; s.numInputChannels = recChannels; s.numOutputChannels = 0; s.bufferSize = bufferSize;
    stream.setup(s);
    int sr = (int)stream.getSampleRate();              // adopt the device's actual rate (e.g. 48000)
    if (sr > 0) sampleRate = sr;
    ofLogNotice() << "input: " << deviceName << " @ " << sampleRate << " Hz";
}

//--------------------------------------------------------------
void ofApp::audioIn(ofSoundBuffer & input) {
    const size_t frames = input.getNumFrames(), ch = input.getNumChannels();
    if (ch == 0) return;
    std::lock_guard<std::mutex> lock(mtx);
    float sum = 0;
    for (size_t i = 0; i < frames; i++) {
        float m = 0;
        for (size_t c = 0; c < ch; c++) {
            float smp = input.getSample(i, c);
            m += smp;
            if (recording) recAudio.push_back((short)(ofClamp(smp, -1.f, 1.f) * 32767));
        }
        m /= (float)ch;
        ringBuf[writePos] = m; writePos = (writePos + 1) % N;
        sum += m * m;
    }
    rms = sqrtf(sum / (float)frames);
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
    {   // toggled RADIAL/GRID -> swap each layout's independent settings, then re-pack the panel
        int cur = (cfgLayout >= 0.5f) ? 1 : 0;
        if (cur != lastLayout) {
            if (lastLayout >= 0) saveLayoutState(lastLayout);
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

void ofApp::drawScene() {
    drawSpace();
    drawStars();
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

    // SIGNAL — NONE (no audio · dim, static) overrules · AUDITION (audio, grey, static) · LIVE (audio+recording, white, pulsing) — top-left
    {
        float gap = 24 * S;
        bool audio = level > 0.008f;                          // is any audio actually coming in?
        bool live  = audio && recording;                      // NONE overrules: no audio => NONE even while recording
        const char* st = !audio ? "NONE" : (recording ? "LIVE" : "AUDITION");
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
        std::string mv = (sMovement == 0) ? "FREEFORM" : (sMovement == 1) ? "COMPOSED" : (sMovementText.empty() ? "OTHER" : ofToUpper(sMovementText));
        std::string meta = ofToUpper(sArtist) + "   ·   " + sDate + "   ·   " + mv;
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
    camPadBox = camZBox = ofRectangle(-99999, -99999, 0, 0);
    presetBox.clear(); presetDelBox.clear();
    if (rightTab == 0) {                                     // GRAPH: GLOBAL / PRESETS sub-tabs
        float subG = 8 * S, subW = (rw - subG) * 0.5f, subY = fm + 162 * S;   // sub-tab sits just under the GRAPH/AUDIO/MOD tab bar
        graphSubBox[0] = ofRectangle(rx, subY, subW, 30 * S);
        graphSubBox[1] = ofRectangle(rx + subW + subG, subY, subW, 30 * S);
        if (graphSub == 0) {                                 // GLOBAL: choices + camera-angle pad + RESET
            for (auto& s : sliders) if (s.tab == 0 && sliderVisible(s)) contentBottom = std::max(contentBottom, s.track.y + s.track.height);
            float padSz = 116 * S, padY = contentBottom + 36 * S;
            camPadBox = ofRectangle(rx, padY, padSz, padSz);                                              // XY pad: camera X (vert) / Y (horiz) angle
            camZBox   = ofRectangle(rx + padSz + 20 * S, padY + padSz - 12 * S, rw - padSz - 20 * S, 13 * S);   // Z roll slider beside the pad
            contentBottom = padY + padSz;
            resetBox = ofRectangle(rx, contentBottom + 28 * S, rw, 42 * S); contentBottom = resetBox.getMaxY();
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
    if (f.op && !f.choices.empty() && f.ip && *f.ip == (int)f.choices.size() - 1) *f.op = f.buf;   // editing the OTHER free-text
    else if (f.sp) *f.sp = f.buf;
    else if (f.ip) *f.ip = ofToInt(f.buf);
    else if (f.fp) *f.fp = ofToFloat(f.buf);
}
void ofApp::buildFields() {
    fields.clear();
    auto addS = [&](std::string l, std::string* p){ Field f; f.label = l; f.sp = p; fields.push_back(f); };
    auto addI = [&](std::string l, int* p){ Field f; f.label = l; f.ip = p; fields.push_back(f); };
    auto addC = [&](std::string l, int* p, std::vector<std::string> c, std::string* op){ Field f; f.label = l; f.ip = p; f.choices = c; f.op = op; fields.push_back(f); };
    auto addF = [&](std::string l, std::string* p){ Field f; f.label = l; f.sp = p; f.folder = true; fields.push_back(f); };
    addS("Channel",          &sChannel);       // the big title word, top-right (e.g. TRANSMISSION)
    addI("Episode",          &sTransmission);  // the number after the channel
    addS("Artifact Title",   &sArtifact);      // filename brand (e.g. HelioGraph)
    addS("Series Shorthand", &sShorthand);     // filename code (e.g. TxN)
    addS("Artist",           &sArtist);
    addS("Title",            &sTitle);         // shows bottom-left while recording
    addS("Note",             &sNote);          // shows bottom-left while recording
    addC("Movement", &sMovement, {"FREEFORM", "COMPOSED", "OTHER"}, &sMovementText);   // click to cycle; OTHER = free text
    addF("Recordings", &sRecDir);    // folder where recordings are saved — click opens a folder picker
    // Waypoint / Heading / Distance are not editable — they're auto-generated telemetry.
}
void ofApp::writeSession() {
    ofJson j;
    { std::ifstream in(gsSessionPath());   // keep any _help block
      if (in) { try { in >> j; } catch (...) {} } }
    j["transmission"] = sTransmission;
    j["title"]   = sTitle;
    j["date"]    = sDate;
    j["artist"]  = sArtist;
    j["channel"] = sChannel;
    j["note"]    = sNote;
    j["movement"] = sMovement;
    j["movementText"] = sMovementText;
    j["artifact"]  = sArtifact;
    j["shorthand"] = sShorthand;
    j["recDir"]    = sRecDir;
    j["coordinates"]["waypoint"] = sWaypoint;
    j["coordinates"]["heading"]  = sHeading;
    j["coordinates"]["distance"] = sDist;
    std::ofstream o(gsSessionPath());
    if (o) { o << j.dump(2); o.close(); saveFlash = t; ofLogNotice() << "session.json saved"; }
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
    lastLayout = (int)cfgLayout;            // preset carries its own layout type; lock it so the layout-swap doesn't clobber the loaded values
    saveLayoutState((int)cfgLayout);        // store the preset into the active layout's slot
    relayout();
    saveFlash = t;
}
void ofApp::deletePresetFile(const std::string& name) {
    ofFile::removeFile(gsPresetsDir() + name + ".json");
    scanPresets();
}
void ofApp::drawSettings() {
    ofSetColor(0, 0, 0, 185); ofDrawRectangle(0, 0, RW, RH);          // dim everything
    float pw = 1200 * S, ph = ((float)fields.size() * 64 + 230) * S;   // +30 for the Title/Note info line's own row
    float px = (RW - pw) * 0.5f, py = (RH - ph) * 0.5f;
    ofSetColor(12, 14, 16, 248); ofDrawRectangle(px, py, pw, ph);
    ofSetColor(210, 216, 212); fKick.drawString("SESSION SETTINGS", px + 40 * S, py + 56 * S);
    float fy = py + 124 * S;
    for (size_t i = 0; i < fields.size(); i++) {
        Field& f = fields[i];
        f.box = ofRectangle(px + 320 * S, fy - 30 * S, pw - 360 * S, 44 * S);
        ofSetColor(150, 160, 156); fLabel.drawString(ofToUpper(f.label), px + 40 * S, fy - 2 * S);
        bool foc = ((int)i == editingField);
        ofSetColor(foc ? ofColor(44, 48, 52) : ofColor(26, 30, 32)); ofDrawRectangle(f.box);   // grey focus highlight (no green)
        std::string txt = foc ? f.buf : fieldText(f);
        ofSetColor(220, 226, 222); fValue.drawString(txt, f.box.x + 14 * S, fy);
        if (!f.choices.empty()) { ofSetColor(120, 126, 124); std::string h = "click to toggle"; fUI.drawString(h, f.box.x + f.box.width - 16 * S - fUI.stringWidth(h), fy - 2 * S); }
        if (f.folder)           { ofSetColor(120, 126, 124); std::string h = "click to choose folder"; fUI.drawString(h, f.box.x + f.box.width - 16 * S - fUI.stringWidth(h), fy - 2 * S); }
        if (foc && fmodf(t, 1.0f) < 0.55f) {                          // blinking cursor
            float cx = f.box.x + 16 * S + fValue.stringWidth(f.buf);
            ofSetColor(cNeon); ofDrawRectangle(cx, fy - 20 * S, 2 * S, 26 * S);
        }
        if (f.label == "Note") { ofSetColor(112, 118, 116); std::string in = "Title & Note appear in the bottom-left of your live recording"; fUI.drawString(in, f.box.getMaxX() - fUI.stringWidth(in), fy + 32 * S); fy += 30 * S; }   // info line (right-aligned to the field edge)
        fy += 64 * S;
    }
    bool saved = (t - saveFlash) < 1.6f;
    saveBox = ofRectangle(px + pw - 240 * S, py + ph - 72 * S, 200 * S, 46 * S);
    ofSetColor(saved ? ofColor(96, 102, 100) : ofColor(54, 60, 58)); ofDrawRectangle(saveBox);   // greyscale (no green)
    ofSetColor(cNeon); { std::string s = saved ? "SAVED" : "SAVE"; fValue.drawString(s, saveBox.x + saveBox.width * 0.5f - fValue.stringWidth(s) * 0.5f, saveBox.y + 31 * S); }
    ofSetColor(150, 156, 154); fUI.drawString("RECORDING:   " + recBaseName() + ".mp4", px + 40 * S, py + ph - 104 * S);
    ofSetColor(120, 126, 124); fUI.drawString("click a field to edit   ·   [tab]/[enter] next   ·   [s]ave / [esc] close", px + 40 * S, py + ph - 40 * S);
}

//--------------------------------------------------------------
//  SHORTCUTS overlay ('h')
//--------------------------------------------------------------
void ofApp::drawHelp() {
    ofSetColor(0, 0, 0, 210); ofDrawRectangle(0, 0, RW, RH);
    static const std::pair<std::string, std::string> keys[] = {
        {"G", "show / hide control panels"},
        {"H", "show / hide this help"},
        {"I", "parameter help — hover any control"},
        {"E", "edit session details"},
        {"R", "start / stop recording"},
        {"M", "cycle mode (orbit / vehicle / platform)"},
        {"TAB", "toggle layout (radial / grid)"},
        {"X", "reset all settings to defaults"},
        {"U", "show / hide the broadcast HUD"},
        {"F", "fullscreen"},
        {"S", "save a screenshot to /tmp"},
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
    std::string raw = sArtifact + "-" + sShorthand + pad(sTransmission, 3) + "-" + sArtist + "-" + sTitle;   // e.g. HelioGraph-TxN001-amo_eba-Deuce
    std::string safe; for (char c : raw) {                                                    // lowercase + sanitise
        if (isalnum((unsigned char)c))      safe += (char)tolower((unsigned char)c);
        else if (c == '_' || c == '-')      safe += c;
        else                                safe += '_';
    }
    return safe;   // -> heliograph-txn001-amo_eba
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
    if (vidPipe) { GS_PCLOSE(vidPipe); vidPipe = nullptr; }
    std::string b = recFinalPath.substr(0, recFinalPath.size() - 4);
    std::string vidTmp = b + ".video.mp4", log = gsScratch("gs_ffmpeg.log");
    std::string wavTmp = gsScratch("heliograph_rec_audio.wav");   // scratch only — muxed in, then deleted (no separate WAV kept)
    { std::lock_guard<std::mutex> lock(mtx); writeWav(wavTmp, recAudio, recChannels, sampleRate); }
    // mux video + audio into ONE MP4 (AAC 320k), then drop both temporaries
    std::string mux = gsFFmpeg() + " -y -i \"" + vidTmp + "\" -i \"" + wavTmp +
        "\" -c:v copy -c:a aac -b:a 320k -shortest \"" + recFinalPath + "\" 2>>\"" + log + "\"";
    system(mux.c_str());
    ofFile::removeFile(vidTmp);   // portable cleanup (no shell 'rm')
    ofFile::removeFile(wavTmp);
    ofLogNotice() << "REC saved -> " << recFinalPath;
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
        if (key == OF_KEY_ESC) { settingsOpen = false; editingField = -1; }
        else if (editingField >= 0) {
            Field& f = fields[editingField];
            if (key == OF_KEY_BACKSPACE) { if (!f.buf.empty()) f.buf.pop_back(); commitField(f); }
            else if (key == OF_KEY_RETURN || key == OF_KEY_TAB) {                  // commit + jump to next
                commitField(f);
                editingField = (editingField + 1) % (int)fields.size();
                fields[editingField].buf = fieldText(fields[editingField]);
            }
            else if (key >= 32 && key < 127) { f.buf += (char)key; commitField(f); }
        }
        else if (key == 's' || key == 'S' || key == OF_KEY_RETURN) { writeSession(); settingsOpen = false; editingField = -1; }  // save + close
        return;
    }
    if (key == OF_KEY_ESC) { if (recWarn) recWarn = false; else if (modPickKind >= 0) modPickKind = -1; else if (helpMode) helpMode = false; else showHelp = false; return; }
    if (key == 'e' || key == 'E') { settingsOpen = true; editingField = -1; return; }   // open editor
    if (key == 'g' || key == 'G') showPanel = !showPanel;
    else if (key == 'h' || key == 'H') showHelp = !showHelp;                     // help overlay (all shortcuts)
    else if (key == 'i' || key == 'I') helpMode = !helpMode;                     // parameter help: hover any control for an explainer
    else if (key == 'u' || key == 'U') showHud = !showHud;                       // hide/show the broadcast HUD
    else if (key == 'f' || key == 'F') ofToggleFullscreen();
    else if (key == 'r' || key == 'R') { if (recording) stopRecording(); else startRecording(); }
    else if (key == 'm' || key == 'M') {                                         // cycle modes (count comes from the Mode options — add a mode without touching this)
        for (auto& sl : sliders) if (sl.val == &cfgMode && !sl.opts.empty()) { cfgMode = fmodf(cfgMode + 1.0f, (float)sl.opts.size()); break; }
    }
    else if (key == OF_KEY_TAB)        cfgLayout = (cfgLayout < 0.5f) ? 1 : 0;   // toggle RADIAL / GRID
    else if (key == 'x' || key == 'X') resetConfig();                            // reset to init settings
    else if (key == 's' || key == 'S') ofSaveScreen(gsScratch("heliograph_frame.png"));
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
        for (size_t i = 0; i < fields.size(); i++)
            if (fields[i].box.inside(fx, fy)) {
                if (fields[i].folder) { pickRecDir(); editingField = -1; }   // folder field: open the native chooser (SAVE persists it)
                else if (!fields[i].choices.empty() && fields[i].ip) {   // selector: click cycles
                    *fields[i].ip = (*fields[i].ip + 1) % (int)fields[i].choices.size();
                    if (fields[i].op && *fields[i].ip == (int)fields[i].choices.size() - 1) { editingField = (int)i; fields[i].buf = *fields[i].op; }   // landed on OTHER -> type a custom value
                    else editingField = -1;
                }
                else { editingField = (int)i; fields[i].buf = fieldText(fields[i]); }
                return;
            }
        if (saveBox.inside(fx, fy)) { writeSession(); settingsOpen = false; editingField = -1; }   // SAVE saves AND closes the dialog
        return;
    }
    if (!showPanel) return;   // panels are interactive mid-recording too (they're screen-only, never captured)
    // ---- tab bar ----
    for (int i = 0; i < 3; i++) if (tabBox[i].inside(fx, fy)) { if (rightTab != i) { rightTab = i; modPickKind = -1; presetNaming = false; relayout(); } return; }
    if (resetBox.inside(fx, fy)) { resetConfig(); return; }                       // RESET (GRAPH/GLOBAL)
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
void ofApp::exit() { if (recording) stopRecording(); stream.close(); }

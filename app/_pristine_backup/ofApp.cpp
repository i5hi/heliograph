#include "ofApp.h"
#include <fstream>
#include <cstdio>
#include <cstdlib>

#define FFMPEG "/opt/homebrew/bin/ffmpeg"

// =============================================================================
//  GREENSHIFT — where to tweak things
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

    auto loadFont = [&](ofTrueTypeFont& f, int sz) {
        ofTrueTypeFontSettings st("fonts/Saira-Variable.ttf", sz);
        st.antialiased = true;
        st.addRanges({ ofUnicode::Latin, ofUnicode::Latin1Supplement, ofUnicode::GeneralPunctuation });
        f.load(st);
    };
    loadFont(fKick,  (int)(18 * S)); fKick.setLetterSpacing(1.35f);   // roomier GREENSHIFT wordmark
    loadFont(fTitle, (int)(26 * S));
    loadFont(fLabel, (int)(10 * S)); fLabel.setLetterSpacing(1.42f);
    loadFont(fValue, (int)(15 * S));
    loadFont(fNote,  (int)(16 * S));
    loadFont(fUI,    (int)(13 * S));

    loadSession();
    buildSliders();
    setupAudio();
}

//--------------------------------------------------------------
void ofApp::loadSession() {
    std::ifstream in("/Users/ishi/Music/Greenshift/sessions/session.json");
    if (in) {
        try {
            ofJson j; in >> j;
            sTransmission = j.value("transmission", 1);
            sTitle   = j.value("title", sTitle);
            sArtist  = j.value("artist", sArtist);
            sChannel = j.value("channel", sChannel);
            sNote    = j.value("note", sNote);
            std::string d = j.value("date", std::string("today"));
            if (!d.empty() && d != "today") sDate = d; else sDate.clear();
            if (j.contains("coordinates")) {
                auto c = j["coordinates"];
                sWaypoint = c.value("waypoint", sWaypoint);
                sHeading  = c.value("heading", 0.0f);
                sDist     = c.value("distance", 0.0f);
            }
        } catch (...) { ofLogError() << "session.json parse failed"; }
    }
    if (sDate.empty()) {
        auto pad = [](int v){ return (v < 10 ? "0" : "") + ofToString(v); };
        sDate = ofToString(ofGetYear()) + "." + pad(ofGetMonth()) + "." + pad(ofGetDay());
    }
}

//--------------------------------------------------------------
void ofApp::buildSliders() {
    sliders.clear();
    auto add = [&](std::string n, float* v, float lo, float hi, int side, bool isInt, int prec, bool reactive) {
        Slider s; s.name = n; s.val = v; s.lo = lo; s.hi = hi; s.side = side;
        s.intStep = isInt; s.prec = prec; s.reactive = reactive; sliders.push_back(s);
    };
    // LEFT — FOREST: static (shape) first, then audio-reactive
    add("Tree Len", &cfgTreeLen,     40, 240, 0, false, 0, false);
    add("Angle",    &cfgAngle,       0, 1.2f, 0, false, 2, false);
    add("Opacity",  &cfgTreeOpacity, 0, 1.0f, 0, false, 2, false);
    add("Sway",     &cfgSway,        0, 3.0f, 0, false, 2, false);   // ambient wind, not audio-driven
    // RIGHT — VISUALIZER: static (shape) first, then audio-reactive
    add("Centre", &cfgCenter, 1, 30,    1, false, 1, false);
    add("Bands",  &cfgBands,  4, BANDS, 1, true,  0, false);
    add("Red",    &cfgR,      0, 255,   1, true,  0, false);
    add("Green",  &cfgG,      0, 255,   1, true,  0, false);
    add("Blue",   &cfgB,      0, 255,   1, true,  0, false);
    add("Rot X",  &cfgRotX,  -0.12f, 0.12f, 1, false, 3, false);
    add("Rot Y",  &cfgRotY,  -0.12f, 0.12f, 1, false, 3, false);
    add("Rot Z",  &cfgRotZ,  -0.12f, 0.12f, 1, false, 3, false);
    add("Rate",   &cfgRate,   0, 0.99f, 1, false, 2, true);
    add("Spread", &cfgSpread, 0, 12,    1, false, 1, true);

    float w = 220 * S, h = 13 * S, gap = 46 * S, subGap = 62 * S, y0 = fm + 140 * S;   // below the top HUD bar
    float yL = y0, yR = y0; bool prevL = false, prevR = false;
    for (auto& s : sliders) {
        float& y    = (s.side == 0) ? yL : yR;
        bool&  prev = (s.side == 0) ? prevL : prevR;
        if (s.reactive && !prev) y += subGap;             // room for the AUDIO-REACTIVE divider
        s.track = ofRectangle((s.side == 0) ? fm + 20 * S : RW - fm - 20 * S - w, y, w, h);
        y += gap;
        prev = s.reactive;
    }
}

//--------------------------------------------------------------
void ofApp::setupAudio() {
    ofSoundStreamSettings s;
    auto devices = stream.getDeviceList();
    int chosen = -1;
    for (size_t i = 0; i < devices.size(); i++)
        if (devices[i].inputChannels > 0 &&
            ofToLower(devices[i].name).find("blackhole") != std::string::npos) { chosen = (int)i; break; }
    if (chosen < 0) {                                  // BlackHole only — never fall back to the mic
        deviceName = "(no signal)";
        ofLogError() << "BlackHole input not found — run: sudo killall coreaudiod";
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
        v = std::min(v * fftGain, 120.0f);
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
    high  = highE.process(energy(2000, 9000, 34));
    float bInst = energy(30, 180, 22);
    if (bInst > prevBass * 1.35f && bInst > 0.16f) beatEnv = 1.0f;
    prevBass = ofLerp(prevBass, bInst, 0.10f);
    beatEnv = std::max(0.0f, beatEnv - dt / 0.45f);

    rotationX += cfgRotX; rotationY += cfgRotY; rotationZ += cfgRotZ;

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
    ofMesh g; g.setMode(OF_PRIMITIVE_TRIANGLE_FAN);
    g.addVertex(glm::vec3(RW * 0.5f, RH * 0.50f, 0)); g.addColor(ofColor(12, 12, 12));  // near-black, neutral
    float rad = sqrtf((float)RW * RW + (float)RH * RH) * 0.7f;
    int segs = 48;
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

// audioVisualizer: concentric FFT circles, RGB + audio colour-mod, cumulative 3D rotation
void ofApp::drawPortal() {
    ofPushMatrix();
    ofTranslate(RW * 0.5f, RH * 0.50f, 0);     // centred on screen
    ofNoFill();
    int nb = (int)ofClamp(cfgBands, 1.f, (float)BANDS);
    for (int i = 0; i < nb; i++) {
        float v = bands[i];
        float mod = fmodf(v * 1200.0f, 255.0f);
        // opacity fade inner->outer along an exponential curve: fast drop at the start,
        // slow toward the edge, last band ~invisible. (f normalised to the drawn count.)
        float f = (float)i / std::max(1, nb - 1);                 // 0 = inner, 1 = outermost
        float k = 3.2f;                                           // TWEAK: higher = faster initial fade
        float o = (expf(-k * f) - expf(-k)) / (1.0f - expf(-k));  // 1 -> 0
        ofSetColor((int)ofClamp(cfgR + mod, 0.f, 255.f),
                   (int)ofClamp(cfgG + mod, 0.f, 255.f),
                   (int)ofClamp(cfgB + mod, 0.f, 255.f), (int)(185 * o));
        // band thickness: inner thick -> outer thin. TWEAK the two px numbers to taste.
        ofSetLineWidth(ofLerp(2.6f, 0.5f, f) * S);
        float rad = (cfgCenter * i + v * cfgSpread + beatEnv * 8.0f) * S;
        ofDrawCircle(0, 0, rad);
        ofRotateXDeg(rotationX); ofRotateYDeg(rotationY); ofRotateZDeg(rotationZ);
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
    drawForest();
}

//--------------------------------------------------------------
void ofApp::draw() {
    fboFinal.begin();
    ofClear(0, 0, 0, 255);                        // black outside the frame
    glEnable(GL_SCISSOR_TEST);                    // keep all scene content inside the frame
    glScissor((int)fm, (int)fm, (int)(RW - 2 * fm), (int)(RH - 2 * fm));
    drawScene();
    glDisable(GL_SCISSOR_TEST);
    drawHud();
    if (showPanel && !recording) drawPanels();   // 'g' truly hides them
    fboFinal.end();

    ofRectangle r = displayRect();                       // letterboxed: preserves 16:9 at any window/fullscreen size
    ofSetColor(255);
    ofEnableBlendMode(OF_BLENDMODE_DISABLED);
    fboFinal.draw(r.x, r.y, r.width, r.height);
    ofEnableBlendMode(OF_BLENDMODE_ALPHA);

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

    if (recording && vidPipe) {
        recAccum += ofGetLastFrameTime();
        if (recAccum >= 1.0 / 30.0) { recAccum -= 1.0 / 30.0; writeRecordFrame(); }
    }
    if (autoShot && t > 8.0f) { ofSaveScreen("/tmp/greenshift_frame.png"); autoShot = false; }
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
    std::string sig = (deviceName.find("BlackHole") != std::string::npos) ? "BLACKHOLE" : "LIVE";

    // ===== top bar: SIGNAL (left) · GREENSHIFT (centre) · TRANSMISSION (right, large) =====
    float yTop = m + fTitle.getAscenderHeight() + 8 * S;
    float lx = m + ins;

    // GREENSHIFT wordmark (centre): GREEN white + SHIFT bold green
    {
        std::string a = "GREEN", b = "SHIFT";
        float wA = fKick.stringWidth(a), wB = fKick.stringWidth(b);
        float x0 = W * 0.5f - (wA + wB) * 0.5f;
        ofSetColor(236, 240, 237); fKick.drawString(a, x0, yTop);
        ofSetColor(cNeon);
        float o = 0.8f * S;
        for (float dx = -o; dx <= o; dx += o) for (float dy = -o; dy <= o; dy += o) fKick.drawString(b, x0 + wA + dx, yTop + dy);
    }
    // SIGNAL (bold white) ● LIVE (jade, pulsing dot) — top-left
    {
        float o = 0.7f * S, gap = 24 * S;
        ofSetColor(238, 242, 239);
        for (float dx = -o; dx <= o; dx += o) for (float dy = -o; dy <= o; dy += o) fValue.drawString("SIGNAL", lx + dx, yTop + dy);
        float wS = fValue.stringWidth("SIGNAL");
        float pulse = 0.30f + 0.70f * (0.5f + 0.5f * sinf(t * 2.6f));
        ofSetColor(cNeon, (int)(255 * pulse));
        ofDrawCircle(lx + wS + gap * 0.5f, yTop - fValue.getAscenderHeight() * 0.32f, 6 * S);
        ofSetColor(cNeon); fValue.drawString(sig, lx + wS + gap, yTop);
    }
    // TRANSMISSION Nº 001 — top-right, large (title font)
    { std::string s = "TRANSMISSION Nº " + pad(sTransmission, 3); ofSetColor(val); fTitle.drawString(s, W - m - ins - fTitle.stringWidth(s), yTop); }

    // ===== bottom: shared 3-row log grid =====
    float lh = fTitle.getLineHeight();
    float yB = H - m - 24 * S, yM = yB - lh, yT = yM - lh * 0.80f;
    float hd = fmodf(sHeading + t * 1.6f, 360.0f);
    long  ds = recording ? (long)(t - recStart) : (long)(sDist + t * 7.3f);   // recording -> counts seconds

    ofSetColor(lab);  fLabel.drawString(ofToUpper(sArtist) + "   ·   " + sDate + "   ·   " + ofToUpper(sChannel), lx, yT);
    ofSetColor(val);  fTitle.drawString(sTitle, lx, yM);
    ofSetColor(note); fNote.drawString(sNote, lx, yB);

    { std::string s = ofToUpper(std::string("Coordinates")); ofSetColor(lab); fLabel.drawString(s, rx(fLabel, s), yT); }
    { std::string s = "WPT " + sWaypoint + "    HDG " + ofToString(hd, 1) + "°"; ofSetColor(val); fValue.drawString(s, rx(fValue, s), yM); }
    { std::string s = "DIST " + ofToString(ds) + " LY"; ofSetColor(val); fValue.drawString(s, rx(fValue, s), yB); }

    ofSetLineWidth(1.0f);
}

//--------------------------------------------------------------
void ofApp::drawPanels() {
    auto box = [&](int side, const std::string& title) {
        float minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9; bool any = false;
        for (auto& s : sliders) if (s.side == side) {
            any = true;
            minx = std::min(minx, s.track.x); miny = std::min(miny, s.track.y);
            maxx = std::max(maxx, s.track.x + s.track.width); maxy = std::max(maxy, s.track.y + s.track.height);
        }
        if (!any) return;
        float px = 16 * S, pyT = 64 * S, pyB = 16 * S;
        ofSetColor(0, 0, 0, 120);                                // translucent surface
        ofDrawRectangle(minx - px, miny - pyT, (maxx - minx) + px * 2, (maxy - miny) + pyT + pyB);
        ofSetColor(190, 196, 193); fUI.drawString(title, minx, miny - 44 * S);   // grey header, clear gap above row 1
        // AUDIO-REACTIVE divider above the first reactive slider of this side
        for (auto& s : sliders) if (s.side == side && s.reactive) {
            ofSetColor(70, 84, 78); ofDrawRectangle(s.track.x, s.track.y - 29 * S, maxx - minx, std::max(1.0f, 1 * S));
            ofSetColor(132, 150, 140); fUI.drawString("AUDIO-REACTIVE", s.track.x, s.track.y - 40 * S);
            break;
        }
        for (auto& s : sliders) if (s.side == side) {
            ofSetColor(158, 164, 162); fUI.drawString(s.name, s.track.x, s.track.y - 9 * S);
            std::string vs = ofToString(*s.val, s.prec);
            ofSetColor(214, 218, 216); fUI.drawString(vs, s.track.x + s.track.width - fUI.stringWidth(vs), s.track.y - 9 * S);
            ofSetColor(48, 50, 52); ofDrawRectangle(s.track);    // grey track
            // for bipolar (e.g. Rot) fill from centre; else from left
            if (s.lo < 0) {
                float zero = ofMap(0, s.lo, s.hi, 0, s.track.width, true);
                float fr = ofMap(*s.val, s.lo, s.hi, 0, s.track.width, true);
                ofSetColor(150, 156, 154, 235);
                ofDrawRectangle(s.track.x + std::min(zero, fr), s.track.y, fabsf(fr - zero), s.track.height);
            } else {
                float fr = ofMap(*s.val, s.lo, s.hi, 0, s.track.width, true);
                ofSetColor(150, 156, 154, 235); ofDrawRectangle(s.track.x, s.track.y, fr, s.track.height);
            }
        }
    };
    box(0, "FOREST");
    box(1, "VISUALIZER");

    // shortcuts list, below the FOREST panel
    float ly = 0; for (auto& s : sliders) if (s.side == 0) ly = std::max(ly, s.track.y + s.track.height);
    float sx = sliders.empty() ? fm + 20 * S : sliders[0].track.x;
    float sy = ly + 54 * S;
    ofSetColor(150, 156, 154); fUI.drawString("SHORTCUTS", sx, sy);
    const char* keys[] = { "G   show / hide panels", "R   start / stop recording",
                           "L   reload session.json", "F   fullscreen",
                           "H   debug line", "S   save screenshot" };
    ofSetColor(120, 126, 124);
    for (int i = 0; i < 6; i++) fUI.drawString(keys[i], sx, sy + (i + 1) * 27 * S);
}

//--------------------------------------------------------------
void ofApp::startRecording() {
    if (recording) return;
    { std::lock_guard<std::mutex> lock(mtx); recAudio.clear(); }
    std::string dir = "/Users/ishi/Music/Greenshift/recordings/";
    std::string base = "GREENSHIFT_" + ofToString(sTransmission) + "_" + sDate;
    std::string safe; for (char c : base) safe += (isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-') ? c : '_';
    recFinalPath = dir + safe + ".mp4";
    std::string vidTmp = dir + safe + ".video.mp4";
    std::string cmd = std::string(FFMPEG) + " -y -f rawvideo -pixel_format rgba -video_size " +
        ofToString(REC_W) + "x" + ofToString(REC_H) + " -framerate 30 -i - -an "
        "-c:v h264_videotoolbox -b:v 40M -pix_fmt yuv420p \"" + vidTmp + "\" 2>>/tmp/gs_ffmpeg.log";
    vidPipe = popen(cmd.c_str(), "w");
    recAccum = 0; recStart = t; recording = true;
    ofLogNotice() << "REC start -> " << recFinalPath;
}

void ofApp::stopRecording() {
    if (!recording) return;
    recording = false;
    if (vidPipe) { pclose(vidPipe); vidPipe = nullptr; }
    std::string b = recFinalPath.substr(0, recFinalPath.size() - 4);
    std::string vidTmp = b + ".video.mp4", audTmp = b + ".audio.wav";
    { std::lock_guard<std::mutex> lock(mtx); writeWav(audTmp, recAudio, recChannels, sampleRate); }
    std::string mux = std::string(FFMPEG) + " -y -i \"" + vidTmp + "\" -i \"" + audTmp +
        "\" -c:v copy -c:a aac -b:a 256k -shortest \"" + recFinalPath + "\" 2>>/tmp/gs_ffmpeg.log"
        " && rm -f \"" + vidTmp + "\" \"" + audTmp + "\"";
    system(mux.c_str());
    ofLogNotice() << "REC saved -> " << recFinalPath;
}

void ofApp::writeRecordFrame() {
    fboRec.begin();
    ofClear(0, 0, 0, 255);
    ofSetColor(255); ofEnableBlendMode(OF_BLENDMODE_DISABLED);
    fboFinal.draw(0, 0, REC_W, REC_H);
    ofEnableBlendMode(OF_BLENDMODE_ALPHA);
    fboRec.end();
    static ofPixels px;
    fboRec.readToPixels(px);
    if (vidPipe) fwrite(px.getData(), 1, (size_t)REC_W * REC_H * 4, vidPipe);
}

//--------------------------------------------------------------
void ofApp::keyPressed(int key) {
    if (key == 'g' || key == 'G') showPanel = !showPanel;
    else if (key == 'h' || key == 'H') showHud = !showHud;
    else if (key == 'f' || key == 'F') ofToggleFullscreen();
    else if (key == 'r' || key == 'R') { if (recording) stopRecording(); else startRecording(); }
    else if (key == 'l' || key == 'L') loadSession();
    else if (key == 's' || key == 'S') ofSaveScreen("/tmp/greenshift_frame.png");
}

ofRectangle ofApp::displayRect() {
    float ww = ofGetWidth(), wh = ofGetHeight();
    float fa = (float)RW / RH, wa = ww / wh;
    float dw, dh;
    if (wa > fa) { dh = wh; dw = dh * fa; } else { dw = ww; dh = dw / fa; }
    return ofRectangle((ww - dw) * 0.5f, (wh - dh) * 0.5f, dw, dh);
}

void ofApp::mousePressed(int x, int y, int button) {
    if (!showPanel || recording) return;
    ofRectangle r = displayRect();
    float fx = (x - r.x) * RW / r.width, fy = (y - r.y) * RH / r.height;   // window -> FBO coords
    for (size_t i = 0; i < sliders.size(); i++) {
        ofRectangle hit = sliders[i].track; hit.y -= 12 * S; hit.height += 24 * S;
        if (hit.inside(fx, fy)) { activeSlider = (int)i; mouseDragged(x, y, button); return; }
    }
}
void ofApp::mouseDragged(int x, int y, int button) {
    if (activeSlider < 0) return;
    ofRectangle r = displayRect();
    float fx = (x - r.x) * RW / r.width;
    Slider& s = sliders[activeSlider];
    float v = ofMap(fx, s.track.x, s.track.x + s.track.width, s.lo, s.hi, true);
    *s.val = s.intStep ? roundf(v) : v;
}
void ofApp::mouseReleased(int x, int y, int button) { activeSlider = -1; }

void ofApp::windowResized(int w, int h) {}
void ofApp::exit() { if (recording) stopRecording(); stream.close(); }

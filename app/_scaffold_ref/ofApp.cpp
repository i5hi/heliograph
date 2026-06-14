#include "ofApp.h"

//--------------------------------------------------------------
void ofApp::setup() {
    ofSetFrameRate(60);
    ofSetVerticalSync(true);
    ofEnableAntiAliasing();
    ofEnableSmoothing();
    ofSetCircleResolution(120);

    // palette: neutral grayscale space + one clay-orange accent.
    bgCol.set(14, 13, 14);            // near-black neutral
    planetCol.set(190, 104, 60);      // soft clay / terracotta
    ringCol.set(214, 120, 70);        // clay, a touch warmer
    glowCol.set(255, 110, 40);        // the faint "neon" glow (spacey hint)
    starCol.set(205, 202, 198);       // grayscale starlight

    ringBuf.assign(N, 0.0f);
    spectrum.assign(N / 2, 0.0f);
    spectrumSmooth.assign(N / 2, 0.0f);
    wave.assign(M, 0.0f);

    buildStars();
    setupAudio();
}

//--------------------------------------------------------------
void ofApp::setupAudio() {
    ofSoundStreamSettings settings;
    auto devices = soundStream.getDeviceList();

    int chosen = -1;
    for (size_t i = 0; i < devices.size(); i++) {
        if (devices[i].inputChannels > 0 &&
            ofToLower(devices[i].name).find("blackhole") != std::string::npos) {
            chosen = (int)i; break;
        }
    }
    if (chosen < 0)
        for (size_t i = 0; i < devices.size(); i++)
            if (devices[i].inputChannels > 0) { chosen = (int)i; break; }

    if (chosen >= 0) {
        settings.setInDevice(devices[chosen]);
        deviceName = devices[chosen].name;
        ofLogNotice() << "Audio input: " << deviceName;
    } else {
        deviceName = "(no input device found)";
        ofLogError() << deviceName;
    }

    settings.setInListener(this);
    settings.sampleRate = sampleRate;
    settings.numInputChannels = 2;
    settings.numOutputChannels = 0;
    settings.bufferSize = bufferSize;
    soundStream.setup(settings);
}

//--------------------------------------------------------------
void ofApp::audioIn(ofSoundBuffer & input) {
    const size_t frames = input.getNumFrames();
    const size_t ch = input.getNumChannels();
    if (ch == 0) return;

    std::lock_guard<std::mutex> lock(audioMutex);
    float sum = 0.0f;
    for (size_t i = 0; i < frames; i++) {
        float mono = 0.0f;
        for (size_t c = 0; c < ch; c++) mono += input.getSample(i, c);
        mono /= (float)ch;
        ringBuf[writePos] = mono;
        writePos = (writePos + 1) % N;
        sum += mono * mono;
    }
    rms = sqrtf(sum / (float)frames);
}

//--------------------------------------------------------------
// Compact in-place radix-2 FFT (Cooley-Tukey) with Hann window. No addons.
void ofApp::computeFFT(const std::vector<float>& in, std::vector<float>& outMag) {
    static std::vector<float> re, im;
    re.assign(in.begin(), in.end());
    im.assign(N, 0.0f);

    for (int i = 0; i < N; i++)
        re[i] *= 0.5f * (1.0f - cosf(TWO_PI * i / (N - 1)));   // Hann

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
                float tr = cr * re[b] - ci * im[b];
                float ti = cr * im[b] + ci * re[b];
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
    outMag.resize(N / 2);
    for (int i = 0; i < N / 2; i++)
        outMag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]) / (N * 0.5f);
}

//--------------------------------------------------------------
float ofApp::bandEnergy(float fLo, float fHi) {
    int lo = std::max(1, (int)(fLo * N / sampleRate));
    int hi = std::min(N / 2 - 1, (int)(fHi * N / sampleRate));
    if (hi <= lo) return 0.0f;
    float s = 0.0f;
    for (int i = lo; i <= hi; i++) s += spectrumSmooth[i];
    return s / (hi - lo + 1);
}

//--------------------------------------------------------------
void ofApp::buildStars() {
    stars.clear();
    int count = 240;          // minimal, not busy
    for (int i = 0; i < count; i++) {
        Star s;
        s.npos.set(ofRandom(1.0f), ofRandom(1.0f));
        s.size = ofRandom(0.6f, 2.0f);
        s.base = ofRandom(0.15f, 0.75f);
        s.phase = ofRandom(TWO_PI);
        stars.push_back(s);
    }
}

//--------------------------------------------------------------
void ofApp::update() {
    float dt = std::min(ofGetLastFrameTime(), 0.05);
    t += dt;

    // ---- snapshot audio + FFT ----
    std::vector<float> w(N);
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        for (int i = 0; i < N; i++) w[i] = ringBuf[(writePos + i) % N];
    }
    computeFFT(w, spectrum);
    for (int i = 0; i < N / 2; i++) {
        if (spectrum[i] > spectrumSmooth[i]) spectrumSmooth[i] = spectrum[i];
        else spectrumSmooth[i] = ofLerp(spectrumSmooth[i], spectrum[i], 0.16f);
    }

    // ---- loudness + bass + beat ----
    float lvlTarget = ofClamp(rms * 5.0f, 0.0f, 1.0f);
    level = ofLerp(level, lvlTarget, lvlTarget > level ? 0.5f : 0.08f);

    float bT = ofClamp(bandEnergy(30, 180) * 22.0f, 0.0f, 1.0f);
    bass = ofLerp(bass, bT, bT > bass ? 0.6f : 0.12f);
    if (bT > prevBass * 1.35f && bT > 0.16f) beat = 1.0f;
    prevBass = ofLerp(prevBass, bT, 0.10f);
    beat *= 0.90f;

    // ---- build looped, smoothed waveform around the ring ----
    // Symmetric mirror so the ring closes seamlessly (start == end).
    int half = M / 2;
    std::vector<float> raw(M);
    for (int k = 0; k < M; k++) {
        int j = (k < half) ? k : (M - 1 - k);
        int idx = (int)((float)j / half * (N - 1));
        raw[k] = w[idx];
    }
    // a couple of smoothing passes -> curved & smooth
    for (int pass = 0; pass < 2; pass++) {
        std::vector<float> tmp = raw;
        for (int k = 0; k < M; k++) {
            float a = tmp[(k - 1 + M) % M];
            float b = tmp[k];
            float c = tmp[(k + 1) % M];
            raw[k] = (a + 2.0f * b + c) * 0.25f;
        }
    }
    for (int k = 0; k < M; k++) wave[k] = ofLerp(wave[k], raw[k], 0.5f);

    spin += (3.0f + level * 16.0f) * dt;   // gentle rotation, faster when loud
}

//--------------------------------------------------------------
void ofApp::draw() {
    ofBackground(bgCol);
    float W = ofGetWidth(), H = ofGetHeight();
    float cx = W * 0.5f, cy = H * 0.5f;

    // ---- stars (grayscale, faint twinkle) ----
    ofEnableAlphaBlending();
    for (auto& s : stars) {
        float a = s.base * (0.55f + 0.45f * sinf(t * 1.3f + s.phase));
        ofSetColor(starCol, (int)(a * 200));
        ofDrawCircle(s.npos.x * W, s.npos.y * H, s.size);
    }

    float planetR = std::min(W, H) * 0.16f * (1.0f + bass * 0.05f + beat * 0.03f);
    float ringR   = planetR * 1.7f;
    float ampOut  = planetR * (0.16f + level * 0.65f + beat * 0.22f);
    float phi     = ofDegToRad(74.0f);     // near edge-on tilt
    float cph = cosf(phi), sph = sinf(phi);

    // ---- compute ring points (x, y, depth) ----
    std::vector<glm::vec3> pts(M);
    for (int i = 0; i < M; i++) {
        float a = TWO_PI * i / M + spin;
        float wv = wave[i];
        glm::vec3 L(cosf(a) * ringR, wv * ampOut, sinf(a) * ringR);
        float y1 = L.y * cph - L.z * sph;
        float z1 = L.y * sph + L.z * cph;
        pts[i] = glm::vec3(cx + L.x, cy + y1, z1);
    }
    // split into segments behind / in front of the planet
    ofMesh behind, front;
    behind.setMode(OF_PRIMITIVE_LINES);
    front.setMode(OF_PRIMITIVE_LINES);
    for (int i = 0; i < M; i++) {
        int n = (i + 1) % M;
        glm::vec3 p0(pts[i].x, pts[i].y, 0), p1(pts[n].x, pts[n].y, 0);
        float midz = (pts[i].z + pts[n].z) * 0.5f;
        ofMesh& dst = (midz < 0.0f) ? behind : front;
        dst.addVertex(p0); dst.addVertex(p1);
    }

    auto drawRing = [&](ofMesh& m) {
        // soft glow (neon touch)
        ofEnableBlendMode(OF_BLENDMODE_ADD);
        ofSetColor(glowCol, (int)(36 * glow * (0.6f + beat)));
        ofSetLineWidth(7.0f); m.draw();
        ofSetColor(glowCol, (int)(60 * glow));
        ofSetLineWidth(3.5f); m.draw();
        // clay core
        ofEnableBlendMode(OF_BLENDMODE_ALPHA);
        ofSetColor(ringCol);
        ofSetLineWidth(1.7f); m.draw();
    };

    // ---- draw order: back ring -> planet -> equator -> front ring ----
    drawRing(behind);

    // planet: matte clay disc + faint warm rim (atmosphere)
    ofEnableBlendMode(OF_BLENDMODE_ADD);
    ofSetColor(glowCol, (int)(16 + bass * 26));
    ofDrawCircle(cx, cy, planetR * 1.08f);
    ofEnableBlendMode(OF_BLENDMODE_ALPHA);
    ofSetColor(planetCol);
    ofDrawCircle(cx, cy, planetR);

    // equator waveform across the planet (subtle)
    ofPolyline eq;
    for (int k = 0; k <= 80; k++) {
        float fx = (float)k / 80.0f;                 // 0..1 across planet
        float px = cx + (fx * 2.0f - 1.0f) * planetR * 0.94f;
        float py = cy + wave[(int)(fx * (M - 1))] * planetR * 0.14f;
        eq.addVertex(px, py);
    }
    ofSetColor(ringCol, 90);
    ofSetLineWidth(1.4f);
    eq.draw();

    drawRing(front);

    ofSetLineWidth(1.0f);

    // ---- HUD ----
    if (showHud) {
        std::string info;
        info += "Saturn Audio Visualizer   " + ofToString(ofGetFrameRate(), 0) + " fps\n";
        info += "input: " + deviceName + "   level: " + ofToString(level, 2) + "\n";
        info += "[h] hud  [+/-] glow  [r] stars  [f] fullscreen";
        ofDrawBitmapStringHighlight(info, 16, 24,
            ofColor(0, 0, 0, 140), ofColor(220, 150, 110));
    }
}

//--------------------------------------------------------------
void ofApp::keyPressed(int key) {
    if (key == 'h' || key == 'H') showHud = !showHud;
    else if (key == 'f' || key == 'F') ofToggleFullscreen();
    else if (key == 'r' || key == 'R') buildStars();
    else if (key == '+' || key == '=') glow = ofClamp(glow + 0.1f, 0.0f, 3.0f);
    else if (key == '-' || key == '_') glow = ofClamp(glow - 0.1f, 0.0f, 3.0f);
}

//--------------------------------------------------------------
void ofApp::windowResized(int w, int h) {}

//--------------------------------------------------------------
void ofApp::exit() {
    soundStream.close();
}

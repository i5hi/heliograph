#pragma once

#include "ofMain.h"
#include <mutex>
#include <vector>

struct Star { glm::vec2 p; float r, base, ph; };
struct Env  { float v = 0, atk = 0.40f, rel = 0.08f; float process(float t){ v += (t > v ? atk : rel) * (t - v); return v; } };
struct Slider { std::string name; float* val = nullptr; float lo = 0, hi = 1; int side = 0; bool intStep = false; int prec = 2; bool reactive = false; ofRectangle track; };

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
    static constexpr int N = 2048;
    std::vector<float> ringBuf;
    int    writePos = 0;
    std::mutex mtx;
    std::vector<float> spectrum, spectrumSmooth;
    float  rms = 0;
    Env    levelE, bassE, midE, highE;
    float  level = 0, bass = 0, mid = 0, high = 0, prevBass = 0, beatEnv = 0;
    static constexpr int BANDS = 128;
    std::vector<float> bands;

    // ---- forest ----
    void  addTree(float pos, float h, float spr);
    void  branch(float len, float angle, int depth);
    // ---- visualizer (audioVisualizer port) ----
    void  drawPortal();
    float rotationX = 0, rotationY = 0, rotationZ = 0;

    void  drawScene();
    void  drawSpace();
    void  drawStars();
    void  drawForest();
    std::vector<Star> stars;
    float t = 0;

    // ---- render (4K native; displayed crisp on Retina) ----
    static constexpr int RW = 2560, RH = 1440, REC_W = 2560, REC_H = 1440;   // render = window (1:1 crisp); record 1440p
    float S = 1.0f;
    float fm = 60;                 // frame margin (corner ticks + content bounds)
    ofFbo fboFinal, fboRec;

    // ---- recorder ----
    FILE*  vidPipe = nullptr;
    std::vector<short> recAudio;
    int    recChannels = 2;
    double recAccum = 0;
    std::string recFinalPath;
    bool   autoRecTest = false, didRecTest = false;
    void   startRecording();
    void   stopRecording();
    void   writeRecordFrame();

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
    float cfgBands   = 40;      // number of rings (4..128)
    float cfgR = 175, cfgG = 175, cfgB = 175;  // base ring colour (RGB 0..255) — grey by default; audio shifts it
    float cfgRotX = 0, cfgRotY = 0, cfgRotZ = 0;  // cumulative 3D ring rotation (deg/frame, ±0.12)
    float fftGain    = 600.0f;  // overall audio sensitivity feeding the rings + tree

    // ---- controls ----
    std::vector<Slider> sliders;
    int   activeSlider = -1;
    void  buildSliders();
    bool  showPanel = true, recording = false;
    float recStart = 0;
    bool  showHud = true, autoShot = true;

    // ---- HUD / session ----
    ofTrueTypeFont fKick, fTitle, fLabel, fValue, fNote, fUI;
    int   sTransmission = 1;
    std::string sTitle = "UNTITLED", sDate = "", sArtist = "", sChannel = "GREENSHIFT",
                sNote = "", sWaypoint = "00·00·000";
    float sHeading = 0, sDist = 0;
    void  loadSession();
    void  drawHud();
    void  drawPanels();
    ofRectangle displayRect();     // aspect-fit (letterbox) rect for the FBO in the window

    // palette (GREENSHIFT "Jade Shift"): cNeon = brand green, cStar = starfield grey. TWEAK to reskin.
    ofColor cTrunk{16, 74, 50}, cNeon{52, 245, 166}, cPastel{143, 233, 196}, cStar{170, 184, 178};
};

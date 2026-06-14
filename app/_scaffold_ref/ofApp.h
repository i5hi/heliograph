#pragma once

#include "ofMain.h"
#include <mutex>
#include <vector>

// A faint grayscale star.
struct Star {
    glm::vec2 npos;   // normalized 0..1 position
    float size;
    float base;       // base brightness 0..1
    float phase;      // twinkle phase
};

class ofApp : public ofBaseApp {
public:
    void setup();
    void update();
    void draw();
    void exit();
    void keyPressed(int key);
    void windowResized(int w, int h);

    // ---- audio ----
    void audioIn(ofSoundBuffer & input);
    void setupAudio();

    ofSoundStream soundStream;
    int    sampleRate = 44100;
    int    bufferSize = 512;
    std::string deviceName = "(none)";

    static const int N = 2048;          // FFT / capture window (power of 2)
    std::vector<float> ringBuf;         // circular buffer of mono samples
    int    writePos = 0;
    std::mutex audioMutex;

    std::vector<float> spectrum;        // magnitude N/2
    std::vector<float> spectrumSmooth;
    void  computeFFT(const std::vector<float>& in, std::vector<float>& out);
    float bandEnergy(float fLo, float fHi);

    float rms = 0, level = 0;           // overall loudness (smoothed)
    float bass = 0, prevBass = 0, beat = 0;

    // ---- waveform around the ring ----
    static const int M = 720;           // samples around the ring
    std::vector<float> wave;            // looped, smoothed waveform (-1..1)

    // ---- visuals ----
    std::vector<Star> stars;
    void  buildStars();
    float t = 0, spin = 0;
    bool  showHud = true;
    float glow = 1.0f;                  // "neon touch" intensity

    // palette: grayscale + a single clay-orange accent
    ofColor bgCol, planetCol, ringCol, glowCol, starCol;
};

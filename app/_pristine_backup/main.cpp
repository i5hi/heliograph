#include "ofMain.h"
#include "ofApp.h"

int main() {
    ofGLFWWindowSettings settings;
    settings.setSize(2560, 1440);    // locked 16:9 window; FBO renders 1:1 at this size (crisp)
    settings.setGLVersion(2, 1);     // legacy pipeline: wide lines work here
    settings.numSamples = 0;         // window just blits a texture; AA happens in the FBO
    settings.resizable = false;      // lock the resolution
    settings.title = "GREENSHIFT";

    auto window = ofCreateWindow(settings);
    ofRunApp(window, std::make_shared<ofApp>());
    ofRunMainLoop();
}

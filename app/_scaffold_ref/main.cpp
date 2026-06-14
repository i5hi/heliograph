#include "ofMain.h"
#include "ofApp.h"

int main() {
    ofGLFWWindowSettings settings;
    settings.setSize(1280, 800);
    settings.setGLVersion(2, 1);   // fixed pipeline is plenty for this 2D scene
    settings.numSamples = 8;       // MSAA -> smooth, curved lines
    settings.title = "Saturn Audio Visualizer";

    auto window = ofCreateWindow(settings);
    ofRunApp(window, std::make_shared<ofApp>());
    ofRunMainLoop();
}

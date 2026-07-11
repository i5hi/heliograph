#include "ofMain.h"
#include "ofApp.h"

int main() {
#ifdef TARGET_OSX
    // Self-contained .app: if data/ was copied into the bundle (Contents/Resources/data), point the
    // data root there so the app needs no sibling bin/data. Falls back to the default (bin/data) in dev.
    std::string bundleData = ofFilePath::getCurrentExeDir() + "../Resources/data/";
    if (ofDirectory::doesDirectoryExist(bundleData)) ofSetDataPathRoot(bundleData);
#endif
    ofGLFWWindowSettings settings;
    settings.setSize(2560, 1440);    // locked 16:9 window; FBO renders 1:1 at this size (crisp)
    settings.setGLVersion(2, 1);     // legacy pipeline: wide lines work here
    settings.numSamples = 0;         // window just blits a texture; AA happens in the FBO
    // Resizable, not locked: native macOS fullscreen (Cocoa's toggleFullScreen:, used by 'F') requires
    // NSWindowCollectionBehaviorFullScreenPrimary, which GLFW only grants a resizable window — a
    // fixed-size window silently can't go fullscreen. This doesn't touch rendered/recorded quality:
    // the FBO stays a fixed RW×RH (see ofApp.h) regardless of window size; only the on-screen blit
    // (displayRect(), already written to letterbox at any window/fullscreen size) changes.
    settings.resizable = true;
    settings.title = "heliograph";

    auto window = ofCreateWindow(settings);
    ofRunApp(window, std::make_shared<ofApp>());
    ofRunMainLoop();
}

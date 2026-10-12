#pragma once

// bridger-gui.exe application shell (docs/adr/0009): an SDL3 window whose client area is drawn by
// the Vulkan viewer, with the WebView2 user interface layered on top (transparent background).
// The UI talks to the app through JSON messages (docs/app/bridge.md).

#include <memory>
#include <string>

struct SDL_Window;

namespace bridger::app {

class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Returns a process exit code. `open_path`: optional file or world to open at start.
    int run(const std::string& open_path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::app

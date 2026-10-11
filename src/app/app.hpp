#pragma once

// bridger-gui.exe application shell.
// Phase 0: opens an empty window so the SDL3 + ImGui toolchain is proven end to end.
// The Vulkan renderer and ImGui UI arrive in Phases 7 and 8.

struct SDL_Window;

namespace bridger::app {

class App {
public:
    App() = default;
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Returns a process exit code.
    int run();

private:
    bool init();
    void shutdown();

    SDL_Window* window_ = nullptr;
    bool sdl_initialized_ = false;
};

} // namespace bridger::app

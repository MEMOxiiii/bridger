#pragma once

// The desktop app (docs/adr/0009, 0010): the Vulkan viewer with the web user interface layered on top
// (transparent background), in a window provided by the system's host (host.hpp). The UI talks to the app
// through JSON messages (docs/app/bridge.md).

#include <memory>
#include <optional>
#include <string>

namespace bridger::app {

class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    struct RunOptions {
        std::string open_path; // a file or world to open at start (optional)
        // --smoke-test[=seconds]: wait for the interface and, with a file, for the 3D preview's first frame (or for the
        // file page when there is no GPU), keep the window that many seconds more, print a summary and quit. Exit
        // code 0 when everything came up, 1 otherwise or after 90 s (docs/testing.md; CI runs it on every system).
        std::optional<int> smoke_test;
    };
    // Returns a process exit code.
    int run(const RunOptions& options);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::app

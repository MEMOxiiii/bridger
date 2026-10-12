// bridger-gui.exe — desktop app entry point.

#include "app.hpp"

#include <SDL3/SDL_main.h>

#include <cstdio>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    std::string open_path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--version") {
            std::printf("bridger-gui %s\n", BRIDGER_VERSION_STRING);
            return 0;
        }
        if (!a.starts_with("--")) open_path = argv[i]; // a file passed on the command line or by "Open with"
    }
    bridger::app::App app;
    return app.run(open_path);
}

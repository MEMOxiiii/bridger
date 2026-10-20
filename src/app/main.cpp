// bridger-gui — desktop app entry point (Windows, Linux; Bridger.app on macOS).

#include "app.hpp"

#ifdef _WIN32
#include <SDL3/SDL_main.h> // WinMain -> main (the Windows host uses SDL, docs/adr/0009)
#endif

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    bridger::app::App::RunOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--version") {
            std::printf("bridger-gui %s\n", BRIDGER_VERSION_STRING);
            return 0;
        }
        if (a == "--smoke-test") options.smoke_test = 0;
        else if (a.starts_with("--smoke-test=")) options.smoke_test = std::atoi(argv[i] + 13);
        else if (!a.starts_with("-")) options.open_path = argv[i]; // a file passed on the command line or by "Open with"
    }
    bridger::app::App app;
    return app.run(options);
}

// bridger-gui.exe — desktop app entry point.

#include "app.hpp"

#include <SDL3/SDL_main.h>
#include <imgui.h>

#include <cstdio>
#include <string_view>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--version") {
            std::printf("bridger-gui %s (Dear ImGui %s)\n", BRIDGER_VERSION_STRING, ImGui::GetVersion());
            return 0;
        }
    }
    bridger::app::App app;
    return app.run();
}

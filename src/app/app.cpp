#include "app.hpp"

#include <bridger/core/log.hpp>

#include <SDL3/SDL.h>

namespace bridger::app {

App::~App() { shutdown(); }

bool App::init() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log::error("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    sdl_initialized_ = true;

    window_ = SDL_CreateWindow("Bridger " BRIDGER_VERSION_STRING, 1280, 800, SDL_WINDOW_RESIZABLE);
    if (window_ == nullptr) {
        log::error("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    return true;
}

void App::shutdown() {
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (sdl_initialized_) {
        SDL_Quit();
        sdl_initialized_ = false;
    }
}

int App::run() {
    if (!init()) return 1;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_WaitEventTimeout(&event, 100)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
        }
    }
    return 0;
}

} // namespace bridger::app

// Windows host (docs/adr/0009): an SDL3 window, WebView2 in visual mode above a DirectComposition swapchain that
// receives the Vulkan scene through a shared D3D11 texture (Compositor).

#include "../host.hpp"

#include "compositor.hpp"
#include "shell.hpp"
#include "webview_host.hpp"

#include <bridger/core/log.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace bridger::app {
namespace {

constexpr u32 kCharcoal = 0x0F1217; // --bg in src/app/ui/src/styles/tokens.css

// An SDL dialog's filters and callback, alive until SDL calls back (SDL keeps pointers to the filter strings).
struct DialogCall {
    std::vector<std::string> names, patterns;
    std::vector<SDL_DialogFileFilter> filters;
    PathCallback done;

    explicit DialogCall(PathCallback cb, std::span<const FileFilter> list = {}) : done(std::move(cb)) {
        for (const FileFilter& f : list) {
            names.push_back(f.name);
            patterns.push_back(f.patterns);
        }
        for (usize i = 0; i < names.size(); ++i) filters.push_back({names[i].c_str(), patterns[i].c_str()});
    }

    // SDL's callback (any thread): takes the call back from SDL and answers.
    static void answer(void* self, const char* const* list, int) {
        std::unique_ptr<DialogCall> call(static_cast<DialogCall*>(self));
        if (list == nullptr || list[0] == nullptr) call->done(std::nullopt);
        else call->done(std::string(list[0]));
    }
};

} // namespace

struct Host::Impl {
    HostEvents events;
    SDL_Window* window = nullptr;
    void* hwnd = nullptr;
    u32 width = 1, height = 1;
    std::unique_ptr<Compositor> compositor;
    std::unique_ptr<WebViewHost> web;
    bool quit = false;
    bool fly = false;

    ~Impl() {
        web.reset();
        compositor.reset();
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    void handle_event(const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: events.close_requested(); break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            const auto w = static_cast<u32>(std::max(1, e.window.data1)), h = static_cast<u32>(std::max(1, e.window.data2));
            if (web) web->resize(e.window.data1, e.window.data2);
            if (compositor && compositor->resize(w, h)) {
                width = w;
                height = h;
                events.resized(w, h);
            }
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // Focus moving to the WebView inside this window is not "leaving the app".
            if (!window_is_foreground(hwnd)) events.focus_lost();
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (fly) events.fly_look(e.motion.xrel, e.motion.yrel);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (fly) events.fly_wheel(e.wheel.y);
            break;
        case SDL_EVENT_KEY_DOWN:
            // Scancodes: physical keys, whatever the keyboard layout (WASD on an Arabic layout too).
            if (fly && e.key.scancode == SDL_SCANCODE_ESCAPE) events.fly_escape();
            break;
        case SDL_EVENT_DROP_BEGIN: events.drop_hover(true); break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data != nullptr) events.drop_file(e.drop.data);
            break;
        case SDL_EVENT_DROP_COMPLETE: events.drop_hover(false); break;
        default: break;
        }
    }
};

Host::Host() : impl_(std::make_unique<Impl>()) {}
Host::~Host() = default;

std::unique_ptr<Host> Host::create(HostEvents events) {
    std::unique_ptr<Host> host(new Host());
    Impl& d = *host->impl_;
    d.events = std::move(events);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log::error("SDL_Init failed: {}", SDL_GetError());
        return nullptr;
    }
    d.window = SDL_CreateWindow("Bridger", 1440, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (d.window == nullptr) {
        log::error("SDL_CreateWindow failed: {}", SDL_GetError());
        return nullptr;
    }
    SDL_SetWindowMinimumSize(d.window, 960, 600);
    d.hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(d.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    style_title_bar(d.hwnd, kCharcoal);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(d.window, &w, &h);
    d.width = static_cast<u32>(std::max(1, w));
    d.height = static_cast<u32>(std::max(1, h));
    return host;
}

SceneMode Host::scene_mode() const noexcept { return SceneMode::SharedTexture; }
u32 Host::width() const noexcept { return impl_->width; }
u32 Host::height() const noexcept { return impl_->height; }

bool Host::attach_scene(std::optional<u64> adapter_luid) {
    impl_->compositor = Compositor::create(impl_->hwnd, adapter_luid, impl_->width, impl_->height);
    return impl_->compositor != nullptr;
}

void* Host::shared_handle() const noexcept { return impl_->compositor ? impl_->compositor->shared_handle() : nullptr; }
void Host::begin_frame() { impl_->compositor->wait_for_copy(); }
void Host::present() { impl_->compositor->present(); }
void Host::present_pixels(std::span<const u8>, u32, u32) {}

void Host::start_web(std::function<std::optional<WebFile>(std::string_view path)> resource, bool dev_tools) {
    Impl& d = *impl_;
    WebViewCallbacks cb;
    cb.resource = [resource = std::move(resource)](std::string_view path) -> std::optional<WebResource> {
        const auto f = resource(path);
        if (!f) return std::nullopt;
        return WebResource{f->bytes, f->mime};
    };
    cb.on_message = [&d](std::string_view json) { d.events.message(json); };
    cb.on_ready = [&d](bool ok, std::string_view err) {
        d.compositor->commit();
        if (ok) d.events.web_ready(true, {});
        else
            d.events.web_ready(false, std::string(err) + "\n\nBridger needs the Microsoft Edge WebView2 Runtime (included in Windows 11).");
    };
    std::string data_dir = "bridger-webview";
    if (const char* local = std::getenv("LOCALAPPDATA")) data_dir = std::string(local) + "\\Bridger\\WebView2";
    d.web = WebViewHost::create(d.hwnd, d.compositor->ui_visual(), "https://bridger.local/index.html", data_dir, std::move(cb), dev_tools);
}

void Host::post_json(std::string_view json) {
    if (impl_->web) impl_->web->post_json(json);
}
void Host::focus_web() {
    if (impl_->web) impl_->web->focus();
}

void Host::show() { SDL_ShowWindow(impl_->window); }
void Host::set_title(std::string_view utf8) { SDL_SetWindowTitle(impl_->window, std::string(utf8).c_str()); }
void Host::error_box(std::string_view message) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Bridger", std::string(message).c_str(), impl_->window);
}

bool Host::save_screenshot(std::string_view) { return false; } // the window is captured from outside on Windows

void Host::run(const std::function<bool()>& tick) {
    bool busy = true;
    while (!impl_->quit) {
        SDL_Event e;
        // Idle: wait for input instead of spinning when nothing moves.
        if (busy ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, 15)) {
            impl_->handle_event(e);
            while (SDL_PollEvent(&e)) impl_->handle_event(e);
        }
        if (impl_->quit) break;
        busy = tick();
    }
}

void Host::quit() { impl_->quit = true; }

void Host::open_file(std::span<const FileFilter> filters, PathCallback done) {
    auto call = std::make_unique<DialogCall>(std::move(done), filters);
    const auto* list = call->filters.data();
    const int n = static_cast<int>(call->filters.size());
    SDL_ShowOpenFileDialog(&DialogCall::answer, call.release(), impl_->window, list, n, nullptr, false);
}

void Host::open_folder(std::string_view default_dir, PathCallback done) {
    const std::string dir(default_dir);
    SDL_ShowOpenFolderDialog(&DialogCall::answer, std::make_unique<DialogCall>(std::move(done)).release(), impl_->window,
                             dir.empty() ? nullptr : dir.c_str(), false);
}

void Host::save_file(const FileFilter& filter, std::string_view default_path, PathCallback done) {
    auto call = std::make_unique<DialogCall>(std::move(done), std::span<const FileFilter>(&filter, 1));
    const auto* list = call->filters.data();
    const std::string def(default_path);
    SDL_ShowSaveFileDialog(&DialogCall::answer, call.release(), impl_->window, list, 1, def.c_str());
}

void Host::reveal(std::string_view utf8_path) { reveal_in_explorer(utf8_path); }
void Host::open_url(std::string_view https_url) { app::open_url(https_url); }

FlyInput Host::fly_input() const noexcept { return FlyInput::Native; }

void Host::set_fly_capture(bool on) {
    Impl& d = *impl_;
    d.fly = on;
    SDL_SetWindowRelativeMouseMode(d.window, on);
    if (d.web) d.web->set_input_enabled(!on);
    if (on) focus_window(d.hwnd);
    else if (d.web) d.web->focus();
}

HostFlyKeys Host::fly_keys() const {
    const bool* k = SDL_GetKeyboardState(nullptr);
    return {k[SDL_SCANCODE_W],     k[SDL_SCANCODE_S],
            k[SDL_SCANCODE_A],     k[SDL_SCANCODE_D],
            k[SDL_SCANCODE_SPACE], k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT],
            k[SDL_SCANCODE_LCTRL]};
}

} // namespace bridger::app

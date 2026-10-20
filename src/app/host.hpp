#pragma once

// The desktop app's connection to the operating system (docs/adr/0010): the window, the web view with the user
// interface, presenting the 3D scene under it, file dialogs, file drops, the message loop and a few shell helpers.
// app.cpp holds all behaviour and no OS calls; one implementation of this class exists per system:
//   win32/host_win32.cpp  SDL3 + WebView2 + DirectComposition (docs/adr/0009)
//   gtk/host_gtk.cpp      GTK 4 + WebKitGTK 6.0
//   cocoa/host_cocoa.mm   AppKit + WKWebView
// Everything here runs on the UI thread, except where a comment says otherwise.

#include <bridger/core/types.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::app {

struct FileFilter {
    std::string name;     // "Bedrock world"
    std::string patterns; // extensions without dots, separated by ';' ("schem;nbt"), or "*"
};

// A dialog's answer: a UTF-8 path, or nullopt when cancelled. May be called on any thread.
using PathCallback = std::function<void(std::optional<std::string> path)>;

// How the scene reaches the screen.
enum class SceneMode {
    SharedTexture, // Windows: the renderer draws into a D3D11 texture (shared_handle()); present() shows it
    Pixels,        // Linux, macOS: the renderer draws offscreen; present_pixels() shows the RGBA rows
};

struct HostEvents {
    std::function<void(std::string_view json)> message;          // from the page
    std::function<void(bool ok, std::string_view error)> web_ready; // the web view started (or could not)
    std::function<void(u32 width, u32 height)> resized;          // the scene's new size in pixels
    std::function<void()> close_requested;
    std::function<void()> focus_lost;                     // the window lost the keyboard focus to another program
    std::function<void(bool inside)> drop_hover;          // files dragged over the window (true) or away (false)
    std::function<void(std::string path)> drop_file;      // one per dropped file
    // Native fly-mode input (fly_input() == Native): relative mouse motion, the wheel and Esc.
    std::function<void(float dx, float dy)> fly_look;
    std::function<void(float delta)> fly_wheel;
    std::function<void()> fly_escape;
};

// Keys held in fly mode (native input only).
struct HostFlyKeys {
    bool forward = false, back = false, left = false, right = false, up = false, down = false, fast = false;
};

enum class FlyInput {
    Native, // the host captures the mouse and reads the keys (Windows)
    Page,   // the page uses pointer lock and sends `look` / `fly_keys` (Linux, macOS; docs/app/bridge.md)
};

class Host {
public:
    // Creates the (hidden) window. nullptr on failure, after telling the user.
    static std::unique_ptr<Host> create(HostEvents events);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // ---- Scene -------------------------------------------------------------------------------
    [[nodiscard]] SceneMode scene_mode() const noexcept;
    [[nodiscard]] u32 width() const noexcept; // the scene size in pixels
    [[nodiscard]] u32 height() const noexcept;
    // SharedTexture: sets up the drawing surface for the GPU the renderer chose (Windows LUID); false on failure.
    // Pixels: nothing to set up.
    bool attach_scene(std::optional<u64> adapter_luid);
    [[nodiscard]] void* shared_handle() const noexcept; // SharedTexture: the NT handle the renderer draws into
    void begin_frame();                                // SharedTexture: wait until the previous frame was copied
    void present();                                    // SharedTexture: show the frame
    // Pixels: show `rgba` (width * height * 4 bytes, rows top to bottom). Copies; the span may die afterwards.
    void present_pixels(std::span<const u8> rgba, u32 width, u32 height);

    // ---- Web view ------------------------------------------------------------------------------
    struct WebFile {
        std::span<const u8> bytes; // must stay valid for the program's lifetime (the embedded bundle)
        std::string mime;
    };
    // Starts the web view on index.html of the embedded interface; `resource` answers a path such as "index.html".
    void start_web(std::function<std::optional<WebFile>(std::string_view path)> resource, bool dev_tools);
    void post_json(std::string_view json);
    void focus_web();

    // ---- Window --------------------------------------------------------------------------------
    void show();
    void set_title(std::string_view utf8);
    void error_box(std::string_view message); // blocking message box
    // Saves what the window shows (scene and interface) as a PNG; only the app's own window, never the screen.
    // Used by the smoke test (BRIDGER_SMOKE_SCREENSHOT). False where the host cannot do it.
    bool save_screenshot(std::string_view png_path);
    // The main loop: calls `tick` until quit(). `tick` returns true while it wants the next call as soon as possible
    // (an animation, a camera move, streaming); otherwise the host waits for input or about 15 ms.
    void run(const std::function<bool()>& tick);
    void quit();

    // ---- Dialogs, files, shell -------------------------------------------------------------------
    void open_file(std::span<const FileFilter> filters, PathCallback done);
    void open_folder(std::string_view default_dir, PathCallback done);
    void save_file(const FileFilter& filter, std::string_view default_path, PathCallback done);
    void reveal(std::string_view utf8_path); // show the file in the system's file manager
    void open_url(std::string_view https_url);

    // ---- Fly mode --------------------------------------------------------------------------------
    [[nodiscard]] FlyInput fly_input() const noexcept;
    void set_fly_capture(bool on); // Native: relative mouse mode, keys go to the app instead of the page
    [[nodiscard]] HostFlyKeys fly_keys() const;

private:
    Host();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::app

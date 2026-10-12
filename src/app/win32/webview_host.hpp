#pragma once

// Microsoft Edge WebView2 hosted in the app window (docs/adr/0009). Visual hosting: WebView2 draws
// into a DirectComposition visual above the 3D scene (Compositor), with a transparent page
// background; this class forwards the window's mouse input and cursor to it. JSON messages flow
// both ways; UI files are served from memory under https://bridger.local/. Windows only.

#include <bridger/core/types.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::app {

struct WebResource {
    std::span<const u8> bytes; // must stay valid while the request is answered
    std::string mime;
};

struct WebViewCallbacks {
    std::function<void(std::string_view json)> on_message;
    std::function<std::optional<WebResource>(std::string_view path)> resource; // path without the host, e.g. "index.html"
    std::function<void(bool ok, std::string_view error)> on_ready;
};

class WebViewHost {
public:
    // `hwnd`: the app window. `visual`: the IDCompositionVisual (IUnknown*) to draw into. `start_url`: e.g. "https://bridger.local/index.html".
    // `user_data_dir`: UTF-8 folder for WebView2's profile. Creation completes asynchronously on
    // this thread's message loop; on_ready reports the outcome.
    static std::unique_ptr<WebViewHost> create(void* hwnd, void* visual, std::string start_url, std::string user_data_dir, WebViewCallbacks callbacks,
                                               bool dev_tools);
    ~WebViewHost();
    WebViewHost(const WebViewHost&) = delete;
    WebViewHost& operator=(const WebViewHost&) = delete;

    void resize(int width, int height);
    void post_json(std::string_view json);
    void focus();
    // While disabled (fly mode), mouse input stays with the app window instead of the page.
    void set_input_enabled(bool enabled);
    [[nodiscard]] bool ready() const noexcept;

private:
    WebViewHost();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bridger::app

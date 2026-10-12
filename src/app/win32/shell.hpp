#pragma once

// Small Windows shell helpers for the desktop app.

#include <bridger/core/types.hpp>

#include <string_view>

namespace bridger::app {

// Opens an Explorer window with `utf8_path` selected (a file or a folder).
void reveal_in_explorer(std::string_view utf8_path);
// Dark title bar in the app's charcoal colour (Windows 11; ignored where unsupported).
void style_title_bar(void* hwnd, u32 rgb);
// True while `hwnd` (or a window inside it, such as WebView2's) is the foreground window.
[[nodiscard]] bool window_is_foreground(void* hwnd);
// Gives the keyboard focus back to the app window (fly mode reads WASD natively).
void focus_window(void* hwnd);

} // namespace bridger::app

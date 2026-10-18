#include "shell.hpp"

#include <bridger/platform/paths.hpp>

#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <dwmapi.h>

namespace bridger::app {

void reveal_in_explorer(std::string_view utf8_path) {
    auto path = platform::path_from_utf8(utf8_path);
    if (!path) return;
    std::filesystem::path p = *path;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(p.make_preferred().c_str());
    if (pidl == nullptr) return;
    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
}

void style_title_bar(void* hwnd, u32 rgb) {
    const HWND h = static_cast<HWND>(hwnd);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    DwmSetWindowAttribute(h, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &caption, sizeof(caption));
}

bool window_is_foreground(void* hwnd) {
    const HWND fg = GetForegroundWindow();
    return fg != nullptr && (fg == static_cast<HWND>(hwnd) || GetAncestor(fg, GA_ROOTOWNER) == static_cast<HWND>(hwnd));
}

void focus_window(void* hwnd) { SetFocus(static_cast<HWND>(hwnd)); }

void open_url(std::string_view utf8_url) {
    if (!utf8_url.starts_with("https://")) return;
    const auto url = platform::path_from_utf8(utf8_url); // UTF-8 -> UTF-16 (not used as a path)
    if (!url) return;
    ShellExecuteW(nullptr, L"open", url->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace bridger::app

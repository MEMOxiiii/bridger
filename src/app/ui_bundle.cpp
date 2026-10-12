#include "ui_bundle.hpp"

#include <bridger/core/log.hpp>
#include <bridger/io/zip_archive.hpp>

namespace bridger::embedded {
extern const unsigned char ui_bundle_zip[];
extern const std::size_t ui_bundle_zip_size;
} // namespace bridger::embedded

namespace bridger::app {

std::string_view mime_type(std::string_view path) noexcept {
    const auto dot = path.rfind('.');
    const std::string_view ext = dot == std::string_view::npos ? std::string_view() : path.substr(dot);
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ico") return "image/x-icon";
    return "application/octet-stream";
}

UiBundle UiBundle::embedded() {
    UiBundle b;
    io::ZipLimits limits;
    auto entries = io::read_zip(std::span(embedded::ui_bundle_zip, embedded::ui_bundle_zip_size), limits);
    if (!entries) {
        log::error("user interface bundle: {}", entries.error().describe());
        return b;
    }
    for (auto& e : *entries) {
        std::string name = e.name;
        if (name.starts_with("./")) name.erase(0, 2);
        if (name.empty() || name.back() == '/') continue;
        const std::string mime(mime_type(name));
        b.files_.emplace(std::move(name), Entry{std::move(e.data), mime});
    }
    return b;
}

std::optional<UiBundle::File> UiBundle::find(std::string_view path) const {
    const auto it = files_.find(std::string(path));
    if (it == files_.end()) return std::nullopt;
    return File{it->second.bytes, it->second.mime};
}

} // namespace bridger::app

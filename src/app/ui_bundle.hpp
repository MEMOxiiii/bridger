#pragma once

// The built web user interface (src/app/ui, `npm run build`), embedded as a zip at build time
// and served to WebView2 from memory.

#include <bridger/core/types.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bridger::app {

class UiBundle {
public:
    // Unpacks the embedded archive (empty bundle if it is missing or damaged; logged).
    static UiBundle embedded();

    struct File {
        std::span<const u8> bytes;
        std::string_view mime;
    };
    [[nodiscard]] std::optional<File> find(std::string_view path) const;
    [[nodiscard]] bool empty() const noexcept { return files_.empty(); }

private:
    struct Entry {
        std::vector<u8> bytes;
        std::string mime;
    };
    std::unordered_map<std::string, Entry> files_;
};

[[nodiscard]] std::string_view mime_type(std::string_view path) noexcept;

} // namespace bridger::app

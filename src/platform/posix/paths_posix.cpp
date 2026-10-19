// POSIX paths: file names are bytes, used as UTF-8 as they are (Linux and macOS both treat names as UTF-8 by
// convention). Settings folders follow each system's convention: XDG on Linux, ~/Library/Application Support on macOS.

#include <bridger/platform/paths.hpp>

#include <cstdlib>

namespace bridger::platform {
namespace {

// Rejects malformed UTF-8 so that both systems refuse the same names Windows refuses.
bool valid_utf8(std::string_view s) {
    for (usize i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const usize n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 4;
        if (n == 4 || i + n >= s.size() + (n == 0 ? 1 : 0)) return false; // a bad lead byte, or the sequence is cut off
        for (usize k = 1; k <= n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

std::optional<std::filesystem::path> env_path(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::filesystem::path(v);
}

std::optional<std::filesystem::path> home() { return env_path("HOME"); }

} // namespace

Result<std::filesystem::path> path_from_utf8(std::string_view utf8) {
    if (!valid_utf8(utf8)) return make_error(ErrorCode::InvalidArgument, "path is not valid UTF-8");
    return std::filesystem::path(std::string(utf8));
}

Result<std::string> path_to_utf8(const std::filesystem::path& path) {
    const std::string& s = path.native();
    if (!valid_utf8(s)) return make_error(ErrorCode::InvalidArgument, "path is not valid UTF-8");
    return s;
}

// No length limit to work around on POSIX: the path is returned absolute and unchanged otherwise.
Result<std::filesystem::path> to_long_path(const std::filesystem::path& path) { return path; }

std::optional<std::filesystem::path> roaming_app_data_dir() {
#ifdef __APPLE__
    if (const auto h = home()) return *h / "Library" / "Application Support";
    return std::nullopt;
#else
    if (const auto x = env_path("XDG_CONFIG_HOME")) return x;
    if (const auto h = home()) return *h / ".config";
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> local_app_data_dir() {
#ifdef __APPLE__
    if (const auto h = home()) return *h / "Library" / "Caches";
    return std::nullopt;
#else
    if (const auto x = env_path("XDG_DATA_HOME")) return x;
    if (const auto h = home()) return *h / ".local" / "share";
    return std::nullopt;
#endif
}

} // namespace bridger::platform

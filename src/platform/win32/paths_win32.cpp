#include <bridger/platform/paths.hpp>

#include <Windows.h>

#include <limits>

namespace bridger::platform {
namespace {

Result<std::wstring> utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) return std::wstring{};
    if (utf8.size() > static_cast<usize>(std::numeric_limits<int>::max()))
        return make_error(ErrorCode::LimitExceeded, "path too long");
    const int in_len = static_cast<int>(utf8.size());
    const int out_len = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), in_len, nullptr, 0);
    if (out_len <= 0) return make_error(ErrorCode::InvalidArgument, "path is not valid UTF-8");
    std::wstring wide(static_cast<usize>(out_len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), in_len, wide.data(), out_len);
    return wide;
}

Result<std::string> wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) return std::string{};
    if (wide.size() > static_cast<usize>(std::numeric_limits<int>::max()))
        return make_error(ErrorCode::LimitExceeded, "path too long");
    const int in_len = static_cast<int>(wide.size());
    const int out_len =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), in_len, nullptr, 0, nullptr, nullptr);
    if (out_len <= 0) return make_error(ErrorCode::InvalidArgument, "path contains invalid UTF-16");
    std::string utf8(static_cast<usize>(out_len), '\0');
    ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), in_len, utf8.data(), out_len, nullptr,
                          nullptr);
    return utf8;
}

} // namespace

Result<std::filesystem::path> path_from_utf8(std::string_view utf8) {
    BRIDGER_TRY(std::wstring wide, utf8_to_wide(utf8));
    return std::filesystem::path(std::move(wide));
}

Result<std::string> path_to_utf8(const std::filesystem::path& path) {
    return wide_to_utf8(path.native());
}

Result<std::filesystem::path> to_long_path(const std::filesystem::path& path) {
    const std::wstring& native = path.native();
    if (native.starts_with(LR"(\\?\)")) return path;

    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (ec) return make_error(ErrorCode::InvalidArgument, "cannot make path absolute: " + ec.message());
    abs = abs.lexically_normal();

    const std::wstring& a = abs.native();
    if (a.starts_with(LR"(\\)")) {
        // \\server\share\x -> \\?\UNC\server\share\x
        return std::filesystem::path(LR"(\\?\UNC\)" + a.substr(2));
    }
    return std::filesystem::path(LR"(\\?\)" + a);
}

} // namespace bridger::platform

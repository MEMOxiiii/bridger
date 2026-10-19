// POSIX: network access is not available yet. Only the Windows desktop app uses it (the client jar download and the
// self-update, docs/safety.md "Network access"); the command-line tool never does.

#include <bridger/platform/net.hpp>

namespace bridger::platform {

Result<std::vector<u8>> https_get(std::string_view, u64, const DownloadProgress&) {
    return make_error(ErrorCode::NotImplemented, "downloads are only available in the Windows desktop app");
}

Result<std::string> sha1_hex(std::span<const u8>) { return make_error(ErrorCode::NotImplemented, "SHA-1 is not available on this system yet"); }

Result<std::string> sha256_hex(std::span<const u8>) {
    return make_error(ErrorCode::NotImplemented, "SHA-256 is not available on this system yet");
}

} // namespace bridger::platform

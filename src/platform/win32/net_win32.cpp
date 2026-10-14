#include <bridger/platform/net.hpp>

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <memory>
#include <string>

namespace bridger::platform {
namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct InternetHandle {
    HINTERNET h = nullptr;
    ~InternetHandle() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
};

auto net_error(std::string_view what) {
    return make_error(ErrorCode::IoError, std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")");
}

} // namespace

Result<std::vector<u8>> https_get(std::string_view url, u64 max_bytes, const DownloadProgress& progress) {
    const std::wstring wurl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return make_error(ErrorCode::InvalidArgument, "not a valid URL: " + std::string(url));
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) return make_error(ErrorCode::InvalidArgument, "only https downloads are allowed");

    const std::wstring agent = widen("Bridger/" BRIDGER_VERSION_STRING);
    InternetHandle session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0)};
    if (session.h == nullptr) return (net_error("opening an internet session failed"));
    // Never follow a redirect from https to plain http (the default policy, made explicit).
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 30000);
    InternetHandle connection{WinHttpConnect(session.h, host, parts.nPort, 0)};
    if (connection.h == nullptr) return (net_error("connecting to the server failed"));
    InternetHandle request{WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE)};
    if (request.h == nullptr) return (net_error("creating the request failed"));
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return (net_error("the download failed; check the internet connection"));

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return make_error(ErrorCode::IoError, "the server answered HTTP " + std::to_string(status));
    u64 total = 0;
    wchar_t length[32] = {};
    DWORD length_size = sizeof(length);
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &length_size,
                            WINHTTP_NO_HEADER_INDEX))
        total = std::wcstoull(length, nullptr, 10);
    if (total > max_bytes) return make_error(ErrorCode::LimitExceeded, "the download is larger than expected");

    std::vector<u8> out;
    out.reserve(static_cast<usize>(total));
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return (net_error("the download was interrupted"));
        if (available == 0) break;
        if (out.size() + available > max_bytes) return make_error(ErrorCode::LimitExceeded, "the download is larger than expected");
        const usize at = out.size();
        out.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, out.data() + at, available, &read)) return (net_error("the download was interrupted"));
        out.resize(at + read);
        if (progress && !progress(out.size(), total)) return make_error(ErrorCode::Cancelled, "the download was cancelled");
    }
    if (total != 0 && out.size() != total) return make_error(ErrorCode::IoError, "the download ended early");
    return out;
}

Result<std::string> sha1_hex(std::span<const u8> data) {
    u8 digest[20] = {};
    const NTSTATUS st = BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
                                   digest, sizeof(digest));
    if (st < 0) return make_error(ErrorCode::IoError, "hashing failed");
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex(40, '0');
    for (usize i = 0; i < 20; ++i) {
        hex[i * 2] = kHex[digest[i] >> 4];
        hex[i * 2 + 1] = kHex[digest[i] & 15];
    }
    return hex;
}

} // namespace bridger::platform

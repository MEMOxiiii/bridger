// POSIX: HTTPS through the system's libcurl, loaded at run time (dlopen) so that no Bridger binary links against it:
// the command line never downloads anything, and a desktop without libcurl only loses the two downloads
// (docs/safety.md, "Network access"). Hashes come from the portable implementation (hash.hpp).
//
// The few libcurl declarations below are the stable public ABI of libcurl 7/8 (curl/curl.h, 8.5.0): option numbers
// are CURLOPTTYPE_* + n and have never changed meaning.

#include <bridger/platform/hash.hpp>
#include <bridger/platform/net.hpp>

#include <dlfcn.h>

#include <mutex>
#include <string>

namespace bridger::platform {
namespace {

using CURL = void;
using CurlCode = int;
using CurlOption = int;
using CurlInfo = int;
using CurlOffT = long long;

constexpr CurlOption kOptLong = 0, kOptObject = 10000, kOptFunction = 20000;
constexpr CurlOption kWriteData = kOptObject + 1, kUrl = kOptObject + 2, kErrorBuffer = kOptObject + 10, kWriteFunction = kOptFunction + 11,
                     kUserAgent = kOptObject + 18, kLowSpeedLimit = kOptLong + 19, kLowSpeedTime = kOptLong + 20,
                     kNoProgress = kOptLong + 43, kFollowLocation = kOptLong + 52, kXferInfoData = kOptObject + 57,
                     kMaxRedirs = kOptLong + 68, kConnectTimeout = kOptLong + 78, kNoSignal = kOptLong + 99,
                     kProtocols = kOptLong + 181, kRedirProtocols = kOptLong + 182, kXferInfoFunction = kOptFunction + 219,
                     kProtocolsStr = kOptObject + 318, kRedirProtocolsStr = kOptObject + 319;
constexpr CurlInfo kResponseCode = 0x200000 + 2;
constexpr long kProtoHttps = 1L << 1; // CURLPROTO_HTTPS
constexpr long kGlobalAll = 3;        // CURL_GLOBAL_ALL
constexpr CurlCode kOk = 0, kAbortedByCallback = 42;
constexpr usize kErrorSize = 256; // CURL_ERROR_SIZE

struct Curl {
    CurlCode (*global_init)(long) = nullptr;
    CURL* (*easy_init)() = nullptr;
    CurlCode (*easy_setopt)(CURL*, CurlOption, ...) = nullptr;
    CurlCode (*easy_perform)(CURL*) = nullptr;
    CurlCode (*easy_getinfo)(CURL*, CurlInfo, ...) = nullptr;
    void (*easy_cleanup)(CURL*) = nullptr;
    const char* (*easy_strerror)(CurlCode) = nullptr;
    bool ok = false;
};

template <typename F>
void bind(void* lib, const char* name, F& fn) {
    fn = reinterpret_cast<F>(dlsym(lib, name));
}

const Curl& curl() {
    static Curl c;
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef __APPLE__
        const char* const names[] = {"/usr/lib/libcurl.4.dylib", "libcurl.4.dylib"};
#else
        const char* const names[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
#endif
        void* lib = nullptr;
        for (const char* n : names)
            if ((lib = dlopen(n, RTLD_NOW | RTLD_LOCAL)) != nullptr) break;
        if (lib == nullptr) return;
        bind(lib, "curl_global_init", c.global_init);
        bind(lib, "curl_easy_init", c.easy_init);
        bind(lib, "curl_easy_setopt", c.easy_setopt);
        bind(lib, "curl_easy_perform", c.easy_perform);
        bind(lib, "curl_easy_getinfo", c.easy_getinfo);
        bind(lib, "curl_easy_cleanup", c.easy_cleanup);
        bind(lib, "curl_easy_strerror", c.easy_strerror);
        c.ok = c.global_init && c.easy_init && c.easy_setopt && c.easy_perform && c.easy_getinfo && c.easy_cleanup && c.easy_strerror &&
               c.global_init(kGlobalAll) == kOk;
    });
    return c;
}

struct Transfer {
    std::vector<u8> body;
    u64 max_bytes = 0;
    bool too_large = false;
    const DownloadProgress* progress = nullptr;
    bool cancelled = false;
};

usize on_write(const char* data, usize size, usize count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const usize n = size * count;
    if (t->body.size() + n > t->max_bytes) {
        t->too_large = true;
        return 0; // a short count makes curl stop with CURLE_WRITE_ERROR
    }
    t->body.insert(t->body.end(), data, data + n);
    return n;
}

int on_progress(void* user, CurlOffT total, CurlOffT now, CurlOffT, CurlOffT) {
    auto* t = static_cast<Transfer*>(user);
    if (t->progress != nullptr && *t->progress && !(*t->progress)(static_cast<u64>(now < 0 ? 0 : now), static_cast<u64>(total < 0 ? 0 : total))) {
        t->cancelled = true;
        return 1;
    }
    return 0;
}

} // namespace

Result<std::vector<u8>> https_get(std::string_view url, u64 max_bytes, const DownloadProgress& progress) {
    if (!url.starts_with("https://")) return make_error(ErrorCode::InvalidArgument, "only https downloads are allowed");
    const Curl& c = curl();
    if (!c.ok) return make_error(ErrorCode::NotImplemented, "downloading needs libcurl, which was not found on this system");
    CURL* h = c.easy_init();
    if (h == nullptr) return make_error(ErrorCode::IoError, "could not start a download (curl_easy_init)");
    struct Cleanup {
        const Curl& c;
        CURL* h;
        ~Cleanup() { c.easy_cleanup(h); }
    } cleanup{c, h};

    Transfer t;
    t.max_bytes = max_bytes;
    t.progress = &progress;
    char error[kErrorSize] = {};
    const std::string u(url);
    const std::string agent = "Bridger/" BRIDGER_VERSION_STRING;
    c.easy_setopt(h, kUrl, u.c_str());
    c.easy_setopt(h, kUserAgent, agent.c_str());
    c.easy_setopt(h, kErrorBuffer, error);
    // https only, for the first request and every redirect (never down to plain http).
    if (c.easy_setopt(h, kProtocolsStr, "https") != kOk) c.easy_setopt(h, kProtocols, kProtoHttps); // libcurl before 7.85
    if (c.easy_setopt(h, kRedirProtocolsStr, "https") != kOk) c.easy_setopt(h, kRedirProtocols, kProtoHttps);
    c.easy_setopt(h, kFollowLocation, 1L);
    c.easy_setopt(h, kMaxRedirs, 10L);
    c.easy_setopt(h, kNoSignal, 1L);
    c.easy_setopt(h, kConnectTimeout, 15L);
    c.easy_setopt(h, kLowSpeedLimit, 1L); // abort a transfer that stalls for 30 s, like the WinHTTP timeouts
    c.easy_setopt(h, kLowSpeedTime, 30L);
    c.easy_setopt(h, kWriteFunction, &on_write);
    c.easy_setopt(h, kWriteData, &t);
    c.easy_setopt(h, kXferInfoFunction, &on_progress);
    c.easy_setopt(h, kXferInfoData, &t);
    c.easy_setopt(h, kNoProgress, 0L);
    const CurlCode rc = c.easy_perform(h);
    if (t.cancelled || rc == kAbortedByCallback) return make_error(ErrorCode::Cancelled, "the download was cancelled");
    if (t.too_large) return make_error(ErrorCode::LimitExceeded, "the download is larger than " + std::to_string(max_bytes) + " bytes");
    if (rc != kOk) return make_error(ErrorCode::IoError, "download failed: " + std::string(error[0] != '\0' ? error : c.easy_strerror(rc)));
    long status = 0;
    c.easy_getinfo(h, kResponseCode, &status);
    if (status != 200) return make_error(ErrorCode::IoError, "the server answered HTTP " + std::to_string(status));
    return std::move(t.body);
}

Result<std::string> sha1_hex(std::span<const u8> data) { return to_hex(sha1_digest(data)); }

Result<std::string> sha256_hex(std::span<const u8> data) { return to_hex(sha256_digest(data)); }

} // namespace bridger::platform

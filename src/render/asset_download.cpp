#include <bridger/render/asset_download.hpp>

#include <bridger/io/zip_archive.hpp>
#include <bridger/platform/atomic_replace.hpp>
#include <bridger/platform/net.hpp>
#include <bridger/platform/paths.hpp>

#include <nlohmann/json.hpp>

#include <array>

namespace bridger::render {
namespace {

using json = nlohmann::json;

constexpr u64 kMaxManifest = 8ull << 20;  // the manifest is ~0.5 MB
constexpr u64 kMaxVersionFile = 4ull << 20;
constexpr u64 kMaxJar = 256ull << 20;     // the 26.3 client jar is 41.5 MB

Result<RemoteFile> remote_file(const json& j, std::string_view what) {
    if (!j.is_object() || !j.contains("url") || !j["url"].is_string() || !j.contains("sha1") || !j["sha1"].is_string())
        return make_error(ErrorCode::Malformed, std::string(what) + ": no url/sha1");
    RemoteFile f;
    f.url = j["url"].get<std::string>();
    f.sha1 = j["sha1"].get<std::string>();
    if (j.contains("size") && j["size"].is_number_unsigned()) f.size = j["size"].get<u64>();
    if (f.sha1.size() != 40 || f.sha1.find_first_not_of("0123456789abcdef") != std::string::npos)
        return make_error(ErrorCode::Malformed, std::string(what) + ": bad sha1");
    if (!is_mojang_download_url(f.url)) return make_error(ErrorCode::Malformed, std::string(what) + ": not a Mojang download URL");
    return f;
}

Result<std::vector<u8>> fetch_verified(const RemoteFile& f, u64 max_bytes, const platform::DownloadProgress& progress) {
    if (!is_mojang_download_url(f.url)) return make_error(ErrorCode::InvalidArgument, "refusing to download from " + f.url);
    BRIDGER_TRY(std::vector<u8> bytes, platform::https_get(f.url, max_bytes, progress));
    if (f.size != 0 && bytes.size() != f.size) return make_error(ErrorCode::ValidationFailed, "the download has the wrong size");
    BRIDGER_TRY(const std::string sha1, platform::sha1_hex(bytes));
    if (sha1 != f.sha1) return make_error(ErrorCode::ValidationFailed, "the download does not match Mojang's published SHA-1");
    return bytes;
}

} // namespace

bool is_mojang_download_url(std::string_view url) noexcept {
    constexpr std::string_view kPrefix = "https://";
    if (!url.starts_with(kPrefix)) return false;
    const std::string_view rest = url.substr(kPrefix.size());
    const std::string_view host = rest.substr(0, rest.find_first_of("/:?#"));
    constexpr std::array<std::string_view, 4> kHosts{"piston-meta.mojang.com", "piston-data.mojang.com", "launchermeta.mojang.com",
                                                     "launcher.mojang.com"};
    for (const std::string_view h : kHosts)
        if (host == h && (rest.size() == host.size() || rest[host.size()] == '/')) return true;
    return false;
}

Result<RemoteFile> find_version_in_manifest(std::string_view manifest_json, std::string_view version) {
    const json m = json::parse(manifest_json, nullptr, false);
    if (!m.is_object() || !m.contains("versions") || !m["versions"].is_array())
        return make_error(ErrorCode::Malformed, "Mojang's version manifest could not be read");
    for (const json& v : m["versions"])
        if (v.is_object() && v.contains("id") && v["id"].is_string() && v["id"].get<std::string>() == version)
            return remote_file(v, "version manifest entry");
    return make_error(ErrorCode::FileNotFound, "Minecraft Java " + std::string(version) + " is not in Mojang's version manifest");
}

Result<RemoteFile> client_download_of(std::string_view version_json) {
    const json v = json::parse(version_json, nullptr, false);
    if (!v.is_object() || !v.contains("downloads") || !v["downloads"].is_object() || !v["downloads"].contains("client"))
        return make_error(ErrorCode::Malformed, "the version file has no client download");
    return remote_file(v["downloads"]["client"], "client download");
}

std::optional<std::filesystem::path> downloaded_client_jar_path(std::string_view version) {
    const auto base = platform::roaming_app_data_dir();
    auto v = platform::path_from_utf8(version);
    if (!base || !v) return std::nullopt;
    return *base / "Bridger" / "assets" / *v / "client.jar";
}

Result<std::filesystem::path> download_client_jar(std::string_view version, const JarDownloadProgress& progress) {
    const auto dest = downloaded_client_jar_path(version);
    if (!dest) return make_error(ErrorCode::IoError, "the application data folder is not available");
    auto report = [&](float f, std::string_view step) { return !progress || progress(f, step); };
    if (!report(0.0f, "Asking Mojang for the version list")) return make_error(ErrorCode::Cancelled, "the download was cancelled");
    const RemoteFile manifest{std::string(kMojangVersionManifest), {}, 0};
    BRIDGER_TRY(const std::vector<u8> manifest_bytes, platform::https_get(manifest.url, kMaxManifest));
    BRIDGER_TRY(const RemoteFile version_file,
                find_version_in_manifest(std::string_view(reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size()), version));
    if (!report(0.03f, "Reading the version details")) return make_error(ErrorCode::Cancelled, "the download was cancelled");
    BRIDGER_TRY(const std::vector<u8> version_bytes, fetch_verified(version_file, kMaxVersionFile, {}));
    BRIDGER_TRY(const RemoteFile jar,
                client_download_of(std::string_view(reinterpret_cast<const char*>(version_bytes.data()), version_bytes.size())));
    BRIDGER_TRY(const std::vector<u8> jar_bytes, fetch_verified(jar, kMaxJar, [&](u64 got, u64 total) {
                    const u64 all = total != 0 ? total : jar.size;
                    return report(0.05f + 0.9f * (all != 0 ? static_cast<float>(got) / static_cast<float>(all) : 0.0f), "Downloading");
                }));
    if (!report(0.96f, "Checking and saving")) return make_error(ErrorCode::Cancelled, "the download was cancelled");
    std::error_code ec;
    std::filesystem::create_directories(dest->parent_path(), ec);
    // The saved file must open as a client jar with block models before it replaces anything.
    auto validate = [](const std::filesystem::path& written) -> Status {
        BRIDGER_TRY(io::ZipReader zip, io::ZipReader::open(written));
        BRIDGER_TRY(const auto stone, zip.read("assets/minecraft/blockstates/stone.json"));
        if (!stone) return make_error(ErrorCode::ValidationFailed, "the downloaded jar has no block models");
        return {};
    };
    BRIDGER_TRY_STATUS(platform::write_file_atomically(*dest, jar_bytes, validate));
    report(1.0f, "Done");
    return *dest;
}

} // namespace bridger::render

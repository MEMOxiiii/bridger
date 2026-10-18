#include "updater.hpp"

#include <bridger/io/zip_archive.hpp>
#include <bridger/platform/net.hpp>
#include <bridger/platform/paths.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <vector>

namespace bridger::app::update {
namespace fs = std::filesystem;
namespace {

constexpr u64 kMaxApiBytes = u64{2} << 20;    // the release JSON
constexpr u64 kMaxZipBytes = u64{512} << 20;  // the portable zip (1.0.0: about 25 MB)
constexpr u64 kMaxShaBytes = 4096;            // the .sha256 file
constexpr std::string_view kOldList = ".bridger-update-old.txt";

bool is_hex64(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Only this repository's release downloads are fetched (GitHub then redirects to its download host).
bool our_download(std::string_view url) {
    const std::string prefix = "https://github.com/" + std::string(kRepository) + "/releases/download/";
    return url.starts_with(prefix) && url.find("..") == std::string_view::npos;
}

} // namespace

std::optional<Version> Version::parse(std::string_view text) {
    if (text.starts_with('v') || text.starts_with('V')) text.remove_prefix(1);
    Version v;
    i32* parts[3] = {&v.major, &v.minor, &v.patch};
    for (int i = 0; i < 3; ++i) {
        const char* b = text.data();
        const char* e = text.data() + text.size();
        const auto [p, ec] = std::from_chars(b, e, *parts[i]);
        if (ec != std::errc() || *parts[i] < 0) return std::nullopt;
        text.remove_prefix(static_cast<usize>(p - b));
        if (i < 2) {
            if (!text.starts_with('.')) return std::nullopt;
            text.remove_prefix(1);
        }
    }
    if (!text.empty()) return std::nullopt; // "1.2.3-beta" and the like are not releases
    return v;
}

std::string Version::str() const { return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch); }

Result<Release> parse_release(std::string_view json) {
    const auto j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object()) return make_error(ErrorCode::Malformed, "the release information is not valid JSON");
    if (j.value("draft", false) || j.value("prerelease", false)) return make_error(ErrorCode::Malformed, "the latest release is not final");
    Release r;
    r.tag = j.value("tag_name", std::string());
    const auto v = Version::parse(r.tag);
    if (!v) return make_error(ErrorCode::Malformed, "the release tag '" + r.tag + "' is not a version");
    r.version = *v;
    r.page_url = j.value("html_url", std::string());
    r.notes = j.value("body", std::string());
    const std::string zip = "bridger-" + r.version.str() + std::string(kAssetSuffix);
    if (const auto assets = j.find("assets"); assets != j.end() && assets->is_array()) {
        for (const auto& a : *assets) {
            if (!a.is_object()) continue;
            const std::string name = a.value("name", std::string());
            const std::string url = a.value("browser_download_url", std::string());
            if (name == zip && our_download(url)) {
                r.zip_name = name;
                r.zip_url = url;
                r.zip_size = a.value("size", u64{0});
                // GitHub reports "digest": "sha256:<hex>" for release assets.
                const std::string digest = lower(a.value("digest", std::string()));
                if (digest.starts_with("sha256:") && is_hex64(std::string_view(digest).substr(7))) r.sha256 = digest.substr(7);
            } else if (name == zip + ".sha256" && our_download(url)) {
                r.sha256_url = url;
            }
        }
    }
    if (r.zip_url.empty()) return make_error(ErrorCode::Malformed, "release " + r.tag + " has no " + zip);
    return r;
}

std::optional<std::string> parse_sha256_file(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\xEF' || text.front() == '\xBB' ||
                             text.front() == '\xBF'))
        text.remove_prefix(1); // spaces, or a UTF-8 byte order mark
    if (text.size() < 64) return std::nullopt;
    std::string hex = lower(std::string(text.substr(0, 64)));
    if (!is_hex64(hex)) return std::nullopt;
    if (text.size() > 64 && text[64] != ' ' && text[64] != '\t' && text[64] != '\r' && text[64] != '\n') return std::nullopt;
    return hex;
}

Result<Release> fetch_latest() {
    BRIDGER_TRY(const std::vector<u8> body, platform::https_get(kLatestReleaseApi, kMaxApiBytes));
    return parse_release(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));
}

Result<fs::path> extract_package(std::span<const u8> zip, const fs::path& staging) {
    io::ZipLimits limits;
    limits.max_entries = 2000;
    limits.max_total_bytes = u64{1} << 30;
    BRIDGER_TRY(const std::vector<io::ZipEntry> entries, io::read_zip(zip, limits));
    std::string top;
    for (const io::ZipEntry& e : entries) {
        const std::string_view name = e.name;
        if (name.empty() || name.front() == '/' || name.front() == '\\' || name.find(':') != std::string_view::npos ||
            name.find('\\') != std::string_view::npos)
            return make_error(ErrorCode::Malformed, "update package: unsafe file name '" + e.name + "'");
        usize start = 0;
        while (start <= name.size()) {
            const usize end = std::min(name.find('/', start), name.size());
            const std::string_view part = name.substr(start, end - start);
            if (part == ".." || part == ".") return make_error(ErrorCode::Malformed, "update package: unsafe file name '" + e.name + "'");
            start = end + 1;
        }
        const std::string first(name.substr(0, name.find('/')));
        if (top.empty()) top = first;
        if (first != top || name.find('/') == std::string_view::npos)
            return make_error(ErrorCode::Malformed, "update package: files outside its folder");
    }
    if (top.empty()) return make_error(ErrorCode::Malformed, "update package: empty");
    std::error_code ec;
    if (fs::exists(staging, ec) && !fs::is_empty(staging, ec)) return make_error(ErrorCode::AlreadyExists, "update staging folder is not empty");
    fs::create_directories(staging, ec);
    for (const io::ZipEntry& e : entries) {
        if (e.name.ends_with('/')) continue; // a folder entry
        BRIDGER_TRY(const fs::path rel, platform::path_from_utf8(e.name));
        const fs::path out = staging / rel;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(e.data.data()), static_cast<std::streamsize>(e.data.size()));
        if (!f) return make_error(ErrorCode::IoError, "update package: could not write " + e.name);
    }
    BRIDGER_TRY(const fs::path root_rel, platform::path_from_utf8(top));
    const fs::path root = staging / root_rel;
    if (!fs::is_regular_file(root / "bridger-gui.exe", ec)) return make_error(ErrorCode::Malformed, "update package: bridger-gui.exe is missing");
    return root;
}

Result<fs::path> download_and_stage(const Release& release, const fs::path& staging, const Progress& progress) {
    if (!our_download(release.zip_url)) return make_error(ErrorCode::InvalidArgument, "refusing to download from outside the project's releases");
    std::string expected = release.sha256;
    if (expected.empty() && !release.sha256_url.empty()) {
        BRIDGER_TRY(const std::vector<u8> sha, platform::https_get(release.sha256_url, kMaxShaBytes));
        const auto parsed = parse_sha256_file(std::string_view(reinterpret_cast<const char*>(sha.data()), sha.size()));
        if (!parsed) return make_error(ErrorCode::Malformed, "the release's checksum file is not readable");
        expected = *parsed;
    }
    if (expected.empty()) return make_error(ErrorCode::Malformed, "the release has no checksum, so it cannot be installed safely");
    const u64 total = release.zip_size;
    BRIDGER_TRY(const std::vector<u8> zip, platform::https_get(release.zip_url, kMaxZipBytes, [&](u64 got, u64 size) {
                    const u64 t = size ? size : total;
                    return !progress || progress(t ? 0.9f * static_cast<float>(got) / static_cast<float>(t) : 0.0f, "Downloading");
                }));
    if (progress && !progress(0.92f, "Checking")) return make_error(ErrorCode::Cancelled, "cancelled");
    BRIDGER_TRY(const std::string actual, platform::sha256_hex(zip));
    if (actual != expected)
        return make_error(ErrorCode::ValidationFailed, "the download's SHA-256 does not match the release (" + actual + ")");
    if (progress && !progress(0.95f, "Unpacking")) return make_error(ErrorCode::Cancelled, "cancelled");
    return extract_package(zip, staging);
}

Status install(const fs::path& package, const fs::path& app_dir) {
    struct Moved {
        fs::path target;
        bool had_old;
    };
    std::vector<Moved> moved;
    std::error_code ec;
    const auto rollback = [&] {
        std::error_code e;
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            fs::remove(it->target, e);
            if (it->had_old) fs::rename(fs::path(it->target).concat(".old"), it->target, e);
        }
    };
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(package, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file()) files.push_back(fs::relative(it->path(), package));
    if (ec) return make_error(ErrorCode::IoError, "update: cannot list the package: " + ec.message());
    std::sort(files.begin(), files.end());
    for (const fs::path& rel : files) {
        const fs::path target = app_dir / rel;
        const fs::path old = fs::path(target).concat(".old");
        fs::create_directories(target.parent_path(), ec);
        const bool had = fs::exists(target, ec);
        if (had) {
            fs::remove(old, ec); // a leftover from an earlier update
            fs::rename(target, old, ec);
            if (ec) {
                rollback();
                return make_error(ErrorCode::IoError, "update: cannot move " + platform::path_to_utf8(rel).value_or("a file") + " aside (" +
                                                          ec.message() + "); nothing was changed");
            }
        }
        fs::rename(package / rel, target, ec);
        if (ec) {
            if (had) {
                std::error_code e;
                fs::rename(old, target, e);
            }
            rollback();
            return make_error(ErrorCode::IoError, "update: cannot install " + platform::path_to_utf8(rel).value_or("a file") + " (" +
                                                      ec.message() + "); nothing was changed");
        }
        moved.push_back({target, had});
    }
    // The files renamed to ".old", for cleanup() after the restart (it deletes only these).
    std::ofstream list(app_dir / kOldList, std::ios::binary | std::ios::trunc);
    for (const Moved& m : moved)
        if (m.had_old) list << platform::path_to_utf8(fs::relative(m.target, app_dir)).value_or("") << '\n';
    return {};
}

void cleanup(const fs::path& app_dir) {
    std::error_code ec;
    if (std::ifstream list(app_dir / kOldList, std::ios::binary); list) {
        std::string line;
        while (std::getline(list, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.find("..") != std::string::npos) continue;
            const auto rel = platform::path_from_utf8(line + ".old");
            if (rel && rel->is_relative()) fs::remove(app_dir / *rel, ec);
        }
    }
    fs::remove(app_dir / kOldList, ec);
    fs::remove_all(staging_dir(app_dir), ec);
}

fs::path staging_dir(const fs::path& app_dir) { return app_dir / ".bridger-update"; }

} // namespace bridger::app::update

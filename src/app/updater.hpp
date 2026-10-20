#pragma once

// The desktop app's self-update (docs/safety.md, "Network access"; docs/app/updates.md): checks the project's latest
// GitHub release, downloads its portable zip only when the user agrees, verifies the zip's SHA-256, extracts it next
// to the program and swaps the program's files with rollback. OS-free apart from platform/net and platform/process;
// unit-tested from tests/unit/app/updater_test.cpp.

#include <bridger/core/result.hpp>

#include <compare>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bridger::app::update {

inline constexpr std::string_view kRepository = "MEMOxiiii/bridger";
inline constexpr std::string_view kLatestReleaseApi = "https://api.github.com/repos/MEMOxiiii/bridger/releases/latest";
// The release file for this system (release.yml). Only the Windows zip installs itself; on Linux and macOS the app
// offers the release page instead (docs/adr/0010).
#if defined(_WIN32)
inline constexpr std::string_view kAssetSuffix = "-windows-x64.zip";
inline constexpr bool kSelfInstall = true;
#elif defined(__APPLE__)
inline constexpr std::string_view kAssetSuffix = "-macos-arm64.tar.gz";
inline constexpr bool kSelfInstall = false;
#else
inline constexpr std::string_view kAssetSuffix = "-linux-x64.tar.gz";
inline constexpr bool kSelfInstall = false;
#endif

struct Version {
    i32 major = 0, minor = 0, patch = 0;
    [[nodiscard]] static std::optional<Version> parse(std::string_view text); // "1.2.3" or "v1.2.3"
    [[nodiscard]] std::string str() const;
    auto operator<=>(const Version&) const = default;
};

struct Release {
    Version version;
    std::string tag;       // "v1.1.0"
    std::string page_url;  // the release page
    std::string notes;     // release notes (Markdown)
    std::string zip_name;  // "bridger-1.1.0-windows-x64.zip"
    std::string zip_url;   // https://github.com/<repo>/releases/download/<tag>/<zip>
    u64 zip_size = 0;
    std::string sha256;     // lowercase hex from the API's asset "digest"; empty if GitHub gave none
    std::string sha256_url; // the "<zip>.sha256" asset published with the release, if any
};

// The GitHub API's latest-release JSON -> Release. Draft and pre-releases are refused; the zip must be an asset of
// this repository's release (its download URL is checked).
[[nodiscard]] Result<Release> parse_release(std::string_view json);

// "<64 hex digits>  <file name>" (sha256sum format) -> the lowercase digest; nullopt when malformed.
[[nodiscard]] std::optional<std::string> parse_sha256_file(std::string_view text);

using Progress = std::function<bool(float fraction, std::string_view step)>; // false cancels

// Asks GitHub for the latest release.
[[nodiscard]] Result<Release> fetch_latest();

// Downloads the release zip, verifies its SHA-256 (the API digest, else the .sha256 asset; neither: refused) and
// extracts it into `staging` (created; must be empty or absent). Returns the folder holding the program files.
[[nodiscard]] Result<std::filesystem::path> download_and_stage(const Release& release, const std::filesystem::path& staging,
                                                               const Progress& progress);

// Extracts zip bytes into `staging` after checking every name (no absolute paths, no "..", one top-level folder that
// holds bridger-gui.exe). Returns that folder. Exposed for tests.
[[nodiscard]] Result<std::filesystem::path> extract_package(std::span<const u8> zip, const std::filesystem::path& staging);

// Moves every file of `package` into `app_dir`: an existing file is first renamed to "<name>.old" (Windows allows
// renaming a running program), then the new one moved in. Any failure puts back everything already moved.
[[nodiscard]] Status install(const std::filesystem::path& package, const std::filesystem::path& app_dir);

// After a restart: removes the ".old" files install() made (it records them in .bridger-update-old.txt; nothing else is
// deleted) and the staging folder. Best effort.
void cleanup(const std::filesystem::path& app_dir);

// The staging folder for `app_dir` (next to the program, so that the final moves stay on one volume).
[[nodiscard]] std::filesystem::path staging_dir(const std::filesystem::path& app_dir);

} // namespace bridger::app::update

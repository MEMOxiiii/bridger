#pragma once

// Downloading the Minecraft Java client jar from Mojang's servers, only when the user asks for it
// (docs/safety.md, "Network access"; docs/formats/assets.md). The jar holds the textures and models
// the 3D preview draws with; it is read as a zip and never executed. Every step is checked: only
// Mojang's hosts over https, the version file against the manifest's SHA-1, the jar against the
// version file's SHA-1 and size, and the saved jar must open as a client jar with block models.

#include <bridger/core/result.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace bridger::render {

inline constexpr std::string_view kMojangVersionManifest = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

// Where a downloaded jar is kept: %APPDATA%/Bridger/assets/<version>/client.jar (never .minecraft).
[[nodiscard]] std::optional<std::filesystem::path> downloaded_client_jar_path(std::string_view version);

// fraction 0..1 and a short step name; return false to cancel.
using JarDownloadProgress = std::function<bool(float fraction, std::string_view step)>;

// Downloads, verifies and saves the client jar of `version` (atomically, at downloaded_client_jar_path).
[[nodiscard]] Result<std::filesystem::path> download_client_jar(std::string_view version, const JarDownloadProgress& progress = {});

// Pieces of the above, exposed for tests (no network).
struct RemoteFile {
    std::string url;
    std::string sha1;
    u64 size = 0; // 0 = not given
};
// https and one of Mojang's download hosts.
[[nodiscard]] bool is_mojang_download_url(std::string_view url) noexcept;
// The version's entry in version_manifest_v2.json.
[[nodiscard]] Result<RemoteFile> find_version_in_manifest(std::string_view manifest_json, std::string_view version);
// downloads.client of a version file.
[[nodiscard]] Result<RemoteFile> client_download_of(std::string_view version_json);

} // namespace bridger::render

#pragma once

// Names of game versions, for showing which version saved a file and which one a conversion targets
// (docs/versions.md, "Version names").

#include <bridger/core/types.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bridger {

struct JavaRelease {
    std::string_view name; // "1.20.1"
    i32 data_version;      // world DataVersion of that release
};

// Every Java release since 1.9 (the first with a DataVersion), oldest first.
[[nodiscard]] std::span<const JavaRelease> java_releases() noexcept;
[[nodiscard]] std::optional<JavaRelease> find_java_release(std::string_view name) noexcept;

// "1.20.1"; between two releases "a snapshot between 1.20.1 and 1.20.2"; above the newest known
// "newer than 26.3"; 0 or below 169 "before 1.9".
[[nodiscard]] std::string java_version_name(i32 data_version);

// Bedrock versions are written as five numbers ([1, 21, 50, 0, 0] in level.dat); the game calls the
// year-numbered ones (minor 26 and above) "26.50" and the older ones "1.21.50".
[[nodiscard]] std::string bedrock_version_name(std::span<const i32> parts);

} // namespace bridger

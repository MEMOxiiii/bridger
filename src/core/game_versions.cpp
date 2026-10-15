#include <bridger/core/game_versions.hpp>

#include <algorithm>
#include <array>
#include <format>

namespace bridger {
namespace {

// Mojang's version manifest (release ids) joined with each release's DataVersion from PrismarineJS
// minecraft-data `pc/common/protocolVersions.json` (MIT); checked 2026-10-15.
constexpr JavaRelease kReleases[] = {
    {"1.9", 169},
    {"1.9.1", 175},
    {"1.9.2", 176},
    {"1.9.3", 183},
    {"1.9.4", 184},
    {"1.10", 510},
    {"1.10.1", 511},
    {"1.10.2", 512},
    {"1.11", 819},
    {"1.11.1", 921},
    {"1.11.2", 922},
    {"1.12", 1139},
    {"1.12.1", 1241},
    {"1.12.2", 1343},
    {"1.13", 1519},
    {"1.13.1", 1628},
    {"1.13.2", 1631},
    {"1.14", 1952},
    {"1.14.1", 1957},
    {"1.14.2", 1963},
    {"1.14.3", 1968},
    {"1.14.4", 1976},
    {"1.15", 2225},
    {"1.15.1", 2227},
    {"1.15.2", 2230},
    {"1.16", 2566},
    {"1.16.1", 2567},
    {"1.16.2", 2578},
    {"1.16.3", 2580},
    {"1.16.4", 2584},
    {"1.16.5", 2586},
    {"1.17", 2724},
    {"1.17.1", 2730},
    {"1.18", 2860},
    {"1.18.1", 2865},
    {"1.18.2", 2975},
    {"1.19", 3105},
    {"1.19.1", 3117},
    {"1.19.2", 3120},
    {"1.19.3", 3218},
    {"1.19.4", 3337},
    {"1.20", 3463},
    {"1.20.1", 3465},
    {"1.20.2", 3578},
    {"1.20.3", 3698},
    {"1.20.4", 3700},
    {"1.20.5", 3837},
    {"1.20.6", 3839},
    {"1.21", 3953},
    {"1.21.1", 3955},
    {"1.21.2", 4080},
    {"1.21.3", 4082},
    {"1.21.4", 4189},
    {"1.21.5", 4325},
    {"1.21.6", 4435},
    {"1.21.7", 4438},
    {"1.21.8", 4440},
    {"1.21.9", 4554},
    {"1.21.10", 4556},
    {"1.21.11", 4671},
    {"26.1", 4786},
    {"26.1.1", 4788},
    {"26.1.2", 4790},
    {"26.2", 4903},
    {"26.3", 5023},
};

} // namespace

std::span<const JavaRelease> java_releases() noexcept { return kReleases; }

std::optional<JavaRelease> find_java_release(std::string_view name) noexcept {
    for (const JavaRelease& r : kReleases)
        if (r.name == name) return r;
    return std::nullopt;
}

std::string java_version_name(i32 data_version) {
    if (data_version < kReleases[0].data_version) return "before 1.9";
    const JavaRelease* exact = nullptr;
    const JavaRelease* below = nullptr;
    for (const JavaRelease& r : kReleases) {
        if (r.data_version == data_version) exact = &r; // the last release with it (e.g. 1.9.4 over 1.9.3)
        if (r.data_version < data_version) below = &r;
    }
    if (exact != nullptr) return std::string(exact->name);
    const JavaRelease& newest = kReleases[std::size(kReleases) - 1];
    if (data_version > newest.data_version) return "newer than " + std::string(newest.name);
    const auto above = std::find_if(std::begin(kReleases), std::end(kReleases), [&](const JavaRelease& r) { return r.data_version > data_version; });
    return std::format("a snapshot between {} and {}", below->name, above->name);
}

std::string bedrock_version_name(std::span<const i32> parts) {
    if (parts.size() < 3) return "unknown";
    if (parts[0] == 1 && parts[1] >= 26) return std::format("{}.{}", parts[1], parts[2]);
    return std::format("{}.{}.{}", parts[0], parts[1], parts[2]);
}

} // namespace bridger

#pragma once

// User settings of the desktop app, stored as JSON in %APPDATA%/Bridger/settings.json.
// A missing or unreadable file gives the defaults (it is rewritten on the next change).

#include <bridger/core/types.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bridger::app {

struct Settings {
    std::string client_jar;          // UTF-8 path; empty = the launcher's jar of the pinned version
    std::vector<std::string> packs;  // resource packs (zip or folder), lowest priority first
    int render_distance = 12;        // chunks around the camera (worlds)
    int distant_terrain = 32;        // simplified terrain out to this many chunks; <= render_distance: off
    std::vector<std::string> recent; // most recent first

    static constexpr usize kMaxRecent = 6;
    void add_recent(const std::string& path);
};

[[nodiscard]] std::optional<std::filesystem::path> settings_path();
[[nodiscard]] Settings load_settings();
void save_settings(const Settings& settings);

} // namespace bridger::app

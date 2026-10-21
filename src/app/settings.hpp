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
    bool check_updates = true;       // look for a new release once a day (docs/app/updates.md)
    i64 last_update_check = 0;       // Unix seconds of the last automatic check
    std::string skipped_version;     // "1.1.0": not offered again automatically
    // Advanced convert options, remembered between conversions (the convert page's "Advanced" section).
    std::string convert_entities = "decorations"; // decorations | all | none (ConvertOptions::entities)
    bool convert_player = true;                   // the single-player player of a world
    std::string convert_fallback = "air";         // air | marker: blocks with no counterpart

    static constexpr usize kMaxRecent = 6;
    void add_recent(const std::string& path);
};

[[nodiscard]] std::optional<std::filesystem::path> settings_path();
[[nodiscard]] Settings load_settings();
void save_settings(const Settings& settings);

} // namespace bridger::app

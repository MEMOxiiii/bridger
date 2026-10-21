#include "settings.hpp"

#include <bridger/core/log.hpp>
#include <bridger/platform/paths.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace bridger::app {

void Settings::add_recent(const std::string& path) {
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
}

std::optional<std::filesystem::path> settings_path() {
    const auto dir = platform::roaming_app_data_dir();
    if (!dir) return std::nullopt;
    return *dir / "Bridger" / "settings.json";
}

Settings load_settings() {
    Settings s;
    const auto path = settings_path();
    if (!path) return s;
    std::ifstream in(*path, std::ios::binary);
    if (!in) return s;
    std::stringstream text;
    text << in.rdbuf();
    const auto j = nlohmann::json::parse(text.str(), nullptr, false);
    if (!j.is_object()) {
        log::warn("settings: {} is not valid JSON; using the defaults", platform::path_to_utf8(*path).value_or("settings.json"));
        return s;
    }
    if (auto it = j.find("client_jar"); it != j.end() && it->is_string()) s.client_jar = it->get<std::string>();
    if (auto it = j.find("render_distance"); it != j.end() && it->is_number_integer()) s.render_distance = std::clamp(it->get<int>(), 2, 48);
    if (auto it = j.find("distant_terrain"); it != j.end() && it->is_number_integer()) s.distant_terrain = std::clamp(it->get<int>(), 0, 96);
    for (const char* key : {"packs", "recent"}) {
        auto& out = std::string_view(key) == "packs" ? s.packs : s.recent;
        if (auto it = j.find(key); it != j.end() && it->is_array())
            for (const auto& v : *it)
                if (v.is_string()) out.push_back(v.get<std::string>());
    }
    if (s.recent.size() > Settings::kMaxRecent) s.recent.resize(Settings::kMaxRecent);
    if (auto it = j.find("check_updates"); it != j.end() && it->is_boolean()) s.check_updates = it->get<bool>();
    if (auto it = j.find("last_update_check"); it != j.end() && it->is_number_integer()) s.last_update_check = it->get<i64>();
    if (auto it = j.find("skipped_version"); it != j.end() && it->is_string()) s.skipped_version = it->get<std::string>();
    if (auto it = j.find("convert_entities"); it != j.end() && it->is_string()) {
        const std::string v = it->get<std::string>();
        if (v == "decorations" || v == "all" || v == "none") s.convert_entities = v;
    }
    if (auto it = j.find("convert_player"); it != j.end() && it->is_boolean()) s.convert_player = it->get<bool>();
    if (auto it = j.find("convert_fallback"); it != j.end() && it->is_string()) {
        const std::string v = it->get<std::string>();
        if (v == "air" || v == "marker") s.convert_fallback = v;
    }
    return s;
}

void save_settings(const Settings& s) {
    const auto path = settings_path();
    if (!path) return;
    std::error_code ec;
    std::filesystem::create_directories(path->parent_path(), ec);
    const nlohmann::json j = {{"client_jar", s.client_jar}, {"packs", s.packs}, {"render_distance", s.render_distance},
                              {"distant_terrain", s.distant_terrain}, {"recent", s.recent}, {"check_updates", s.check_updates},
                              {"last_update_check", s.last_update_check}, {"skipped_version", s.skipped_version},
                              {"convert_entities", s.convert_entities}, {"convert_player", s.convert_player},
                              {"convert_fallback", s.convert_fallback}};
    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
    out << j.dump(2);
    if (!out) log::warn("settings: could not write {}", platform::path_to_utf8(*path).value_or("settings.json"));
}

} // namespace bridger::app

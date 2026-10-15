#include <bridger/convert/source_version.hpp>
#include <bridger/core/game_versions.hpp>

#include <array>
#include <format>

namespace bridger::convert {

GameVersionInfo java_version_info(formats::FormatId format, std::optional<i32> data_version, std::optional<std::string> version_name,
                                  std::optional<i32> storage_format) {
    GameVersionInfo v;
    v.edition = Edition::Java;
    if (format == formats::FormatId::LegacySchematic) {
        v.name = "before 1.13";
        v.detail = "MCEdit numeric block ids";
        return v;
    }
    if (version_name && !version_name->empty()) {
        v.name = *version_name; // as the game wrote it into level.dat
        v.exact = true;
        if (data_version) v.detail = std::format("DataVersion {}", *data_version);
        return v;
    }
    if (data_version && *data_version > 0) {
        v.name = java_version_name(*data_version);
        v.exact = find_java_release(v.name).has_value();
        v.detail = std::format("DataVersion {}", *data_version);
        return v;
    }
    if (storage_format == 19132) {
        v.name = "Beta 1.3 – 1.1";
        v.detail = "McRegion world";
    } else if (storage_format == 19133) {
        v.name = "1.2 – 1.8";
        v.detail = "Anvil world without a DataVersion";
    } else if (format == formats::FormatId::SpongeSchem) {
        v.detail = "Sponge v1 records no version (1.13 or newer)";
    } else {
        v.detail = "not recorded in the file";
    }
    return v;
}

GameVersionInfo bedrock_world_version_info(const std::vector<i32>& last_opened) {
    GameVersionInfo v;
    v.edition = Edition::Bedrock;
    if (last_opened.size() >= 3 && last_opened[0] > 0) {
        v.name = bedrock_version_name(last_opened);
        v.detail = "last opened with this version";
        v.exact = true;
    } else {
        v.name = "before 1.2";
        v.detail = "level.dat records no version";
    }
    return v;
}

GameVersionInfo bedrock_block_version_info(i32 block_version) {
    GameVersionInfo v;
    v.edition = Edition::Bedrock;
    if (block_version <= 0) {
        v.detail = "not recorded (block data before 1.13)";
        return v;
    }
    const auto u = static_cast<u32>(block_version);
    const std::array<i32, 3> parts{static_cast<i32>(u >> 24), static_cast<i32>((u >> 16) & 0xFF), static_cast<i32>((u >> 8) & 0xFF)};
    v.name = bedrock_version_name(parts) + " or newer";
    v.detail = "block data version (not every release changes it)";
    return v;
}

std::string describe(const GameVersionInfo& v) {
    const char* ed = v.edition == Edition::Java ? "Java" : "Bedrock";
    if (v.name.empty()) return std::format("{}: {}", ed, v.detail.empty() ? "version not recorded" : v.detail);
    return v.detail.empty() ? std::format("{} {}", ed, v.name) : std::format("{} {} ({})", ed, v.name, v.detail);
}

} // namespace bridger::convert

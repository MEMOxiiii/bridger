// `bridger info <file>`: describe a structure or world file.

#include "commands.hpp"

#include <bridger/core/log.hpp>
#include <bridger/formats/detect.hpp>
#include <bridger/formats/java/sponge_schem.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>

#include <chrono>
#include <cstdio>
#include <format>

namespace bridger::cli {
namespace {

void line(std::string_view label, std::string_view value) {
    std::puts(std::format("{:<16}{}", label, value).c_str());
}

u64 count_non_air(const Structure& s) {
    u64 n = 0;
    const Extent size = s.size();
    for (u32 cy = 0; cy < s.sections_y(); ++cy)
        for (u32 cz = 0; cz < s.sections_z(); ++cz)
            for (u32 cx = 0; cx < s.sections_x(); ++cx) {
                const Section& sec = s.section(cx, cy, cz);
                if (sec.blocks().is_uniform() && sec.blocks().palette()[0] == kAirStateId) continue;
                for (u32 y = 0; y < 16 && cy * 16 + y < size.y; ++y)
                    for (u32 z = 0; z < 16 && cz * 16 + z < size.z; ++z)
                        for (u32 x = 0; x < 16 && cx * 16 + x < size.x; ++x)
                            if (sec.block(static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)) != kAirStateId) ++n;
            }
    return n;
}

int info_schem(const std::string& name, std::span<const u8> bytes) {
    auto file = formats::java::read_sponge_schem(bytes, std::make_shared<BlockStateRegistry>(),
                                                 std::make_shared<BiomeRegistry>());
    if (!file) {
        log::error("{}", file.error().with_file(name).describe());
        return kExitError;
    }
    const formats::java::SchemInfo& i = file->info;
    const Structure& s = file->structure;

    line("File:", name);
    line("Format:", std::format("Sponge schematic v{} ({})", i.version, io::to_string(i.compression)));
    line("DataVersion:", i.data_version ? std::to_string(*i.data_version) : std::string("(none)"));
    line("Size:", std::format("{} x {} x {} ({} blocks)", i.width, i.height, i.length, s.size().volume()));
    line("Offset:", std::format("{}, {}, {}", i.offset[0], i.offset[1], i.offset[2]));
    line("Block palette:", std::format("{} states", i.palette_size));
    line("Non-air blocks:", std::to_string(count_non_air(s)));
    line("Block entities:", std::to_string(i.block_entity_count));
    line("Entities:", std::to_string(i.entity_count));
    line("Biomes:", i.biome_palette_size == 0
                        ? std::string("(none)")
                        : std::format("{} biomes, {}", i.biome_palette_size, i.biomes_3d ? "per block" : "per column"));
    if (!s.metadata.name.empty()) line("Name:", s.metadata.name);
    if (!s.metadata.author.empty()) line("Author:", s.metadata.author);
    if (s.metadata.date_ms) {
        const auto tp = std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(*s.metadata.date_ms));
        line("Date:", std::format("{:%Y-%m-%d %H:%M:%S} UTC", std::chrono::floor<std::chrono::seconds>(tp)));
    }
    if (!s.metadata.required_mods.empty()) {
        std::string mods;
        for (const auto& m : s.metadata.required_mods) mods += (mods.empty() ? "" : ", ") + m;
        line("Required mods:", mods);
    }
    for (const auto& w : i.warnings) log::warn("{}", w);
    return kExitOk;
}

} // namespace

int run_info(const std::string& input_utf8) {
    auto path = platform::path_from_utf8(input_utf8);
    if (!path) {
        log::error("{}", path.error().describe());
        return kExitError;
    }
    auto mapped = platform::MappedFile::open(*path);
    if (!mapped) {
        log::error("{}", mapped.error().describe());
        return kExitError;
    }
    const auto bytes = mapped->data();
    auto format = formats::detect_format(*path, bytes.first(std::min<usize>(bytes.size(), 16)));
    if (!format) {
        log::error("{}", format.error().with_file(input_utf8).describe());
        return kExitError;
    }
    switch (*format) {
    case formats::FormatId::SpongeSchem: return info_schem(input_utf8, bytes);
    default:
        log::error("'info' does not support {} files yet", formats::info(*format).name);
        return kExitNotImplemented;
    }
}

} // namespace bridger::cli

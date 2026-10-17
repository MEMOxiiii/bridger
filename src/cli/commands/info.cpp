// `bridger info <file>`: describe a structure or world file.

#include "commands.hpp"

#include <bridger/convert/source_version.hpp>

#include <bridger/convert/pipeline.hpp>
#include <bridger/core/log.hpp>
#include <bridger/formats/bedrock/mcstructure.hpp>
#include <bridger/formats/bedrock/mcworld/bedrock_world_reader.hpp>
#include <bridger/formats/detect.hpp>
#include <bridger/formats/java/anvil/java_world.hpp>
#include <bridger/formats/java/anvil/region_file.hpp>
#include <bridger/formats/java/legacy_schematic.hpp>
#include <bridger/formats/java/sponge_schem.hpp>
#include <bridger/formats/structure_file.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <format>
#include <random>

namespace bridger::cli {
namespace {

void line(std::string_view label, std::string_view value) {
    std::puts(std::format("{:<16}{}", label, value).c_str());
}

u64 count_non_air(const Structure& s, StateId air = kAirStateId) {
    u64 n = 0;
    const Extent size = s.size();
    for (u32 cy = 0; cy < s.sections_y(); ++cy)
        for (u32 cz = 0; cz < s.sections_z(); ++cz)
            for (u32 cx = 0; cx < s.sections_x(); ++cx) {
                const Section& sec = s.section(cx, cy, cz);
                if (sec.blocks().is_uniform() && sec.blocks().palette()[0] == air) continue;
                for (u32 y = 0; y < 16 && cy * 16 + y < size.y; ++y)
                    for (u32 z = 0; z < 16 && cz * 16 + z < size.z; ++z)
                        for (u32 x = 0; x < 16 && cx * 16 + x < size.x; ++x)
                            if (sec.block(static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)) != air) ++n;
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
    line("Saved by:", convert::describe(convert::java_version_info(formats::FormatId::SpongeSchem, i.data_version)));
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

const translate::MappingData* mapping_or_log() {
    auto m = translate::MappingData::embedded();
    if (!m) {
        log::error("{}", m.error().describe());
        return nullptr;
    }
    return *m;
}

void print_metadata(const Structure& s) {
    if (!s.metadata.name.empty()) line("Name:", s.metadata.name);
    if (!s.metadata.author.empty()) line("Author:", s.metadata.author);
    if (s.metadata.date_ms) {
        const auto tp = std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(*s.metadata.date_ms));
        line("Date:", std::format("{:%Y-%m-%d %H:%M:%S} UTC", std::chrono::floor<std::chrono::seconds>(tp)));
    }
}

// .nbt, .litematic, legacy .schematic.
int info_java_structure(const std::string& name, std::span<const u8> bytes, formats::FormatId format) {
    const translate::MappingData* m = mapping_or_log();
    if (m == nullptr) return kExitError;
    const formats::java::LegacyTables legacy{
        [m](u16 id, u8 data) -> std::optional<std::string> {
            if (const auto st = m->legacy_block(id, data)) return m->java_state_string(*st);
            return std::nullopt;
        },
        [m](u16 id, u16 damage) -> std::optional<std::string> {
            if (const auto i = m->legacy_item(id, damage)) return std::string(*i);
            return std::nullopt;
        }};
    auto file = formats::read_java_structure(bytes, format, std::make_shared<BlockStateRegistry>(),
                                             std::make_shared<BiomeRegistry>(), &legacy);
    if (!file) {
        log::error("{}", file.error().with_file(name).describe());
        return kExitError;
    }
    const Structure& s = file->structure;
    line("File:", name);
    line("Format:", std::string(formats::info(format).description));
    line("Saved by:", convert::describe(convert::java_version_info(format, file->data_version)));
    line("Size:", std::format("{} x {} x {} ({} blocks)", s.size().x, s.size().y, s.size().z, s.size().volume()));
    line("Block states:", std::format("{} distinct", s.block_registry().size()));
    line("Non-air blocks:", std::to_string(count_non_air(s)));
    line("Block entities:", std::to_string(s.block_entities.size()));
    line("Entities:", std::to_string(s.entities.size()));
    print_metadata(s);
    for (const auto& w : file->warnings) log::warn("{}", w);
    return kExitOk;
}

int info_mcstructure(const std::string& name, std::span<const u8> bytes) {
    const translate::MappingData* m = mapping_or_log();
    if (m == nullptr) return kExitError;
    const u32 air = m->find_bedrock("minecraft:air", {}).value_or(0);
    auto file = formats::bedrock::read_mcstructure(bytes, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(),
                                                   m->bedrock_block_version(), convert::bedrock_state_lookup(*m), air);
    if (!file) {
        log::error("{}", file.error().with_file(name).describe());
        return kExitError;
    }
    const Structure& s = file->structure;
    usize liquids = 0;
    for (u32 y = 0; y < s.size().y; ++y)
        for (u32 z = 0; z < s.size().z; ++z)
            for (u32 x = 0; x < s.size().x; ++x) {
                const BlockPos p{static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)};
                if (s.has_secondary(p) && s.secondary(p) != air) ++liquids;
            }
    line("File:", name);
    line("Format:", "Bedrock structure (.mcstructure, format_version 1)");
    line("Saved by:", convert::describe(convert::bedrock_block_version_info(file->block_version)));
    line("Size:", std::format("{} x {} x {} ({} blocks)", s.size().x, s.size().y, s.size().z, s.size().volume()));
    line("World origin:", std::format("{}, {}, {}", file->world_origin[0], file->world_origin[1], file->world_origin[2]));
    line("Non-air blocks:", std::to_string(count_non_air(s, air)));
    line("Layer 2:", std::format("{} liquid blocks", liquids));
    line("Block entities:", std::to_string(s.block_entities.size()));
    line("Entities:", std::to_string(s.entities.size()));
    for (const auto& w : file->warnings) log::warn("{}", w);
    return kExitOk;
}

int info_mcworld(const std::string& name, const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::random_device rd;
    const fs::path scratch = fs::temp_directory_path(ec) / std::format("bridger-info-{:08x}{:08x}", rd(), rd());
    fs::create_directories(scratch, ec);
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } cleanup{scratch};
    auto reader = formats::bedrock::BedrockWorldReader::open_path(path, scratch); // opens a copy; the input is only read
    if (!reader) {
        log::error("{}", reader.error().with_file(name).describe());
        return kExitError;
    }
    const auto& lv = reader->level();
    const auto& chunks = reader->overworld_chunks();
    static constexpr std::string_view kGenerators[] = {"old", "infinite", "flat", "nether", "the end", "void"};
    line("File:", name);
    line("Format:", formats::bedrock::is_bedrock_world_folder(path) ? "Bedrock world (folder)" : "Bedrock world (.mcworld)");
    line("Level name:", lv.level_name.empty() ? std::string("(none)") : lv.level_name);
    line("Saved by:", convert::describe(convert::bedrock_world_version_info(lv.last_opened_version)));
    line("Generator:", lv.generator >= 0 && lv.generator < 6 ? std::string(kGenerators[lv.generator]) : std::to_string(lv.generator));
    line("Seed:", std::to_string(lv.seed));
    line("Spawn:", std::format("{}, {}, {}", lv.spawn.x, lv.spawn.y, lv.spawn.z));
    line("Overworld:", std::format("{} chunks", chunks.size()));
    if (!chunks.empty()) {
        i32 x0 = INT_MAX, x1 = INT_MIN, z0 = INT_MAX, z1 = INT_MIN;
        for (const ChunkPos c : chunks) {
            x0 = std::min(x0, c.x);
            x1 = std::max(x1, c.x);
            z0 = std::min(z0, c.z);
            z1 = std::max(z1, c.z);
        }
        line("Chunk area:", std::format("x {}..{}, z {}..{} (blocks x {}..{}, z {}..{})", x0, x1, z0, z1, x0 * 16, x1 * 16 + 15,
                                        z0 * 16, z1 * 16 + 15));
    }
    line("Nether:", std::format("{} chunks", reader->chunks(formats::bedrock::Dimension::Nether).size()));
    line("End:", std::format("{} chunks", reader->chunks(formats::bedrock::Dimension::End).size()));
    line("Entities:", std::to_string(reader->entity_count()));
    return kExitOk;
}

int info_java_world(const std::string& name, const std::filesystem::path& path) {
    auto world = formats::java::open_java_world(path);
    if (!world) {
        log::error("{}", world.error().with_file(name).describe());
        return kExitError;
    }
    const auto count_chunks = [](const std::vector<std::filesystem::path>& regions) {
        usize n = 0;
        for (const auto& rp : regions)
            if (auto r = formats::java::RegionFile::open(rp))
                for (u32 lz = 0; lz < 32; ++lz)
                    for (u32 lx = 0; lx < 32; ++lx) n += r->has_chunk(lx, lz) ? 1 : 0;
        return n;
    };
    const usize chunks = count_chunks(world->overworld_regions);
    const auto& lv = world->level;
    line("Folder:", name);
    line("Format:", world->modern_layout ? "Java world (26.1+ folder layout)" : "Java world (Anvil)");
    line("Level name:", lv.level_name.empty() ? std::string("(none)") : lv.level_name);
    line("Saved by:", convert::describe(convert::java_version_info(formats::FormatId::JavaWorld, lv.data_version, lv.version_name,
                                                                    lv.storage_format)));
    line("Seed:", lv.seed ? std::to_string(*lv.seed) : std::string("(unknown)"));
    if (lv.spawn) line("Spawn:", std::format("{}, {}, {}", lv.spawn->x, lv.spawn->y, lv.spawn->z));
    line("Overworld:", std::format("{} region files, {} chunks", world->overworld_regions.size(), chunks));
    line("Nether:", std::format("{} region files, {} chunks", world->nether_regions.size(), count_chunks(world->nether_regions)));
    line("End:", std::format("{} region files, {} chunks", world->end_regions.size(), count_chunks(world->end_regions)));
    if (world->server_nether_folder || world->server_end_folder)
        line("Server folders:", "the Nether / End are in separate server folders next to this world (not converted)");
    return kExitOk;
}

} // namespace

int run_info(const std::string& input_utf8) {
    auto path = platform::path_from_utf8(input_utf8);
    if (!path) {
        log::error("{}", path.error().describe());
        return kExitError;
    }
    {
        std::error_code ec;
        if (formats::bedrock::is_bedrock_world_folder(*path)) return info_mcworld(input_utf8, *path);
        if (std::filesystem::is_directory(*path, ec) || path->filename() == "level.dat") return info_java_world(input_utf8, *path);
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
    case formats::FormatId::StructureNbt:
    case formats::FormatId::Litematic:
    case formats::FormatId::LegacySchematic: return info_java_structure(input_utf8, bytes, *format);
    case formats::FormatId::McStructure: return info_mcstructure(input_utf8, bytes);
    case formats::FormatId::McWorld: return info_mcworld(input_utf8, *path);
    default:
        log::error("'info' does not support {} files yet", formats::info(*format).name);
        return kExitNotImplemented;
    }
}

} // namespace bridger::cli

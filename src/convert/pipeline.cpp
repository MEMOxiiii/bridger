#include <bridger/convert/pipeline.hpp>
#include <bridger/core/log.hpp>
#include <bridger/core/parallel.hpp>
#include <bridger/formats/bedrock/mcstructure.hpp>
#include <bridger/formats/bedrock/mcworld/bedrock_world.hpp>
#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/bedrock/mcworld/subchunk_codec.hpp>
#include <bridger/formats/detect.hpp>
#include <bridger/formats/bedrock/mcworld/bedrock_world_reader.hpp>
#include <bridger/formats/java/anvil/anvil_writer.hpp>
#include <bridger/formats/java/anvil/chunk_codec.hpp>
#include <bridger/formats/java/anvil/java_world.hpp>
#include <bridger/formats/java/anvil/region_file.hpp>
#include <bridger/formats/java/java_writers.hpp>
#include <bridger/formats/java/legacy_schematic.hpp>
#include <bridger/formats/structure_file.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/zip_archive.hpp>
#include <bridger/platform/atomic_replace.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>
#include <bridger/formats/java/upgrade.hpp>
#include <bridger/translate/bedrock_upgrade.hpp>
#include <bridger/translate/player.hpp>
#include <bridger/translate/targets.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <tuple>
#include <unordered_map>

namespace bridger::convert {
namespace fs = std::filesystem;
using formats::FormatId;

namespace {

// A scratch directory under %TEMP%, removed afterwards.
class ScratchDir {
public:
    static Result<ScratchDir> create(std::string_view purpose) {
        std::random_device rd;
        std::error_code ec;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const fs::path p = fs::temp_directory_path(ec) / std::format("bridger-{}-{:08x}{:08x}", purpose, rd(), rd());
            if (!ec && fs::create_directories(p, ec) && !ec) return ScratchDir(p);
        }
        return make_error(ErrorCode::IoError, "cannot create a scratch directory: " + ec.message());
    }
    ScratchDir(ScratchDir&& o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
    ScratchDir& operator=(ScratchDir&&) = delete;
    ~ScratchDir() {
        std::error_code ec;
        if (!path_.empty()) fs::remove_all(path_, ec);
    }
    const fs::path& path() const noexcept { return path_; }

private:
    explicit ScratchDir(fs::path p) : path_(std::move(p)) {}
    fs::path path_;
};

// The last cancellation point: checked after the output was validated, immediately before it is
// moved into place. The move is the point of no return; a cancel after it no longer applies.
Status cancel_point(const ConvertOptions& o) {
    if (o.cancel != nullptr && o.cancel->load(std::memory_order_relaxed)) return make_error(ErrorCode::Cancelled, "the conversion was cancelled");
    return {};
}

// The gate the atomic writer calls once, after validation and right before the move.
platform::MoveGate cancel_gate(const ConvertOptions& o) {
    return [&o]() -> Status { return cancel_point(o); };
}

// Progress report + cancellation point.
Status step(const ConvertOptions& o, float fraction, std::string_view what) {
    if (o.cancel != nullptr && o.cancel->load(std::memory_order_relaxed)) return make_error(ErrorCode::Cancelled, "the conversion was cancelled");
    if (o.progress) o.progress(std::clamp(fraction, 0.0f, 1.0f), what);
    return {};
}

std::string json_escape(std::string_view s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", c);
            else out += c;
        }
    }
    return out;
}

bool is_java_structure(FormatId f) {
    return f == FormatId::SpongeSchem || f == FormatId::StructureNbt || f == FormatId::Litematic ||
           f == FormatId::LegacySchematic;
}

// Encoded {name, states, version} NBT per Bedrock state, built on demand.
class BedrockStateNbt {
public:
    explicit BedrockStateNbt(const translate::MappingData& m) : m_(m), cache_(m.bedrock_state_count()) {}
    // Encodes every state up front, so fn() never writes and is safe to call from many threads.
    void prepare_all() {
        const auto f = fn();
        for (u32 s = 0; s < cache_.size(); ++s) (void)f(s);
    }
    formats::bedrock::StateNbtFn fn() {
        return [this](u32 state) -> Result<std::span<const u8>> {
            if (state >= cache_.size()) return make_error(ErrorCode::InvalidArgument, "unknown Bedrock state");
            if (!cache_[state]) {
                const translate::BedrockState& bs = m_.bedrock(state);
                io::nbt::NbtWriter w(Endian::Little);
                w.begin_root_compound("");
                w.write_string("name", bs.name);
                w.begin_compound("states");
                for (const translate::BedrockProperty& p : bs.properties) {
                    switch (p.type) {
                    case translate::NbtValueType::Byte: w.write_byte(p.key, static_cast<i8>(p.int_value)); break;
                    case translate::NbtValueType::Int: w.write_int(p.key, p.int_value); break;
                    case translate::NbtValueType::String: w.write_string(p.key, p.str_value); break;
                    }
                }
                w.end_compound();
                w.write_int("version", static_cast<i32>(m_.bedrock_block_version()));
                w.end_compound();
                BRIDGER_TRY(cache_[state], std::move(w).finish());
            }
            return std::span<const u8>(*cache_[state]);
        };
    }

private:
    const translate::MappingData& m_;
    std::vector<std::optional<std::vector<u8>>> cache_;
};

formats::bedrock::StateLookupFn bedrock_lookup(const translate::MappingData& m) { return bedrock_state_lookup(m); }

u64 count_non_air(const Structure& s, u32 air) {
    u64 n = 0;
    for (u32 y = 0; y < s.size().y; ++y)
        for (u32 z = 0; z < s.size().z; ++z)
            for (u32 x = 0; x < s.size().x; ++x)
                if (s.block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)}) != air) ++n;
    return n;
}

// The output's target version (ConvertOptions::target_version), resolved once per conversion.
struct OutputTarget {
    const translate::JavaTargets* java_targets = nullptr;
    const translate::JavaTarget* java = nullptr;
    const translate::BedrockTargets* bedrock_targets = nullptr;
    const translate::BedrockTarget* bedrock = nullptr;
};

bool bedrock_output(FormatId f) { return f == FormatId::McWorld || f == FormatId::McStructure; }

Result<OutputTarget> resolve_target(const ConvertOptions& o, FormatId output, ConvertSummary& summary) {
    OutputTarget t;
    if (bedrock_output(output)) {
        BRIDGER_TRY(t.bedrock_targets, translate::BedrockTargets::embedded());
        t.bedrock = o.target_version.empty() ? &t.bedrock_targets->pinned() : t.bedrock_targets->find(o.target_version);
        if (t.bedrock == nullptr)
            return make_error(ErrorCode::InvalidArgument,
                              std::format("Bedrock {} is not a target version Bridger can write ({} to {})", o.target_version,
                                          t.bedrock_targets->targets().back().name, t.bedrock_targets->pinned().name));
        if (o.into_world && !t.bedrock->pinned)
            return make_error(ErrorCode::InvalidArgument, "placing into an existing world writes for that world's game; leave the target "
                                                          "version at the newest");
        summary.target_version = "Bedrock " + t.bedrock->name;
    } else {
        BRIDGER_TRY(t.java_targets, translate::JavaTargets::embedded());
        t.java = o.target_version.empty() ? &t.java_targets->pinned() : t.java_targets->find(o.target_version);
        if (t.java == nullptr)
            return make_error(ErrorCode::InvalidArgument,
                              std::format("Java {} is not a target version Bridger can write ({} to {})", o.target_version,
                                          t.java_targets->targets().back().name, t.java_targets->pinned().name));
        summary.target_version = "Java " + t.java->name;
        summary.target_data_version = static_cast<u32>(t.java->data_version);
    }
    return t;
}

// Bedrock palette entries for the target: the pinned encoder, or the target's spellings and block version.
formats::bedrock::StateNbtFn target_state_nbt(const OutputTarget& t, const translate::MappingData& m, formats::bedrock::StateNbtFn pinned) {
    if (t.bedrock == nullptr || t.bedrock->pinned) return pinned;
    return [&t, &m](u32 state) -> Result<std::span<const u8>> {
        const std::span<const u8> bytes = t.bedrock_targets->state_nbt(*t.bedrock, state, m);
        if (bytes.empty()) return make_error(ErrorCode::InvalidArgument, "unknown Bedrock state");
        return bytes;
    };
}

void apply_target_level(const OutputTarget& t, formats::bedrock::LevelDatOptions& level) {
    if (t.bedrock == nullptr) return;
    level.storage_version = t.bedrock->storage_version;
    level.network_version = t.bedrock->protocol;
    level.version = t.bedrock->version;
}

BlockState java_fallback(translate::Fallback f) {
    return BlockState(f == translate::Fallback::Air ? "minecraft:air" : "minecraft:sponge", {});
}

Status check_output(const ConvertOptions& o) {
    if (!o.overwrite && fs::exists(o.output))
        return make_error(ErrorCode::AlreadyExists, "output exists; pass --overwrite to replace it: " +
                                                        platform::path_to_utf8(o.output).value_or("?"));
    return {};
}

bool is_java_world_input(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec) || p.filename() == "level.dat";
}

// The input is never modified (docs/safety.md): the output may not be the input, lie inside an input world folder,
// or be a folder that contains the input (a world written there would replace it). weakly_canonical gives the real
// casing of the existing part of a path, which is the only part that can overlap with the (existing) input.
Status check_output_is_not_input(const ConvertOptions& o) {
    std::error_code ec;
    fs::path in = o.input.filename() == "level.dat" ? o.input.parent_path() : o.input;
    in = fs::weakly_canonical(in, ec);
    if (ec) return {};
    const fs::path out = fs::weakly_canonical(o.output, ec);
    if (ec) return {};
    const auto within = [](const fs::path& inner, const fs::path& outer) {
        auto i = inner.begin();
        for (auto p = outer.begin(); p != outer.end(); ++p, ++i)
            if (i == inner.end() || *i != *p) return false;
        return true;
    };
    if (within(out, in) || within(in, out))
        return make_error(ErrorCode::InvalidArgument, "the output would replace or sit inside the input; choose another name: " +
                                                          platform::path_to_utf8(o.output).value_or("?"));
    return {};
}

// Java world -> new .mcworld, one chunk column at a time (docs/formats/anvil.md). Memory
// stays bounded by one region file mapping plus one column; the world is built in a
// scratch directory, validated there, zipped next to the output and moved atomically.
// Compacts, validates and zips a world built by WorldBuilder in `dir`, then moves it to o.output atomically.
template <class Phase>
Result<ConvertSummary> finish_mcworld(const ConvertOptions& o, ConvertSummary summary, formats::bedrock::WorldBuilder& builder,
                                      const fs::path& dir, const formats::bedrock::LevelDatOptions& level, const Phase& phase) {
    BRIDGER_TRY_STATUS(step(o, 0.86f, "Compacting the world database"));
    BRIDGER_TRY_STATUS(builder.finish(level));
    phase("database compacted");
    const formats::bedrock::WorldWriteStats written = builder.stats();
    summary.chunks = written.chunks;
    summary.subchunks = written.subchunks;
    summary.block_entities = written.block_entities;

    // Validate the database before zipping, then the zip itself before the move.
    formats::bedrock::WorldWriteStats reread;
    BRIDGER_TRY_STATUS(step(o, 0.9f, "Validating"));
    BRIDGER_TRY_STATUS(formats::bedrock::validate_world_directory(dir, &reread));
    if (reread.subchunks != written.subchunks || reread.block_entities != written.block_entities ||
        reread.chunks != written.chunks)
        return make_error(ErrorCode::ValidationFailed, "re-read world differs from what was written");
    phase("world validated");
    BRIDGER_TRY(const fs::path temp, platform::AtomicFileWriter::temp_path_for(o.output));
    BRIDGER_TRY_STATUS(step(o, 0.94f, "Packing the .mcworld"));
    platform::AtomicFileWriter writer = platform::AtomicFileWriter::adopt(temp, o.output);
    BRIDGER_TRY_STATUS(io::write_zip_file_from_directory(dir, temp));
    phase("zipped");
    usize expected_entries = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) ++expected_entries;
    auto validate = [&](const fs::path& zip) -> Status {
        BRIDGER_TRY(const std::vector<io::ZipListing> listing, io::list_zip_file(zip));
        const bool has_level = std::any_of(listing.begin(), listing.end(), [](const io::ZipListing& l) { return l.name == "level.dat"; });
        if (listing.size() != expected_entries || !has_level)
            return make_error(ErrorCode::ValidationFailed, "the zipped world does not list every world file");
        return {};
    };
    BRIDGER_TRY_STATUS(writer.commit(validate, cancel_gate(o)));
    phase("moved into place");
    return summary;
}

struct JavaWorldInfo {
    std::string level_name;
    BlockPos spawn{0, 64, 0};
    std::optional<i64> seed;
    std::optional<RawNbt> player; // level.dat Data.Player
};

// Writes level.dat (and, from 26.1, world_gen_settings.dat) into the Java world built in `temp` for `target`, checks
// that every chunk re-reads, and renames the folder to o.output. `keep` is set once the folder must not be removed.
using formats::bedrock::Dimension;
constexpr std::array<Dimension, 3> kDimensions{Dimension::Overworld, Dimension::Nether, Dimension::End};

std::string_view dimension_name(Dimension d) {
    return d == Dimension::Nether ? "the Nether" : d == Dimension::End ? "the End" : "the Overworld";
}

// A dimension's region folder in a Java world: dimensions/minecraft/<name>/region (26.1+), else region, DIM-1/region,
// DIM1/region (docs/formats/anvil.md).
fs::path java_region_dir(const fs::path& root, bool modern_layout, Dimension d) {
    if (modern_layout)
        return root / "dimensions" / "minecraft" / (d == Dimension::Nether ? "the_nether" : d == Dimension::End ? "the_end" : "overworld") /
               "region";
    return d == Dimension::Nether ? root / "DIM-1" / "region" : d == Dimension::End ? root / "DIM1" / "region" : root / "region";
}

// A dimension's entity region folder (Java 1.17+): dimensions/minecraft/<name>/entities, or entities, DIM-1/entities,
// DIM1/entities.
fs::path java_entities_dir(const fs::path& root, bool modern_layout, Dimension d) {
    return java_region_dir(root, modern_layout, d).parent_path() / "entities";
}

const std::vector<fs::path>& java_regions(const formats::java::JavaWorld& w, Dimension d) {
    return d == Dimension::Nether ? w.nether_regions : d == Dimension::End ? w.end_regions : w.overworld_regions;
}

// Bedrock biome a column of a dimension falls back to (Chunker Dimension.fallbackBiome; Bedrock ids from
// BedrockBiomeIDResolver: hell 8, the_end 9).
i32 bedrock_default_biome(Dimension d) {
    return d == Dimension::Nether ? 8 : d == Dimension::End ? 9 : translate::kDefaultBedrockBiome;
}

// Warnings for the Nether and the End a server keeps in separate world folders (not read, as Chunker).
void warn_server_dimensions(const formats::java::JavaWorld& w, ConvertSummary& summary) {
    if (w.server_nether_folder)
        summary.warnings.push_back("the Nether is in a separate server folder (" + platform::path_to_utf8(w.root.filename()).value_or("world") +
                                   "_nether) and was not converted");
    if (w.server_end_folder)
        summary.warnings.push_back("the End is in a separate server folder (" + platform::path_to_utf8(w.root.filename()).value_or("world") +
                                   "_the_end) and was not converted");
}

void warn_clipped(const std::array<usize, 3>& clipped, ConvertSummary& summary) {
    for (const Dimension d : kDimensions)
        if (const usize n = clipped[static_cast<usize>(d)]; n > 0) {
            const auto range = formats::bedrock::dimension_range(d);
            summary.warnings.push_back(std::format("{} blocks of {} outside the Bedrock build height (y {} to {}) were dropped", n,
                                                   dimension_name(d), range.min_y(), range.max_y()));
        }
}

Result<ConvertSummary> finish_java_world(const ConvertOptions& o, ConvertSummary summary, const fs::path& temp, bool& keep,
                                         const OutputTarget& target, const JavaWorldInfo& info, usize chunks, usize block_entities,
                                         const std::vector<std::string>& experiments) {
    const bool modern_layout = target.java->data_version >= formats::java::kJava26_1;
    formats::java::JavaLevelOptions level;
    level.level_name = info.level_name;
    level.spawn = info.spawn;
    level.seed = info.seed;
    level.player = info.player;
    level.data_version = target.java->data_version;
    level.version_name = target.java->name;
    level.experiments = experiments;
    for (const std::string& e : experiments)
        summary.warnings.push_back(std::format("the world uses blocks of the experimental feature {} of Java {}; it is switched on in "
                                               "the world (the game marks it as experimental)", e, target.java->name));
    level.last_played_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    BRIDGER_TRY(const std::vector<u8> level_dat, formats::java::encode_java_level_dat(level));
    {
        std::ofstream f(temp / "level.dat", std::ios::binary);
        f.write(reinterpret_cast<const char*>(level_dat.data()), static_cast<std::streamsize>(level_dat.size()));
        if (!f) return make_error(ErrorCode::IoError, "cannot write level.dat");
    }
    if (modern_layout) { // 26.1+: the seed is in data/minecraft/world_gen_settings.dat (before: in level.dat)
        BRIDGER_TRY(const std::vector<u8> wgs, formats::java::encode_world_gen_settings(info.seed.value_or(0), level.data_version));
        std::ofstream g(temp / "data" / "minecraft" / "world_gen_settings.dat", std::ios::binary);
        g.write(reinterpret_cast<const char*>(wgs.data()), static_cast<std::streamsize>(wgs.size()));
        if (!g) return make_error(ErrorCode::IoError, "cannot write world_gen_settings.dat");
    }

    // Validate: the folder opens as a Java world and every chunk decodes.
    {
        BRIDGER_TRY(const formats::java::JavaWorld back, formats::java::open_java_world(temp));
        usize reread = 0;
        for (const Dimension d : kDimensions)
        for (const fs::path& rp : java_regions(back, d)) {
            BRIDGER_TRY(const formats::java::RegionFile rf, formats::java::RegionFile::open(rp));
            for (u32 lz = 0; lz < 32; ++lz)
                for (u32 lx = 0; lx < 32; ++lx) {
                    if (!rf.has_chunk(lx, lz)) continue;
                    BRIDGER_TRY(const std::optional<std::vector<u8>> raw, rf.read_chunk(lx, lz));
                    if (!raw) return make_error(ErrorCode::ValidationFailed, "a written chunk is missing");
                    auto blocks = std::make_shared<BlockStateRegistry>();
                    BRIDGER_TRY(const formats::java::JavaChunk jc, formats::java::decode_chunk(*raw, blocks, std::make_shared<BiomeRegistry>()));
                    if (!jc.full) return make_error(ErrorCode::ValidationFailed, "a written chunk is not marked full");
                    ++reread;
                }
        }
        if (reread != chunks)
            return make_error(ErrorCode::ValidationFailed, std::format("re-read {} chunks, expected {}", reread, chunks));
    }
    // Last cancellation point, then the move into place: a rename of the folder (never replaces
    // anything; checked again by the OS). A cancelled build is removed with its temp folder.
    BRIDGER_TRY_STATUS(cancel_point(o));
    if (auto moved = platform::os_file_ops().move_new(temp, o.output); !moved) {
        keep = true;
        return make_error(ErrorCode::IoError, "the converted world is complete at " + platform::path_to_utf8(temp).value_or("?") +
                                                  " but could not be renamed to the destination: " + moved.error().describe());
    }
    keep = true; // it is the destination now
    summary.chunks = chunks;
    summary.block_entities = block_entities;
    return summary;
}

Result<ConvertSummary> convert_java_world_to_java(const ConvertOptions& o, ConvertSummary summary);

Result<ConvertSummary> convert_java_world(const ConvertOptions& o, ConvertSummary summary) {
    if (summary.output_format == FormatId::JavaWorld) return convert_java_world_to_java(o, std::move(summary));
    if (summary.output_format != FormatId::McWorld)
        return make_error(ErrorCode::NotImplemented, "a Java world converts to a .mcworld or to a Java world folder");
    if (o.into_world) return make_error(ErrorCode::InvalidArgument, "--into-world is not available for whole worlds");
    BRIDGER_TRY_STATUS(check_output(o));
    BRIDGER_TRY(const formats::java::JavaWorld world, formats::java::open_java_world(o.input));
    BRIDGER_TRY(const translate::MappingData* mapping, translate::MappingData::embedded());
    summary.target_data_version = mapping->java_data_version();
    summary.source_data_version = world.level.data_version;
    summary.source_version = java_version_info(FormatId::JavaWorld, world.level.data_version, world.level.version_name,
                                               world.level.storage_format);
    const auto air = mapping->find_bedrock("minecraft:air", {});
    if (!air) return make_error(ErrorCode::Malformed, "mapping data lacks air");
    if (!world.level.data_version)
        summary.warnings.push_back(std::format("the world was saved before Java 1.9 (no DataVersion); its blocks and biomes were "
                                               "upgraded to Java {}", mapping->java_version()));
    else if (static_cast<u32>(*world.level.data_version) < mapping->java_data_version())
        summary.warnings.push_back(std::format("the world was saved by an older Java version (DataVersion {}); its blocks and biomes "
                                               "were upgraded to Java {}", *world.level.data_version, mapping->java_version()));
    else if (static_cast<u32>(*world.level.data_version) != mapping->java_data_version())
        summary.warnings.push_back(std::format("world DataVersion {} is newer than the pinned Java version's {} ({}); unknown "
                                               "blocks are reported",
                                               *world.level.data_version, mapping->java_data_version(), mapping->java_version()));
    warn_server_dimensions(world, summary);
    if (world.overworld_regions.empty()) return make_error(ErrorCode::Malformed, "the world has no Overworld region files");

    BRIDGER_TRY(const ScratchDir build, ScratchDir::create("javaworld"));
    const fs::path dir = build.path() / "world";
    BRIDGER_TRY(formats::bedrock::WorldBuilder builder, formats::bedrock::WorldBuilder::create(dir));
    const auto t0 = std::chrono::steady_clock::now();
    const auto phase = [&t0](std::string_view what) {
        const auto now = std::chrono::steady_clock::now();
        log::debug("{}: {} ms", what, std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count());
    };
    BRIDGER_TRY(const OutputTarget target, resolve_target(o, FormatId::McWorld, summary));
    BedrockStateNbt state_nbt(*mapping);
    state_nbt.prepare_all(); // read-only afterwards: shared by the worker threads without locks
    const formats::java::LegacyTables legacy = legacy_tables(*mapping); // pre-1.13 chunks (numeric ids)
    if (!target.bedrock->pinned) // encode every target spelling up front too (the cache then only reads)
        for (u32 s = 0; s < mapping->bedrock_state_count(); ++s) (void)target.bedrock_targets->state_nbt(*target.bedrock, s, *mapping);
    const auto state_fn = target_state_nbt(target, *mapping, state_nbt.fn());
    translate::TranslateOptions topt;
    topt.fallback = o.fallback;
    usize skipped_partial = 0;
    std::array<usize, 3> clipped{}; // per dimension
    std::map<std::string, usize> failures; // reason -> chunk count
    std::string first_failure;

    // Workers decode, translate and encode chunks (each with its own registries and
    // report); this thread then stores the results in region/slot order, so the output
    // does not depend on the thread count (docs/performance.md, tests/unit/formats/anvil_test.cpp).
    struct ChunkResult {
        enum class Kind : u8 { Empty, Failed, Partial, Done } kind = Kind::Empty;
        std::optional<Error> error;
        formats::bedrock::EncodedColumn encoded;
        translate::ConversionReport report;
        std::vector<std::string> warnings;
        u64 blocks = 0;
    };
    constexpr usize kBatch = 128; // chunk slots in flight: bounds memory to ~kBatch encoded columns
    std::vector<ChunkResult> results(kBatch);
    usize regions_done = 0;
    const usize total_regions = world.overworld_regions.size() + world.nether_regions.size() + world.end_regions.size();
    for (const Dimension dim : kDimensions)
    for (const fs::path& region_path : java_regions(world, dim)) {
        BRIDGER_TRY_STATUS(step(o, 0.85f * static_cast<float>(regions_done++) / static_cast<float>(total_regions), "Converting chunks"));
        auto region = formats::java::RegionFile::open(region_path);
        if (!region) {
            ++failures[region.error().describe()];
            if (first_failure.empty()) first_failure = region.error().describe();
            continue;
        }
        // Entities (1.17+) live in a region file of the same name under entities/.
        std::optional<formats::java::RegionFile> entity_region;
        if (const fs::path ep = java_entities_dir(world.root, world.modern_layout, dim) / region_path.filename(); fs::exists(ep)) {
            if (auto er = formats::java::RegionFile::open(ep)) entity_region.emplace(std::move(*er));
            else summary.warnings.push_back("entity file " + platform::path_to_utf8(ep.filename()).value_or("?") + " could not be read: " +
                                            er.error().describe());
        }
        for (usize batch = 0; batch < 1024; batch += kBatch) {
            BRIDGER_TRY_STATUS(step(o, 0.85f * (static_cast<float>(regions_done - 1) + static_cast<float>(batch) / 1024.0f) /
                                           static_cast<float>(total_regions),
                                    "Converting chunks"));
            parallel_for(kBatch, o.threads, [&](usize i) {
                ChunkResult& r = results[i];
                r = ChunkResult{};
                const u32 slot = static_cast<u32>(batch + i), lx = slot % 32, lz = slot / 32;
                if (!region->has_chunk(lx, lz)) return;
                auto fail = [&](Error e) {
                    r.kind = ChunkResult::Kind::Failed;
                    r.error = std::move(e);
                };
                auto raw = region->read_chunk(lx, lz);
                if (!raw) return fail(std::move(raw).error());
                auto chunk = formats::java::decode_chunk(**raw, std::make_shared<BlockStateRegistry>(),
                                                         std::make_shared<BiomeRegistry>(), {}, &legacy);
                if (!chunk) return fail(std::move(chunk).error());
                if (!chunk->full) {
                    r.kind = ChunkResult::Kind::Partial;
                    return;
                }
                r.warnings = std::move(chunk->warnings);
                r.blocks = count_non_air(chunk->column, kAirStateId);
                r.report.set_origin({chunk->pos.x * 16, chunk->min_y, chunk->pos.z * 16});
                if (entity_region && entity_region->has_chunk(lx, lz)) {
                    auto raw_entities = entity_region->read_chunk(lx, lz);
                    auto entities = raw_entities && *raw_entities
                                        ? formats::java::decode_entity_chunk(**raw_entities, chunk->pos, chunk->min_y)
                                        : Result<std::vector<Entity>>(std::vector<Entity>{});
                    if (entities)
                        for (Entity& e : *entities) chunk->column.entities.push_back(std::move(e));
                    else
                        r.warnings.push_back(std::format("entities of chunk {},{} could not be read", chunk->pos.x, chunk->pos.z));
                }
                auto bedrock = translate::translate_to_bedrock(chunk->column, *mapping, topt, r.report);
                if (!bedrock) return fail(std::move(bedrock).error());
                if (auto rw = translate::rewrite_for_bedrock_target(*bedrock, *target.bedrock_targets, *target.bedrock, *mapping, r.report);
                    !rw)
                    return fail(std::move(rw).error());
                auto encoded = formats::bedrock::WorldBuilder::encode_column(chunk->pos, chunk->min_y, *bedrock, *air,
                                                                             bedrock_default_biome(dim), state_fn,
                                                                             static_cast<u8>(target.bedrock->chunk_version), dim);
                if (!encoded) return fail(std::move(encoded).error());
                r.encoded = std::move(*encoded);
                r.kind = ChunkResult::Kind::Done;
            });
            for (usize i = 0; i < kBatch; ++i) {
                ChunkResult& r = results[i];
                switch (r.kind) {
                case ChunkResult::Kind::Empty: break;
                case ChunkResult::Kind::Partial: ++skipped_partial; break;
                case ChunkResult::Kind::Failed:
                    if (first_failure.empty())
                        first_failure = std::format("region {},{} chunk {},{}: {}", region->region_x(), region->region_z(),
                                                    (batch + i) % 32, (batch + i) / 32, r.error->describe());
                    ++failures[std::string(to_string(r.error->code()))];
                    break;
                case ChunkResult::Kind::Done:
                    summary.warnings.insert(summary.warnings.end(), r.warnings.begin(), r.warnings.end());
                    summary.blocks += r.blocks;
                    summary.report.merge(r.report);
                    clipped[static_cast<usize>(dim)] += r.encoded.clipped_blocks;
                    BRIDGER_TRY_STATUS(builder.put(r.encoded));
                    break;
                }
                r = ChunkResult{}; // release memory before the next batch
            }
        }
    }
    if (skipped_partial > 0)
        summary.warnings.push_back(std::format("{} partially generated chunks were skipped (the game generates them)",
                                               skipped_partial));
    warn_clipped(clipped, summary);
    for (const auto& [reason, n] : failures)
        summary.warnings.push_back(std::format("{} chunks could not be read ({})", n, reason));
    if (!first_failure.empty()) summary.warnings.push_back("first unreadable chunk: " + first_failure);
    if (builder.stats().chunks == 0)
        return make_error(ErrorCode::Malformed, "no chunk of the world could be converted" +
                                                    (first_failure.empty() ? std::string() : "; " + first_failure));

    formats::bedrock::LevelDatOptions level;
    level.level_name = !o.level_name.empty() ? o.level_name
                       : !world.level.level_name.empty() ? world.level.level_name
                                                          : platform::path_to_utf8(world.root.filename()).value_or("Bridger World");
    level.spawn = world.level.spawn.value_or(BlockPos{0, 64, 0});
    level.random_seed = world.level.seed.value_or(0);
    level.generator = 1; // infinite: chunks the Java world never generated are generated by Bedrock from the same seed
    if (world.level.player)
        if (const auto player = translate::java_player_to_bedrock(*world.level.player, *mapping, summary.report))
            BRIDGER_TRY_STATUS(builder.put_raw("~local_player", *player));
    level.flat_world_layers = formats::bedrock::default_flat_world_layers();
    level.last_played =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    apply_target_level(target, level);
    phase("chunks converted");
    return finish_mcworld(o, std::move(summary), builder, dir, level, phase);
}


Result<FormatId> output_format_of(const ConvertOptions& o) {
    if (o.output_format) return *o.output_format;
    BRIDGER_TRY(const std::string ext, platform::path_to_utf8(o.output.extension()));
    if (ext.empty()) return FormatId::JavaWorld; // a folder
    const auto f = formats::format_from_extension(ext);
    if (!f) return make_error(ErrorCode::UnsupportedFormat, "cannot tell the output format from '" + ext + "'; use --format");
    return *f;
}

Result<ConvertSummary> not_supported(const ConvertSummary& s) {
    return make_error(ErrorCode::NotImplemented, "converting " + std::string(formats::info(s.input_format).name) + " to " +
                                                     std::string(formats::info(s.output_format).name) +
                                                     " is not supported yet");
}

// A Java world is a folder: it is built next to the destination and renamed into place, so the
// destination must not exist (an existing world is never replaced or merged, docs/safety.md).
Status check_output_directory(const ConvertOptions& o) {
    std::error_code ec;
    if (fs::exists(o.output, ec))
        return make_error(ErrorCode::AlreadyExists, "output folder exists; Bridger never replaces a world folder, choose a new name: " +
                                                        platform::path_to_utf8(o.output).value_or("?"));
    return {};
}

// Re-reads a written Java structure file and compares every block with what was written.
Status validate_java_structure(const fs::path& written, FormatId format, const Structure& expected, i32 data_version) {
    BRIDGER_TRY(const platform::MappedFile f, platform::MappedFile::open(written));
    BRIDGER_TRY(const formats::StructureFile back,
                formats::read_java_structure(f.data(), format, std::make_shared<BlockStateRegistry>(),
                                             std::make_shared<BiomeRegistry>()));
    const Structure& s = back.structure;
    if (s.size() != expected.size() || s.block_entities.size() != expected.block_entities.size())
        return make_error(ErrorCode::ValidationFailed, "re-read file differs in size or block entity count");
    std::unordered_map<StateId, StateId> same; // re-read id -> written id
    const Extent e = s.size();
    for (u32 y = 0; y < e.y; ++y)
        for (u32 z = 0; z < e.z; ++z)
            for (u32 x = 0; x < e.x; ++x) {
                const BlockPos p{static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)};
                const StateId got = s.block(p);
                auto it = same.find(got);
                if (it == same.end()) {
                    const auto st = BlockState::parse(s.block_registry().canonical(got));
                    const StateId mapped = st ? expected.block_registry().intern(*st) : UINT32_MAX;
                    it = same.emplace(got, mapped).first;
                }
                // Written for an older version, a state reads back upgraded (e.g. grass -> short_grass).
                const StateId want = expected.block(p);
                if (it->second != want &&
                    expected.block_registry().intern(formats::java::upgrade_block_state(expected.block_registry().get(want), data_version)) != it->second)
                    return make_error(ErrorCode::ValidationFailed,
                                      std::format("re-read block at {},{},{} differs from what was written", p.x, p.y, p.z));
            }
    return {};
}

Result<ConvertSummary> write_java_structure_output(const ConvertOptions& o, ConvertSummary summary, const Structure& java,
                                                   i32 data_version) {
    formats::java::JavaWriteOptions wo;
    wo.data_version = data_version;
    wo.name = o.level_name.empty() ? platform::path_to_utf8(o.output.stem()).value_or("Bridger") : o.level_name;
    wo.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    Result<std::vector<u8>> bytes = make_error(ErrorCode::NotImplemented, "no writer");
    switch (summary.output_format) {
    case FormatId::SpongeSchem: bytes = formats::java::write_sponge_schem(java, wo); break;
    case FormatId::StructureNbt: bytes = formats::java::write_structure_nbt(java, wo); break;
    case FormatId::Litematic: bytes = formats::java::write_litematic(java, wo); break;
    default: return not_supported(summary);
    }
    if (!bytes) return tl::unexpected(std::move(bytes).error());
    const FormatId format = summary.output_format;
    BRIDGER_TRY_STATUS(step(o, 0.9f, "Saving"));
    BRIDGER_TRY_STATUS(platform::write_file_atomically(
        o.output, *bytes, [&](const fs::path& written) { return validate_java_structure(written, format, java, data_version); },
        cancel_gate(o)));
    summary.block_entities = java.block_entities.size();
    return summary;
}

Result<ConvertSummary> write_bedrock_output(const ConvertOptions& o, ConvertSummary summary, const Structure& bedrock,
                                            const translate::MappingData& m, u32 air, const OutputTarget& target) {
    // Actors (paintings) are written in whole worlds only; for structures they are reported.
    for (const Entity& e : bedrock.entities)
        summary.report.add(translate::IssueKind::UnmappedEntity, e.id + " (not written into structures yet)",
                           {static_cast<i32>(e.pos.x), static_cast<i32>(e.pos.y), static_cast<i32>(e.pos.z)});
    BedrockStateNbt pinned_nbt(m);
    const formats::bedrock::StateNbtFn state_fn = target_state_nbt(target, m, pinned_nbt.fn());
    if (summary.output_format == FormatId::McStructure) {
        BRIDGER_TRY(const std::vector<u8> bytes, formats::bedrock::write_mcstructure(bedrock, air, state_fn));
        const auto lookup = bedrock_lookup(m);
        auto validate = [&](const fs::path& written) -> Status {
            BRIDGER_TRY(const platform::MappedFile f, platform::MappedFile::open(written));
            BRIDGER_TRY(const formats::bedrock::McStructureFile back,
                        formats::bedrock::read_mcstructure(f.data(), std::make_shared<BlockStateRegistry>(),
                                                           std::make_shared<BiomeRegistry>(), m.bedrock_block_version(),
                                                           lookup, air));
            if (back.structure.size() != bedrock.size() || back.structure.block_entities.size() != bedrock.block_entities.size() ||
                !back.warnings.empty())
                return make_error(ErrorCode::ValidationFailed, "re-read .mcstructure differs from what was written");
            return {};
        };
        BRIDGER_TRY_STATUS(step(o, 0.9f, "Saving"));
        BRIDGER_TRY_STATUS(platform::write_file_atomically(o.output, bytes, validate, cancel_gate(o)));
        summary.block_entities = bedrock.block_entities.size();
        return summary;
    }

    if (o.into_world) {
        BRIDGER_TRY(const platform::MappedFile world, platform::MappedFile::open(*o.into_world)); // read-only
        formats::bedrock::PlaceOptions po;
        po.origin = summary.placement->origin;
        po.air_replaces = o.placement.air == AirMode::Replace;
        po.clear_entities = o.placement.clear_entities;
        po.air_state = air;
        po.default_biome = translate::kDefaultBedrockBiome;
        BRIDGER_TRY(const ScratchDir work, ScratchDir::create("place"));
        formats::bedrock::PlaceStats stats;
        BRIDGER_TRY(const std::vector<u8> out,
                    formats::bedrock::place_into_world(world.data(), bedrock, po, state_fn, work.path(), &stats));
        for (const auto& r : stats.removed_block_entities)
            summary.report.add(translate::IssueKind::RemovedBlockEntity, r.id, r.pos);
        for (const auto& r : stats.removed_entities) summary.report.add(translate::IssueKind::RemovedEntity, r.id, r.pos);
        summary.warnings.insert(summary.warnings.end(), stats.warnings.begin(), stats.warnings.end());
        summary.chunks = stats.chunks_touched;
        summary.subchunks = stats.subchunks_written;
        summary.block_entities = stats.block_entities_written;
        auto validate = [&](const fs::path& written) -> Status {
            BRIDGER_TRY(const platform::MappedFile f, platform::MappedFile::open(written));
            BRIDGER_TRY(const ScratchDir check, ScratchDir::create("check"));
            return formats::bedrock::validate_placed_world(f.data(), world.data(), stats.touched_keys, check.path());
        };
        BRIDGER_TRY_STATUS(step(o, 0.9f, "Saving"));
        BRIDGER_TRY_STATUS(platform::write_file_atomically(o.output, out, validate, cancel_gate(o)));
        return summary;
    }

    // New world.
    const translate::BedrockProperty no_infiniburn{"infiniburn_bit", translate::NbtValueType::Byte, 0, {}};
    const auto bedrock_block = m.find_bedrock("minecraft:bedrock", std::span(&no_infiniburn, 1));
    const auto dirt = m.find_bedrock("minecraft:dirt", {});
    const auto grass = m.find_bedrock("minecraft:grass_block", {});
    if (!bedrock_block || !dirt || !grass) return make_error(ErrorCode::Malformed, "mapping data lacks the superflat layer blocks");
    formats::bedrock::NewWorldOptions wopt;
    wopt.origin = summary.placement->origin;
    wopt.air_replaces = o.placement.air == AirMode::Replace;
    wopt.air_state = air;
    wopt.flat_layers = {{*bedrock_block, 1}, {*dirt, 2}, {*grass, 1}};
    wopt.default_biome = translate::kDefaultBedrockBiome;
    wopt.level.level_name =
        o.level_name.empty() ? platform::path_to_utf8(o.input.stem()).value_or("Bridger World") : o.level_name;
    wopt.level.spawn = summary.placement->spawn;
    wopt.level.flat_world_layers = formats::bedrock::default_flat_world_layers();
    wopt.level.last_played =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    apply_target_level(target, wopt.level);
    if (target.bedrock != nullptr) wopt.chunk_version = static_cast<u8>(target.bedrock->chunk_version);

    BRIDGER_TRY(const ScratchDir build_dir, ScratchDir::create("world"));
    formats::bedrock::WorldWriteStats written;
    BRIDGER_TRY(const std::vector<u8> mcworld,
                formats::bedrock::write_new_world(bedrock, wopt, state_fn, build_dir.path(), &written));
    summary.chunks = written.chunks;
    summary.subchunks = written.subchunks;
    summary.block_entities = written.block_entities;
    auto validate = [&](const fs::path& written_file) -> Status {
        BRIDGER_TRY(const platform::MappedFile f, platform::MappedFile::open(written_file));
        BRIDGER_TRY(const ScratchDir check_dir, ScratchDir::create("check"));
        formats::bedrock::WorldWriteStats reread;
        BRIDGER_TRY_STATUS(formats::bedrock::validate_mcworld(f.data(), check_dir.path(), &reread));
        if (reread.subchunks != written.subchunks || reread.block_entities != written.block_entities ||
            reread.chunks != written.chunks)
            return make_error(ErrorCode::ValidationFailed,
                              std::format("re-read world differs: {} subchunks / {} block entities / {} chunks, expected "
                                          "{} / {} / {}",
                                          reread.subchunks, reread.block_entities, reread.chunks, written.subchunks,
                                          written.block_entities, written.chunks));
        return {};
    };
    BRIDGER_TRY_STATUS(step(o, 0.9f, "Saving"));
    BRIDGER_TRY_STATUS(platform::write_file_atomically(o.output, mcworld, validate, cancel_gate(o)));
    return summary;
}

// Bedrock world -> new Java world folder, one chunk column at a time (docs/formats/anvil.md,
// "Writing"; bedrock-world.md, "Reading worlds"). Workers read, translate and encode columns of
// one region; this thread adds them to the region in slot order, so the output does not
// depend on the thread count. The world is built in a temp folder next to the destination,
// validated (every chunk re-read) and renamed into place.
// Bedrock world -> Bedrock world for another game version (docs/versions.md, "Target versions"): every column is
// read (older versions upgraded on the way), rewritten for the target and written into a new world.
Result<ConvertSummary> convert_bedrock_world_to_bedrock(const ConvertOptions& o, ConvertSummary summary, const translate::MappingData& m,
                                                        u32 air) {
    if (o.into_world) return make_error(ErrorCode::InvalidArgument, "--into-world is not available for whole worlds");
    BRIDGER_TRY_STATUS(check_output(o));
    BRIDGER_TRY(const ScratchDir unpacked, ScratchDir::create("bedrockworld"));
    BRIDGER_TRY(const formats::bedrock::BedrockWorldReader reader, formats::bedrock::BedrockWorldReader::open_path(o.input, unpacked.path()));
    if (reader.overworld_chunks().empty()) return make_error(ErrorCode::Malformed, "the world has no Overworld chunks");
    summary.source_version = bedrock_world_version_info(reader.level().last_opened_version);
    BRIDGER_TRY(const OutputTarget target, resolve_target(o, FormatId::McWorld, summary));

    BRIDGER_TRY(const ScratchDir build, ScratchDir::create("bedrockout"));
    const fs::path dir = build.path() / "world";
    BRIDGER_TRY(formats::bedrock::WorldBuilder builder, formats::bedrock::WorldBuilder::create(dir));
    const auto t0 = std::chrono::steady_clock::now();
    const auto phase = [&t0](std::string_view what) {
        const auto now = std::chrono::steady_clock::now();
        log::debug("{}: {} ms", what, std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count());
    };
    BedrockStateNbt state_nbt(m);
    state_nbt.prepare_all();
    if (!target.bedrock->pinned)
        for (u32 st = 0; st < m.bedrock_state_count(); ++st) (void)target.bedrock_targets->state_nbt(*target.bedrock, st, m);
    const auto state_fn = target_state_nbt(target, m, state_nbt.fn());
    const auto lookup = bedrock_lookup(m);

    struct ColumnResult {
        std::optional<Error> error;
        formats::bedrock::EncodedColumn encoded;
        translate::ConversionReport report;
        std::vector<std::string> warnings;
        std::map<std::string, u64> unknown;
        u64 blocks = 0;
        bool done = false;
    };
    constexpr usize kBatch = 128;
    std::vector<ColumnResult> results(kBatch);
    std::map<std::string, usize> failures;
    std::string first_failure;
    const usize total = reader.chunks(Dimension::Overworld).size() + reader.chunks(Dimension::Nether).size() +
                        reader.chunks(Dimension::End).size();
    usize done_before = 0;
    for (const Dimension dim : kDimensions) {
    const std::vector<ChunkPos>& all = reader.chunks(dim);
    for (usize start = 0; start < all.size(); start += kBatch) {
        BRIDGER_TRY_STATUS(step(o, 0.85f * static_cast<float>(done_before + start) / static_cast<float>(total), "Converting chunks"));
        const usize count = std::min(kBatch, all.size() - start);
        parallel_for(count, o.threads, [&](usize i) {
            ColumnResult& r = results[i];
            r = ColumnResult{};
            const ChunkPos c = all[start + i];
            auto col = reader.read_column(c, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), m.bedrock_block_version(),
                                          lookup, air, bedrock_default_biome(dim), dim);
            if (!col) {
                r.error = std::move(col).error();
                return;
            }
            r.unknown = std::move(col->unknown_blocks);
            r.warnings = std::move(col->warnings);
            r.blocks = count_non_air(col->column, air);
            r.report.set_origin({c.x * 16, col->min_y, c.z * 16});
            if (auto rw = translate::rewrite_for_bedrock_target(col->column, *target.bedrock_targets, *target.bedrock, m, r.report); !rw) {
                r.error = std::move(rw).error();
                return;
            }
            auto encoded = formats::bedrock::WorldBuilder::encode_column(c, col->min_y, col->column, air, bedrock_default_biome(dim),
                                                                         state_fn, static_cast<u8>(target.bedrock->chunk_version), dim);
            if (!encoded) {
                r.error = std::move(encoded).error();
                return;
            }
            r.encoded = std::move(*encoded);
            r.done = true;
        });
        for (usize i = 0; i < count; ++i) {
            ColumnResult& r = results[i];
            if (r.error) {
                if (first_failure.empty()) first_failure = std::format("chunk {},{}: {}", all[start + i].x, all[start + i].z, r.error->describe());
                ++failures[std::string(to_string(r.error->code()))];
            } else if (r.done) {
                summary.report.merge(r.report);
                summary.warnings.insert(summary.warnings.end(), r.warnings.begin(), r.warnings.end());
                for (const auto& [name, n] : r.unknown)
                    summary.report.add(translate::IssueKind::UnmappedBlock, name + " (unknown Bedrock block)",
                                       {all[start + i].x * 16, 0, all[start + i].z * 16}, n);
                summary.blocks += r.blocks;
                BRIDGER_TRY_STATUS(builder.put(r.encoded));
            }
            r = ColumnResult{};
        }
    }
    done_before += all.size();
    }
    for (const auto& [reason, n] : failures) summary.warnings.push_back(std::format("{} chunks could not be converted ({})", n, reason));
    if (!first_failure.empty()) summary.warnings.push_back("first failed chunk: " + first_failure);
    if (builder.stats().chunks == 0) return make_error(ErrorCode::Malformed, "no chunk of the world could be converted");

    formats::bedrock::LevelDatOptions level;
    level.level_name = !o.level_name.empty() ? o.level_name
                       : !reader.level().level_name.empty() ? reader.level().level_name
                                                             : platform::path_to_utf8(o.output.stem()).value_or("Bridger World");
    level.spawn = reader.level().spawn;
    level.random_seed = reader.level().seed;
    level.generator = reader.level().generator;
    level.flat_world_layers = formats::bedrock::default_flat_world_layers();
    if (reader.level().local_player) BRIDGER_TRY_STATUS(builder.put_raw("~local_player", *reader.level().local_player));
    level.last_played =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    apply_target_level(target, level);
    phase("chunks converted");
    return finish_mcworld(o, std::move(summary), builder, dir, level, phase);
}

// Java world -> Java world for another game version: every chunk is read (older layouts and names upgraded on the
// way), rewritten for the target and written into a new world folder.
Result<ConvertSummary> convert_java_world_to_java(const ConvertOptions& o, ConvertSummary summary) {
    if (o.into_world) return make_error(ErrorCode::InvalidArgument, "--into-world is not available for whole worlds");
    BRIDGER_TRY_STATUS(check_output_directory(o));
    BRIDGER_TRY(const formats::java::JavaWorld world, formats::java::open_java_world(o.input));
    BRIDGER_TRY(const translate::MappingData* mapping, translate::MappingData::embedded());
    summary.source_data_version = world.level.data_version;
    summary.source_version = java_version_info(FormatId::JavaWorld, world.level.data_version, world.level.version_name, world.level.storage_format);
    warn_server_dimensions(world, summary);
    if (world.has_entity_regions) summary.warnings.push_back("entities (entities/*.mca) were not converted: entities are not written yet");
    if (world.overworld_regions.empty()) return make_error(ErrorCode::Malformed, "the world has no Overworld region files");
    BRIDGER_TRY(const OutputTarget target, resolve_target(o, FormatId::JavaWorld, summary));

    BRIDGER_TRY(const fs::path temp, platform::AtomicFileWriter::temp_path_for(o.output));
    struct TempTree {
        fs::path path;
        bool keep = false;
        ~TempTree() {
            std::error_code ec;
            if (!keep) fs::remove_all(path, ec);
        }
    } tree{temp};
    const bool modern_layout = target.java->data_version >= formats::java::kJava26_1;
    fs::create_directories(java_region_dir(temp, modern_layout, Dimension::Overworld));
    if (modern_layout) fs::create_directories(temp / "data" / "minecraft");
    const formats::java::LegacyTables legacy = legacy_tables(*mapping);
    const BlockState fallback_state = java_fallback(o.fallback);

    struct ChunkResult {
        enum class Kind : u8 { Empty, Failed, Partial, Done } kind = Kind::Empty;
        std::optional<Error> error;
        std::vector<u8> nbt;
        translate::ConversionReport report;
        std::vector<std::string> warnings;
        std::vector<std::string> features;
        u64 blocks = 0;
        usize block_entities = 0;
    };
    constexpr usize kBatch = 128;
    std::vector<ChunkResult> results(kBatch);
    usize chunks = 0, block_entities = 0, skipped_partial = 0, regions_done = 0;
    std::vector<std::string> experiments;
    std::map<std::string, usize> failures;
    std::string first_failure;
    const usize total_regions = world.overworld_regions.size() + world.nether_regions.size() + world.end_regions.size();
    for (const Dimension dim : kDimensions)
    for (const fs::path& region_path : java_regions(world, dim)) {
        const fs::path region_dir = java_region_dir(temp, modern_layout, dim);
        fs::create_directories(region_dir);
        BRIDGER_TRY_STATUS(step(o, 0.85f * static_cast<float>(regions_done++) / static_cast<float>(total_regions), "Converting chunks"));
        auto region = formats::java::RegionFile::open(region_path);
        if (!region) {
            ++failures[region.error().describe()];
            if (first_failure.empty()) first_failure = region.error().describe();
            continue;
        }
        formats::java::RegionWriter out(region->region_x(), region->region_z());
        for (usize batch = 0; batch < 1024; batch += kBatch) {
            parallel_for(kBatch, o.threads, [&](usize i) {
                ChunkResult& r = results[i];
                r = ChunkResult{};
                const u32 slot = static_cast<u32>(batch + i), lx = slot % 32, lz = slot / 32;
                if (!region->has_chunk(lx, lz)) return;
                auto fail = [&](Error e) {
                    r.kind = ChunkResult::Kind::Failed;
                    r.error = std::move(e);
                };
                auto raw = region->read_chunk(lx, lz);
                if (!raw) return fail(std::move(raw).error());
                auto chunk = formats::java::decode_chunk(**raw, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), {},
                                                         &legacy);
                if (!chunk) return fail(std::move(chunk).error());
                if (!chunk->full) {
                    r.kind = ChunkResult::Kind::Partial;
                    return;
                }
                r.warnings = std::move(chunk->warnings);
                r.blocks = count_non_air(chunk->column, kAirStateId);
                r.report.set_origin({chunk->pos.x * 16, chunk->min_y, chunk->pos.z * 16});
                for (const Entity& e : chunk->column.entities)
                    r.report.add(translate::IssueKind::UnmappedEntity, e.id,
                                 {static_cast<i32>(e.pos.x), static_cast<i32>(e.pos.y), static_cast<i32>(e.pos.z)});
                auto rw = translate::rewrite_for_java_target(chunk->column, *target.java_targets, *target.java, fallback_state, r.report);
                if (!rw) return fail(std::move(rw).error());
                r.features = std::move(rw->features);
                r.block_entities = chunk->column.block_entities.size();
                auto nbt = formats::java::encode_chunk(chunk->column, chunk->pos, chunk->min_y, target.java->data_version);
                if (!nbt) return fail(std::move(nbt).error());
                r.nbt = std::move(*nbt);
                r.kind = ChunkResult::Kind::Done;
            });
            for (usize i = 0; i < kBatch; ++i) {
                ChunkResult& r = results[i];
                switch (r.kind) {
                case ChunkResult::Kind::Empty: break;
                case ChunkResult::Kind::Partial: ++skipped_partial; break;
                case ChunkResult::Kind::Failed:
                    if (first_failure.empty())
                        first_failure = std::format("region {},{} chunk {},{}: {}", region->region_x(), region->region_z(), (batch + i) % 32,
                                                    (batch + i) / 32, r.error->describe());
                    ++failures[std::string(to_string(r.error->code()))];
                    break;
                case ChunkResult::Kind::Done:
                    summary.warnings.insert(summary.warnings.end(), r.warnings.begin(), r.warnings.end());
                    summary.report.merge(r.report);
                    summary.blocks += r.blocks;
                    block_entities += r.block_entities;
                    for (std::string& f : r.features)
                        if (std::find(experiments.begin(), experiments.end(), f) == experiments.end()) experiments.push_back(std::move(f));
                    BRIDGER_TRY_STATUS(out.add(static_cast<u32>((batch + i) % 32), static_cast<u32>((batch + i) / 32), r.nbt));
                    ++chunks;
                    break;
                }
                r = ChunkResult{};
            }
        }
        if (out.chunk_count() > 0) BRIDGER_TRY_STATUS(out.write(region_dir));
    }
    if (skipped_partial > 0)
        summary.warnings.push_back(std::format("{} partially generated chunks were skipped (the game generates them)", skipped_partial));
    for (const auto& [reason, n] : failures) summary.warnings.push_back(std::format("{} chunks could not be read ({})", n, reason));
    if (!first_failure.empty()) summary.warnings.push_back("first unreadable chunk: " + first_failure);
    if (chunks == 0) return make_error(ErrorCode::Malformed, "no chunk of the world could be converted");

    JavaWorldInfo info;
    info.level_name = !o.level_name.empty() ? o.level_name
                      : !world.level.level_name.empty() ? world.level.level_name
                                                         : platform::path_to_utf8(world.root.filename()).value_or("Bridger World");
    info.spawn = world.level.spawn.value_or(BlockPos{0, 64, 0});
    info.seed = world.level.seed;
    if (world.level.player) {
        if (world.level.data_version.value_or(0) <= target.java->data_version) info.player = world.level.player;
        else summary.warnings.push_back("the player was not converted: the target version is older than the world");
    }
    return finish_java_world(o, std::move(summary), tree.path, tree.keep, target, info, chunks, block_entities, experiments);
}

Result<ConvertSummary> convert_bedrock_world(const ConvertOptions& o, ConvertSummary summary, const translate::MappingData& m, u32 air) {
    if (o.into_world) return make_error(ErrorCode::InvalidArgument, "--into-world is not available for whole worlds");
    BRIDGER_TRY(const ScratchDir unpacked, ScratchDir::create("bedrockworld"));
    // A .mcworld or a world folder; either way only a copy is opened.
    BRIDGER_TRY(const formats::bedrock::BedrockWorldReader reader, formats::bedrock::BedrockWorldReader::open_path(o.input, unpacked.path()));
    if (reader.overworld_chunks().empty()) return make_error(ErrorCode::Malformed, "the world has no Overworld chunks");
    summary.source_version = bedrock_world_version_info(reader.level().last_opened_version);

    // Temp folder in the destination's directory (same volume: the final rename cannot cross volumes).
    BRIDGER_TRY(const fs::path temp, platform::AtomicFileWriter::temp_path_for(o.output));
    struct TempTree {
        fs::path path;
        bool keep = false;
        ~TempTree() {
            std::error_code ec;
            if (!keep) fs::remove_all(path, ec);
        }
    } tree{temp};
    BRIDGER_TRY(const OutputTarget target, resolve_target(o, FormatId::JavaWorld, summary));
    const bool modern_layout = target.java->data_version >= formats::java::kJava26_1; // dimensions/ folders since 26.1
    fs::create_directories(java_region_dir(temp, modern_layout, Dimension::Overworld));
    if (modern_layout) fs::create_directories(temp / "data" / "minecraft");
    const BlockState java_fallback_state = java_fallback(o.fallback);

    const auto lookup = bedrock_lookup(m);
    translate::TranslateOptions topt;
    topt.fallback = o.fallback;

    struct ColumnResult {
        std::optional<Error> error;
        std::vector<u8> nbt;
        translate::ConversionReport report;
        std::vector<std::string> warnings;
        std::map<std::string, u64> unknown;
        u64 blocks = 0;
        usize block_entities = 0;
        std::vector<std::string> features; // experimental features of the target its blocks need
        std::vector<u8> entity_nbt;        // the chunk of the entity region file, if it has entities
    };
    usize chunks = 0, block_entities = 0;
    std::vector<std::string> experiments;
    std::map<std::string, usize> failures;
    std::string first_failure;
    usize regions_done = 0, total_regions = 0;
    for (const Dimension dim : kDimensions) {
        std::set<std::pair<i32, i32>> r;
        for (const ChunkPos c : reader.chunks(dim)) r.insert({c.x >> 5, c.z >> 5});
        total_regions += r.size();
    }
    for (const Dimension dim : kDimensions) {
    std::map<std::pair<i32, i32>, std::vector<ChunkPos>> regions;
    for (const ChunkPos c : reader.chunks(dim)) regions[{c.x >> 5, c.z >> 5}].push_back(c);
    std::set<std::pair<i32, i32>> existing;
    for (const ChunkPos c : reader.chunks(dim)) existing.insert({c.x, c.z});
    const auto range = formats::bedrock::dimension_range(dim);
    const fs::path region_dir = java_region_dir(temp, modern_layout, dim);
    if (!regions.empty()) fs::create_directories(region_dir);
    for (const auto& [rpos, cols] : regions) {
        BRIDGER_TRY_STATUS(step(o, 0.9f * static_cast<float>(regions_done++) / static_cast<float>(total_regions), "Converting chunks"));
        formats::java::RegionWriter region(rpos.first, rpos.second);
        formats::java::RegionWriter entity_region(rpos.first, rpos.second);
        std::vector<ColumnResult> results(cols.size());
        parallel_for(cols.size(), o.threads, [&](usize i) {
            ColumnResult& r = results[i];
            const ChunkPos c = cols[i];
            auto col = reader.read_column(c, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(),
                                          m.bedrock_block_version(), lookup, air, bedrock_default_biome(dim), dim);
            if (!col) {
                r.error = std::move(col).error();
                return;
            }
            r.warnings = std::move(col->warnings);
            r.unknown = std::move(col->unknown_blocks);
            r.blocks = count_non_air(col->column, air);
            // Neighbours across the column border, from the adjacent columns (per-task cache).
            std::map<std::tuple<i32, i32, i32>, std::optional<std::array<u32, kSectionVolume>>> cache;
            translate::TranslateOptions local = topt;
            local.bedrock_outside = [&](BlockPos p) -> std::optional<u32> {
                const i32 wx = c.x * 16 + p.x, wy = col->min_y + p.y, wz = c.z * 16 + p.z;
                const i32 ncx = wx >> 4, ncz = wz >> 4, sy = wy >> 4;
                if (wy < range.min_y() || wy > range.max_y()) return std::nullopt;
                if (!existing.contains({ncx, ncz})) return std::nullopt;
                auto& slot = cache[{ncx, ncz, sy}];
                if (!slot) slot = reader.read_subchunk_blocks({ncx, ncz}, sy, lookup, air, dim).value_or(std::array<u32, kSectionVolume>{});
                return (*slot)[section_index(wx & 15, wy & 15, wz & 15)];
            };
            r.report.set_origin({c.x * 16, col->min_y, c.z * 16});
            auto java = translate::translate_to_java(col->column, m, local, r.report);
            if (!java) {
                r.error = std::move(java).error();
                return;
            }
            auto rw = translate::rewrite_for_java_target(*java, *target.java_targets, *target.java, java_fallback_state, r.report);
            if (!rw) {
                r.error = std::move(rw).error();
                return;
            }
            r.features = std::move(rw->features);
            r.block_entities = java->block_entities.size();
            auto nbt = formats::java::encode_chunk(*java, c, col->min_y, target.java->data_version);
            if (!nbt) {
                r.error = std::move(nbt).error();
                return;
            }
            r.nbt = std::move(*nbt);
            if (!java->entities.empty()) {
                auto entities = formats::java::encode_entity_chunk(java->entities, c, col->min_y, target.java->data_version);
                if (!entities) {
                    r.error = std::move(entities).error();
                    return;
                }
                r.entity_nbt = std::move(*entities);
            }
        });
        for (usize i = 0; i < cols.size(); ++i) {
            ColumnResult& r = results[i];
            if (r.error) {
                if (first_failure.empty())
                    first_failure = std::format("chunk {},{}: {}", cols[i].x, cols[i].z, r.error->describe());
                ++failures[std::string(to_string(r.error->code()))];
                continue;
            }
            summary.report.merge(r.report);
            summary.warnings.insert(summary.warnings.end(), r.warnings.begin(), r.warnings.end());
            for (const auto& [name, n] : r.unknown) summary.report.add(translate::IssueKind::UnmappedBlock, name + " (unknown Bedrock block)",
                                                                       {cols[i].x * 16, 0, cols[i].z * 16}, n);
            summary.blocks += r.blocks;
            block_entities += r.block_entities;
            for (std::string& f : r.features)
                if (std::find(experiments.begin(), experiments.end(), f) == experiments.end()) experiments.push_back(std::move(f));
            BRIDGER_TRY_STATUS(region.add(static_cast<u32>(cols[i].x & 31), static_cast<u32>(cols[i].z & 31), r.nbt));
            if (!r.entity_nbt.empty())
                BRIDGER_TRY_STATUS(entity_region.add(static_cast<u32>(cols[i].x & 31), static_cast<u32>(cols[i].z & 31), r.entity_nbt));
            ++chunks;
            r = ColumnResult{};
        }
        if (region.chunk_count() > 0) BRIDGER_TRY_STATUS(region.write(region_dir));
        if (entity_region.chunk_count() > 0) {
            const fs::path entities_dir = java_entities_dir(temp, modern_layout, dim);
            fs::create_directories(entities_dir);
            BRIDGER_TRY_STATUS(entity_region.write(entities_dir));
        }
    }
    }
    for (const auto& [reason, n] : failures) summary.warnings.push_back(std::format("{} chunks could not be converted ({})", n, reason));
    if (!first_failure.empty()) summary.warnings.push_back("first failed chunk: " + first_failure);
    if (chunks == 0) return make_error(ErrorCode::Malformed, "no chunk of the world could be converted");

    if (reader.level().generator != 1)
        summary.warnings.push_back("the Bedrock world is not an infinite world; chunks it never had are generated by Java's "
                                   "default (infinite) generator");
    JavaWorldInfo info;
    info.level_name = !o.level_name.empty() ? o.level_name
                      : !reader.level().level_name.empty() ? reader.level().level_name
                                                            : platform::path_to_utf8(o.output.filename()).value_or("Bridger World");
    info.spawn = reader.level().spawn;
    info.seed = reader.level().seed;
    if (reader.level().local_player)
        info.player = translate::bedrock_player_to_java(*reader.level().local_player, target.java->data_version, m, summary.report);
    return finish_java_world(o, std::move(summary), tree.path, tree.keep, target, info, chunks, block_entities, experiments);
}

} // namespace

// Palette entry -> Bedrock state index of the pinned palette. Entries from older versions ({name, val},
// numeric ids, or states with an older block version) go through the upgrade schemas first
// (translate/bedrock_upgrade.hpp); entries at the current version are looked up directly.
formats::java::LegacyTables legacy_tables(const translate::MappingData& m) {
    return {[&m](u16 id, u8 data) -> std::optional<std::string> {
                if (const auto st = m.legacy_block(id, data)) return m.java_state_string(*st);
                return std::nullopt;
            },
            [&m](u16 id, u16 damage) -> std::optional<std::string> {
                if (const auto i = m.legacy_item(id, damage)) return std::string(*i);
                return std::nullopt;
            }};
}

static_assert(translate::kLegacyIdKey == formats::bedrock::kLegacyIdEntryKey);

formats::bedrock::StateLookupFn bedrock_state_lookup(const translate::MappingData& m) {
    const translate::BedrockUpgrader* up = translate::BedrockUpgrader::embedded().value_or(nullptr);
    return [&m, up](io::nbt::NbtCompoundView entry) -> std::optional<u32> {
        auto direct = [&]() -> std::optional<u32> {
            const auto name = entry.find("name").as_string();
            if (!name) return std::nullopt;
            std::vector<translate::BedrockProperty> props;
            for (const io::nbt::NbtEntry& e : entry.find("states").entries()) {
                translate::BedrockProperty p;
                p.key = e.name;
                if (const auto b = e.value.as_byte()) { p.type = translate::NbtValueType::Byte; p.int_value = *b; }
                else if (const auto i = e.value.as_int()) { p.type = translate::NbtValueType::Int; p.int_value = *i; }
                else if (const auto s = e.value.as_string()) { p.type = translate::NbtValueType::String; p.str_value = *s; }
                else return std::nullopt;
                props.push_back(p);
            }
            return m.find_bedrock(*name, props);
        };
        const bool legacy = static_cast<bool>(entry.find("val")) || static_cast<bool>(entry.find(translate::kLegacyIdKey));
        const auto version = entry.find("version").as_int();
        const bool current = version && static_cast<u32>(*version) >= m.bedrock_block_version();
        if (!legacy && (current || !version))
            if (const auto r = direct()) return r;
        if (up == nullptr) return legacy ? std::nullopt : direct();
        const auto st = up->from_entry(entry);
        if (!st) return std::nullopt;
        const auto props = st->properties();
        return m.find_bedrock(st->name, props);
    };
}

Result<ViewStructure> load_view_structure(const fs::path& input) {
    BRIDGER_TRY(const std::string name, platform::path_to_utf8(input));
    BRIDGER_TRY(const platform::MappedFile file, platform::MappedFile::open(input));
    ViewStructure v;
    BRIDGER_TRY(v.format, formats::detect_format(input, file.data().first(std::min<usize>(file.size(), 16))));
    BRIDGER_TRY(const translate::MappingData* m, translate::MappingData::embedded());
    if (v.format == FormatId::McStructure) {
        const auto air = m->find_bedrock("minecraft:air", {});
        auto f = formats::bedrock::read_mcstructure(file.data(), std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(),
                                                    m->bedrock_block_version(), bedrock_state_lookup(*m), air.value_or(0));
        if (!f) return tl::unexpected(std::move(f).error().with_file(name));
        v.warnings = f->warnings;
        v.source_version = bedrock_block_version_info(f->block_version);
        BRIDGER_TRY(v.java, translate::translate_to_java(f->structure, *m, {}, v.report));
        return v;
    }
    if (!is_java_structure(v.format))
        return make_error(ErrorCode::NotImplemented, std::string(formats::info(v.format).name) + " cannot be opened as a structure");
    const formats::java::LegacyTables legacy = legacy_tables(*m);
    auto f = formats::read_java_structure(file.data(), v.format, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), &legacy);
    if (!f) return tl::unexpected(std::move(f).error().with_file(name));
    v.warnings = f->warnings;
    v.source_version = java_version_info(v.format, f->data_version);
    v.java = std::move(f->structure);
    return v;
}

Result<ConvertSummary> convert(const ConvertOptions& o) {
    ConvertSummary summary;
    BRIDGER_TRY(summary.output_format, output_format_of(o));
    BRIDGER_TRY_STATUS(check_output_is_not_input(o));
    if (formats::bedrock::is_bedrock_world_folder(o.input) && (!o.input_format || *o.input_format == FormatId::McWorld)) {
        summary.input_format = FormatId::McWorld;
        if (summary.output_format != FormatId::JavaWorld && summary.output_format != FormatId::McWorld)
            return make_error(ErrorCode::NotImplemented, "a Bedrock world converts to a Java world folder or to a .mcworld");
        if (summary.output_format == FormatId::JavaWorld) BRIDGER_TRY_STATUS(check_output_directory(o));
        BRIDGER_TRY(const translate::MappingData* mapping, translate::MappingData::embedded());
        summary.target_data_version = mapping->java_data_version();
        const auto air = mapping->find_bedrock("minecraft:air", {});
        if (!air) return make_error(ErrorCode::Malformed, "mapping data lacks air");
        if (summary.output_format == FormatId::McWorld) return convert_bedrock_world_to_bedrock(o, std::move(summary), *mapping, *air);
        return convert_bedrock_world(o, std::move(summary), *mapping, *air);
    }
    if (is_java_world_input(o.input) && (!o.input_format || *o.input_format == FormatId::JavaWorld)) {
        summary.input_format = FormatId::JavaWorld;
        return convert_java_world(o, std::move(summary));
    }
    BRIDGER_TRY(const std::string input_name, platform::path_to_utf8(o.input));
    BRIDGER_TRY(const platform::MappedFile input, platform::MappedFile::open(o.input)); // read-only: never modified
    if (o.input_format) summary.input_format = *o.input_format;
    else {
        BRIDGER_TRY(summary.input_format, formats::detect_format(o.input, input.data().first(std::min<usize>(input.size(), 16))));
    }
    if (summary.output_format == FormatId::JavaWorld) BRIDGER_TRY_STATUS(check_output_directory(o));
    else BRIDGER_TRY_STATUS(check_output(o));

    BRIDGER_TRY(const translate::MappingData* mapping, translate::MappingData::embedded());
    summary.target_data_version = mapping->java_data_version();
    const auto air = mapping->find_bedrock("minecraft:air", {});
    if (!air) return make_error(ErrorCode::Malformed, "mapping data lacks air");

    if (summary.input_format == FormatId::McWorld) {
        if (summary.output_format == FormatId::McWorld) return convert_bedrock_world_to_bedrock(o, std::move(summary), *mapping, *air);
        if (summary.output_format != FormatId::JavaWorld)
            return make_error(ErrorCode::NotImplemented, "a Bedrock world converts to a Java world folder or to a .mcworld");
        return convert_bedrock_world(o, std::move(summary), *mapping, *air);
    }

    const bool src_bedrock = summary.input_format == FormatId::McStructure;
    if (!src_bedrock && !is_java_structure(summary.input_format))
        return not_supported(summary);
    const bool out_bedrock = summary.output_format == FormatId::McWorld || summary.output_format == FormatId::McStructure;
    const bool out_java = is_java_structure(summary.output_format) && summary.output_format != FormatId::LegacySchematic;
    if (!out_bedrock && !out_java) return not_supported(summary);
    if (o.into_world && summary.output_format != FormatId::McWorld)
        return make_error(ErrorCode::InvalidArgument, "--into-world needs a .mcworld output");
    if (o.into_world && !o.placement.place_at)
        return make_error(ErrorCode::InvalidArgument,
                          "--into-world needs --place-at x,y,z (there is no safe default position in an existing world)");

    // 1. Read.
    BRIDGER_TRY_STATUS(step(o, 0.02f, "Reading"));
    Structure source;
    if (src_bedrock) {
        auto file = formats::bedrock::read_mcstructure(input.data(), std::make_shared<BlockStateRegistry>(),
                                                       std::make_shared<BiomeRegistry>(), mapping->bedrock_block_version(),
                                                       bedrock_lookup(*mapping), *air);
        if (!file) return tl::unexpected(std::move(file).error().with_file(input_name));
        summary.warnings = file->warnings;
        summary.source_version = bedrock_block_version_info(file->block_version);
        source = std::move(file->structure);
        summary.blocks = count_non_air(source, *air);
    } else {
        const formats::java::LegacyTables legacy = legacy_tables(*mapping);
        auto file = formats::read_java_structure(input.data(), summary.input_format, std::make_shared<BlockStateRegistry>(),
                                                 std::make_shared<BiomeRegistry>(), &legacy);
        if (!file) return tl::unexpected(std::move(file).error().with_file(input_name));
        summary.warnings = file->warnings;
        summary.source_data_version = file->data_version;
        summary.source_version = java_version_info(summary.input_format, file->data_version);
        if (file->data_version && static_cast<u32>(*file->data_version) != mapping->java_data_version() && out_bedrock)
            summary.warnings.push_back(std::format(
                "source DataVersion {} differs from the pinned Java version's {} ({}); renamed blocks are upgraded, and states "
                "unknown to Java {} are reported",
                *file->data_version, mapping->java_data_version(), mapping->java_version(), mapping->java_version()));
        source = std::move(file->structure);
        summary.blocks = count_non_air(source, kAirStateId);
    }

    // 2. Place (world outputs; before translation, in the source's id domain).
    BRIDGER_TRY_STATUS(step(o, 0.2f, "Placing"));
    if (summary.output_format == FormatId::McWorld) {
        const WorldBounds bounds{formats::bedrock::kOverworldMinY, formats::bedrock::kOverworldMaxY,
                                 formats::bedrock::kDefaultFlatSurfaceY};
        BRIDGER_TRY(summary.placement, place_in_new_world(source, o.placement, bounds, summary.report,
                                                          src_bedrock ? *air : kAirStateId));
    }

    // 3. Translate when the editions differ.
    BRIDGER_TRY_STATUS(step(o, 0.3f, "Translating blocks"));
    translate::TranslateOptions topt;
    topt.fallback = o.fallback;
    Structure target;
    if (!src_bedrock && out_bedrock) {
        BRIDGER_TRY(target, translate::translate_to_bedrock(source, *mapping, topt, summary.report));
    } else if (src_bedrock && out_java) {
        BRIDGER_TRY(target, translate::translate_to_java(source, *mapping, topt, summary.report));
    } else {
        target = std::move(source);
    }

    // 4. Rewrite for the target version, write, 5. validate + atomic move.
    BRIDGER_TRY(const OutputTarget version, resolve_target(o, summary.output_format, summary));
    BRIDGER_TRY_STATUS(step(o, 0.55f, "Writing and validating"));
    if (out_java) {
        BRIDGER_TRY(const translate::JavaRewrite rewritten,
                    translate::rewrite_for_java_target(target, *version.java_targets, *version.java, java_fallback(o.fallback), summary.report));
        for (const std::string& e : rewritten.features)
            summary.warnings.push_back(std::format("blocks of the experimental feature {} of Java {}: load it into a world with that "
                                                   "experiment switched on", e, version.java->name));
        return write_java_structure_output(o, std::move(summary), target, version.java->data_version);
    }
    BRIDGER_TRY(const u64 rewritten,
                translate::rewrite_for_bedrock_target(target, *version.bedrock_targets, *version.bedrock, *mapping, summary.report));
    (void)rewritten;
    return write_bedrock_output(o, std::move(summary), target, *mapping, *air, version);
}

Result<ConvertSummary> convert_schem_to_new_mcworld(const ConvertOptions& options) {
    ConvertOptions o = options;
    o.input_format = FormatId::SpongeSchem;
    o.output_format = FormatId::McWorld;
    return convert(o);
}

std::string summary_to_json(const ConvertSummary& s, const ConvertOptions& o) {
    using translate::IssueKind;
    std::string j = "{\n";
    j += std::format("  \"input\": \"{}\",\n", json_escape(platform::path_to_utf8(o.input).value_or("")));
    j += std::format("  \"output\": \"{}\",\n", json_escape(platform::path_to_utf8(o.output).value_or("")));
    j += std::format("  \"input_format\": \"{}\",\n  \"output_format\": \"{}\",\n", formats::info(s.input_format).name,
                     formats::info(s.output_format).name);
    // "exact" means nothing was lost: no report entry and no warning (warnings describe data that was
    // not converted, e.g. skipped chunks or dimensions). "blocks_exact" covers the report alone.
    j += std::format("  \"source_version\": \"{}\",\n", json_escape(describe(s.source_version)));
    j += std::format("  \"target_version\": \"{}\",\n", json_escape(s.target_version));
    j += std::format("  \"exact\": {},\n", s.report.exact() && s.warnings.empty() ? "true" : "false");
    j += std::format("  \"blocks_exact\": {},\n", s.report.exact() ? "true" : "false");
    j += std::format("  \"blocks\": {},\n  \"chunks\": {},\n  \"subchunks\": {},\n  \"block_entities\": {},\n", s.blocks,
                     s.chunks, s.subchunks, s.block_entities);
    if (s.placement)
        j += std::format("  \"origin\": [{}, {}, {}],\n  \"spawn\": [{}, {}, {}],\n", s.placement->origin.x,
                         s.placement->origin.y, s.placement->origin.z, s.placement->spawn.x, s.placement->spawn.y,
                         s.placement->spawn.z);
    j += "  \"warnings\": [";
    for (usize i = 0; i < s.warnings.size(); ++i) j += (i ? ", " : "") + std::string("\"") + json_escape(s.warnings[i]) + "\"";
    j += "],\n  \"issues\": {";
    bool first_kind = true;
    for (const IssueKind kind : {IssueKind::UnmappedBlock, IssueKind::LossyBlock, IssueKind::UnmappedBlockEntity,
                                 IssueKind::UnmappedEntity, IssueKind::UnmappedBiome, IssueKind::DroppedSecondaryLayer,
                                 IssueKind::RemovedBlockEntity, IssueKind::RemovedEntity, IssueKind::ClippedBlock}) {
        const auto* subjects = s.report.issues(kind);
        if (subjects == nullptr) continue;
        j += std::format("{}\n    \"{}\": [", first_kind ? "" : ",", translate::to_string(kind));
        first_kind = false;
        bool first = true;
        for (const auto& [subject, entry] : *subjects) {
            j += std::format("{}\n      {{\"subject\": \"{}\", \"count\": {}, \"examples\": [", first ? "" : ",",
                             json_escape(subject), entry.count);
            first = false;
            for (usize i = 0; i < entry.examples.size(); ++i)
                j += std::format("{}[{}, {}, {}]", i ? ", " : "", entry.examples[i].x, entry.examples[i].y,
                                 entry.examples[i].z);
            j += "]}";
        }
        j += "\n    ]";
    }
    j += first_kind ? "}\n}\n" : "\n  }\n}\n";
    return j;
}

} // namespace bridger::convert

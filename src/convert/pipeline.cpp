#include <bridger/convert/pipeline.hpp>
#include <bridger/core/log.hpp>
#include <bridger/core/parallel.hpp>
#include <bridger/formats/bedrock/mcstructure.hpp>
#include <bridger/formats/bedrock/mcworld/bedrock_world.hpp>
#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/detect.hpp>
#include <bridger/formats/java/anvil/chunk_codec.hpp>
#include <bridger/formats/java/anvil/java_world.hpp>
#include <bridger/formats/java/anvil/region_file.hpp>
#include <bridger/formats/java/legacy_schematic.hpp>
#include <bridger/formats/structure_file.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/zip_archive.hpp>
#include <bridger/platform/atomic_replace.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <random>

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

// Palette entry {name, states} -> Bedrock state index of the pinned palette.
formats::bedrock::StateLookupFn bedrock_lookup(const translate::MappingData& m) {
    return [&m](io::nbt::NbtCompoundView entry) -> std::optional<u32> {
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
}

u64 count_non_air(const Structure& s, u32 air) {
    u64 n = 0;
    for (u32 y = 0; y < s.size().y; ++y)
        for (u32 z = 0; z < s.size().z; ++z)
            for (u32 x = 0; x < s.size().x; ++x)
                if (s.block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)}) != air) ++n;
    return n;
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

// Java world -> new .mcworld, one chunk column at a time (docs/formats/anvil.md). Memory
// stays bounded by one region file mapping plus one column; the world is built in a
// scratch directory, validated there, zipped next to the output and moved atomically.
Result<ConvertSummary> convert_java_world(const ConvertOptions& o, ConvertSummary summary) {
    if (summary.output_format != FormatId::McWorld)
        return make_error(ErrorCode::NotImplemented, "a Java world can only be converted to a .mcworld so far");
    if (o.into_world) return make_error(ErrorCode::InvalidArgument, "--into-world is not available for whole worlds");
    BRIDGER_TRY_STATUS(check_output(o));
    BRIDGER_TRY(const formats::java::JavaWorld world, formats::java::open_java_world(o.input));
    BRIDGER_TRY(const translate::MappingData* mapping, translate::MappingData::embedded());
    summary.target_data_version = mapping->java_data_version();
    summary.source_data_version = world.level.data_version;
    const auto air = mapping->find_bedrock("minecraft:air", {});
    if (!air) return make_error(ErrorCode::Malformed, "mapping data lacks air");
    if (world.level.data_version && static_cast<u32>(*world.level.data_version) != mapping->java_data_version())
        summary.warnings.push_back(std::format("world DataVersion {} differs from the pinned Java version's {} ({})",
                                               *world.level.data_version, mapping->java_data_version(),
                                               mapping->java_version()));
    if (world.has_nether) summary.warnings.push_back("the Nether was not converted: only the Overworld is supported");
    if (world.has_end) summary.warnings.push_back("the End was not converted: only the Overworld is supported");
    if (world.has_entity_regions) summary.warnings.push_back("entities (entities/*.mca) were not converted: entities are not written yet");
    if (world.overworld_regions.empty()) return make_error(ErrorCode::Malformed, "the world has no Overworld region files");

    BRIDGER_TRY(const ScratchDir build, ScratchDir::create("javaworld"));
    const fs::path dir = build.path() / "world";
    BRIDGER_TRY(formats::bedrock::WorldBuilder builder, formats::bedrock::WorldBuilder::create(dir));
    const auto t0 = std::chrono::steady_clock::now();
    const auto phase = [&t0](std::string_view what) {
        const auto now = std::chrono::steady_clock::now();
        log::debug("{}: {} ms", what, std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count());
    };
    BedrockStateNbt state_nbt(*mapping);
    state_nbt.prepare_all(); // read-only afterwards: shared by the worker threads without locks
    const auto state_fn = state_nbt.fn();
    translate::TranslateOptions topt;
    topt.fallback = o.fallback;
    usize skipped_partial = 0, clipped = 0;
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
    for (const fs::path& region_path : world.overworld_regions) {
        auto region = formats::java::RegionFile::open(region_path);
        if (!region) {
            ++failures[region.error().describe()];
            if (first_failure.empty()) first_failure = region.error().describe();
            continue;
        }
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
                auto chunk = formats::java::decode_chunk(**raw, std::make_shared<BlockStateRegistry>(),
                                                         std::make_shared<BiomeRegistry>());
                if (!chunk) return fail(std::move(chunk).error());
                if (!chunk->full) {
                    r.kind = ChunkResult::Kind::Partial;
                    return;
                }
                r.warnings = std::move(chunk->warnings);
                r.blocks = count_non_air(chunk->column, kAirStateId);
                r.report.set_origin({chunk->pos.x * 16, chunk->min_y, chunk->pos.z * 16});
                auto bedrock = translate::translate_to_bedrock(chunk->column, *mapping, topt, r.report);
                if (!bedrock) return fail(std::move(bedrock).error());
                auto encoded = formats::bedrock::WorldBuilder::encode_column(chunk->pos, chunk->min_y, *bedrock, *air,
                                                                             translate::kDefaultBedrockBiome, state_fn);
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
                    clipped += r.encoded.clipped_blocks;
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
    if (clipped > 0) summary.warnings.push_back(std::format("{} blocks outside the Bedrock build height were dropped", clipped));
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
    level.flat_world_layers = formats::bedrock::default_flat_world_layers();
    level.last_played =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    phase("chunks converted");
    BRIDGER_TRY_STATUS(builder.finish(level));
    phase("database compacted");
    const formats::bedrock::WorldWriteStats written = builder.stats();
    summary.chunks = written.chunks;
    summary.subchunks = written.subchunks;
    summary.block_entities = written.block_entities;

    // Validate the database before zipping, then the zip itself before the move.
    formats::bedrock::WorldWriteStats reread;
    BRIDGER_TRY_STATUS(formats::bedrock::validate_world_directory(dir, &reread));
    if (reread.subchunks != written.subchunks || reread.block_entities != written.block_entities ||
        reread.chunks != written.chunks)
        return make_error(ErrorCode::ValidationFailed, "re-read world differs from what was written");
    phase("world validated");
    BRIDGER_TRY(const fs::path temp, platform::AtomicFileWriter::temp_path_for(o.output));
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
    BRIDGER_TRY_STATUS(writer.commit(validate));
    phase("moved into place");
    return summary;
}

} // namespace

Result<ConvertSummary> convert(const ConvertOptions& o) {
    ConvertSummary summary;
    if (is_java_world_input(o.input) && (!o.input_format || *o.input_format == FormatId::JavaWorld)) {
        summary.input_format = FormatId::JavaWorld;
        if (o.output_format) summary.output_format = *o.output_format;
        else {
            BRIDGER_TRY(const std::string ext, platform::path_to_utf8(o.output.extension()));
            const auto f = formats::format_from_extension(ext);
            if (!f) return make_error(ErrorCode::UnsupportedFormat, "cannot tell the output format from '" + ext + "'; use --format");
            summary.output_format = *f;
        }
        return convert_java_world(o, std::move(summary));
    }
    BRIDGER_TRY(const std::string input_name, platform::path_to_utf8(o.input));
    BRIDGER_TRY(const platform::MappedFile input, platform::MappedFile::open(o.input)); // read-only: never modified

    // Formats.
    if (o.input_format) summary.input_format = *o.input_format;
    else {
        BRIDGER_TRY(summary.input_format, formats::detect_format(o.input, input.data().first(std::min<usize>(input.size(), 16))));
    }
    if (o.output_format) summary.output_format = *o.output_format;
    else {
        BRIDGER_TRY(const std::string ext, platform::path_to_utf8(o.output.extension()));
        const auto f = formats::format_from_extension(ext);
        if (!f) return make_error(ErrorCode::UnsupportedFormat, "cannot tell the output format from '" + ext + "'; use --format");
        summary.output_format = *f;
    }
    BRIDGER_TRY_STATUS(check_output(o));

    BRIDGER_TRY(const translate::MappingData* mapping, translate::MappingData::embedded());
    summary.target_data_version = mapping->java_data_version();
    const auto air = mapping->find_bedrock("minecraft:air", {});
    if (!air) return make_error(ErrorCode::Malformed, "mapping data lacks air");

    if (!is_java_structure(summary.input_format) || (summary.output_format != FormatId::McWorld &&
                                                     summary.output_format != FormatId::McStructure))
        return make_error(ErrorCode::NotImplemented, "converting " + std::string(formats::info(summary.input_format).name) +
                                                         " to " + std::string(formats::info(summary.output_format).name) +
                                                         " is not supported yet");
    if (o.into_world && summary.output_format != FormatId::McWorld)
        return make_error(ErrorCode::InvalidArgument, "--into-world needs a .mcworld output");
    if (o.into_world && !o.placement.place_at)
        return make_error(ErrorCode::InvalidArgument,
                          "--into-world needs --place-at x,y,z (there is no safe default position in an existing world)");

    // 1. Read.
    const formats::java::LegacyTables legacy{
        [mapping](u16 id, u8 data) -> std::optional<std::string> {
            if (const auto s = mapping->legacy_block(id, data)) return mapping->java_state_string(*s);
            return std::nullopt;
        },
        [mapping](u16 id, u16 damage) -> std::optional<std::string> {
            if (const auto i = mapping->legacy_item(id, damage)) return std::string(*i);
            return std::nullopt;
        }};
    auto file = formats::read_java_structure(input.data(), summary.input_format, std::make_shared<BlockStateRegistry>(),
                                             std::make_shared<BiomeRegistry>(), &legacy);
    if (!file) return tl::unexpected(std::move(file).error().with_file(input_name));
    summary.warnings = file->warnings;
    summary.source_data_version = file->data_version;
    if (file->data_version && static_cast<u32>(*file->data_version) != mapping->java_data_version())
        summary.warnings.push_back(std::format(
            "source DataVersion {} differs from the pinned Java version's {} ({}); blocks are matched by name, and states "
            "unknown to Java {} are reported",
            *file->data_version, mapping->java_data_version(), mapping->java_version(), mapping->java_version()));
    Structure& java = file->structure;
    summary.blocks = count_non_air(java, kAirStateId);

    // 2. Place (world outputs; on the Java structure, before translation).
    const WorldBounds bounds{formats::bedrock::kOverworldMinY, formats::bedrock::kOverworldMaxY,
                             formats::bedrock::kDefaultFlatSurfaceY};
    if (summary.output_format == FormatId::McWorld) {
        BRIDGER_TRY(summary.placement, place_in_new_world(java, o.placement, bounds, summary.report));
    }

    // 3. Translate.
    translate::TranslateOptions topt;
    topt.fallback = o.fallback;
    BRIDGER_TRY(const Structure bedrock, translate::translate_to_bedrock(java, *mapping, topt, summary.report));
    BedrockStateNbt state_nbt(*mapping);

    // 4. Write and 5. validate + atomic move.
    if (summary.output_format == FormatId::McStructure) {
        BRIDGER_TRY(const std::vector<u8> bytes, formats::bedrock::write_mcstructure(bedrock, *air, state_nbt.fn()));
        const auto lookup = bedrock_lookup(*mapping);
        auto validate = [&](const fs::path& written) -> Status {
            BRIDGER_TRY(const platform::MappedFile f, platform::MappedFile::open(written));
            BRIDGER_TRY(const formats::bedrock::McStructureFile back,
                        formats::bedrock::read_mcstructure(f.data(), std::make_shared<BlockStateRegistry>(),
                                                           std::make_shared<BiomeRegistry>(), mapping->bedrock_block_version(),
                                                           lookup, *air));
            if (back.structure.size() != bedrock.size() || back.structure.block_entities.size() != bedrock.block_entities.size() ||
                !back.warnings.empty())
                return make_error(ErrorCode::ValidationFailed, "re-read .mcstructure differs from what was written");
            return {};
        };
        BRIDGER_TRY_STATUS(platform::write_file_atomically(o.output, bytes, validate));
        summary.block_entities = bedrock.block_entities.size();
        return summary;
    }

    if (o.into_world) {
        BRIDGER_TRY(const platform::MappedFile world, platform::MappedFile::open(*o.into_world)); // read-only
        formats::bedrock::PlaceOptions po;
        po.origin = summary.placement->origin;
        po.air_replaces = o.placement.air == AirMode::Replace;
        po.clear_entities = o.placement.clear_entities;
        po.air_state = *air;
        po.default_biome = translate::kDefaultBedrockBiome;
        BRIDGER_TRY(const ScratchDir work, ScratchDir::create("place"));
        formats::bedrock::PlaceStats stats;
        BRIDGER_TRY(const std::vector<u8> out,
                    formats::bedrock::place_into_world(world.data(), bedrock, po, state_nbt.fn(), work.path(), &stats));
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
        BRIDGER_TRY_STATUS(platform::write_file_atomically(o.output, out, validate));
        return summary;
    }

    // New world.
    const translate::BedrockProperty no_infiniburn{"infiniburn_bit", translate::NbtValueType::Byte, 0, {}};
    const auto bedrock_block = mapping->find_bedrock("minecraft:bedrock", std::span(&no_infiniburn, 1));
    const auto dirt = mapping->find_bedrock("minecraft:dirt", {});
    const auto grass = mapping->find_bedrock("minecraft:grass_block", {});
    if (!bedrock_block || !dirt || !grass) return make_error(ErrorCode::Malformed, "mapping data lacks the superflat layer blocks");
    formats::bedrock::NewWorldOptions wopt;
    wopt.origin = summary.placement->origin;
    wopt.air_replaces = o.placement.air == AirMode::Replace;
    wopt.air_state = *air;
    wopt.flat_layers = {{*bedrock_block, 1}, {*dirt, 2}, {*grass, 1}};
    wopt.default_biome = translate::kDefaultBedrockBiome;
    wopt.level.level_name =
        o.level_name.empty() ? platform::path_to_utf8(o.input.stem()).value_or("Bridger World") : o.level_name;
    wopt.level.spawn = summary.placement->spawn;
    wopt.level.flat_world_layers = formats::bedrock::default_flat_world_layers();
    wopt.level.last_played =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    BRIDGER_TRY(const ScratchDir build_dir, ScratchDir::create("world"));
    formats::bedrock::WorldWriteStats written;
    BRIDGER_TRY(const std::vector<u8> mcworld,
                formats::bedrock::write_new_world(bedrock, wopt, state_nbt.fn(), build_dir.path(), &written));
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
    BRIDGER_TRY_STATUS(platform::write_file_atomically(o.output, mcworld, validate));
    return summary;
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
    j += std::format("  \"exact\": {},\n", s.report.exact() ? "true" : "false");
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

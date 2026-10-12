#include <bridger/convert/pipeline.hpp>
#include <bridger/formats/bedrock/mcworld/bedrock_world.hpp>
#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/java/sponge_schem.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/platform/atomic_replace.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>

#include <chrono>
#include <format>
#include <random>

namespace bridger::convert {
namespace fs = std::filesystem;
namespace {

// A scratch directory under %TEMP%, removed afterwards.
class ScratchDir {
public:
    static Result<ScratchDir> create(std::string_view purpose) {
        std::random_device rd;
        std::error_code ec;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const fs::path p = fs::temp_directory_path(ec) /
                               std::format("bridger-{}-{:08x}{:08x}", purpose, rd(), rd());
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

} // namespace

Result<ConvertSummary> convert_schem_to_new_mcworld(const ConvertOptions& o) {
    ConvertSummary summary;
    BRIDGER_TRY(const std::string input_name, platform::path_to_utf8(o.input));

    // 1. Read (inputs are memory-mapped read-only: never modified).
    BRIDGER_TRY(const platform::MappedFile input, platform::MappedFile::open(o.input));
    auto blocks = std::make_shared<BlockStateRegistry>();
    auto biomes = std::make_shared<BiomeRegistry>();
    auto schem = formats::java::read_sponge_schem(input.data(), blocks, biomes);
    if (!schem) return tl::unexpected(std::move(schem).error().with_file(input_name));
    summary.warnings = schem->info.warnings;
    summary.source_data_version = schem->info.data_version;

    BRIDGER_TRY(const translate::MappingData* mapping, translate::MappingData::embedded());
    summary.target_data_version = mapping->java_data_version();
    if (schem->info.data_version && static_cast<u32>(*schem->info.data_version) != mapping->java_data_version())
        summary.warnings.push_back(std::format(
            "schematic DataVersion {} differs from the pinned Java version's {} ({}); blocks are matched by name, "
            "and states unknown to Java {} are reported",
            *schem->info.data_version, mapping->java_data_version(), mapping->java_version(), mapping->java_version()));

    // 2. Place (on the Java structure, before translation).
    const WorldBounds bounds{formats::bedrock::kOverworldMinY, formats::bedrock::kOverworldMaxY,
                             formats::bedrock::kDefaultFlatSurfaceY};
    Structure& java = schem->structure;
    BRIDGER_TRY(summary.placement, place_in_new_world(java, o.placement, bounds, summary.report));

    // 3. Translate.
    translate::TranslateOptions topt;
    topt.fallback = o.fallback;
    BRIDGER_TRY(const Structure bedrock, translate::translate_to_bedrock(java, *mapping, topt, summary.report));

    // 4. Write the world into a scratch directory, zip it.
    const auto air = mapping->find_bedrock("minecraft:air", {});
    const translate::BedrockProperty no_infiniburn{"infiniburn_bit", translate::NbtValueType::Byte, 0, {}};
    const auto bedrock_block = mapping->find_bedrock("minecraft:bedrock", std::span(&no_infiniburn, 1));
    const auto dirt = mapping->find_bedrock("minecraft:dirt", {});
    const auto grass = mapping->find_bedrock("minecraft:grass_block", {});
    if (!air || !bedrock_block || !dirt || !grass)
        return make_error(ErrorCode::Malformed, "mapping data lacks the superflat layer blocks");

    std::vector<std::optional<std::vector<u8>>> nbt_cache(mapping->bedrock_state_count());
    const formats::bedrock::StateNbtFn state_nbt = [&](u32 state) -> Result<std::span<const u8>> {
        if (state >= nbt_cache.size()) return make_error(ErrorCode::InvalidArgument, "unknown Bedrock state");
        if (!nbt_cache[state]) {
            const translate::BedrockState& bs = mapping->bedrock(state);
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
            w.write_int("version", static_cast<i32>(mapping->bedrock_block_version()));
            w.end_compound();
            BRIDGER_TRY(nbt_cache[state], std::move(w).finish());
        }
        return std::span<const u8>(*nbt_cache[state]);
    };

    formats::bedrock::NewWorldOptions wopt;
    wopt.origin = summary.placement.origin;
    wopt.air_replaces = o.placement.air == AirMode::Replace;
    wopt.air_state = *air;
    wopt.flat_layers = {{*bedrock_block, 1}, {*dirt, 2}, {*grass, 1}};
    wopt.default_biome = translate::kDefaultBedrockBiome;
    wopt.level.level_name = o.level_name.empty() ? platform::path_to_utf8(o.input.stem()).value_or("Bridger World") : o.level_name;
    wopt.level.spawn = summary.placement.spawn;
    wopt.level.flat_world_layers = formats::bedrock::default_flat_world_layers();
    wopt.level.last_played =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    BRIDGER_TRY(const ScratchDir build_dir, ScratchDir::create("world"));
    formats::bedrock::WorldWriteStats written;
    BRIDGER_TRY(const std::vector<u8> mcworld,
                formats::bedrock::write_new_world(bedrock, wopt, state_nbt, build_dir.path(), &written));
    summary.chunks = written.chunks;
    summary.subchunks = written.subchunks;
    summary.block_entities = written.block_entities;
    for (u32 y = 0; y < java.size().y; ++y)
        for (u32 z = 0; z < java.size().z; ++z)
            for (u32 x = 0; x < java.size().x; ++x)
                if (java.block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)}) != kAirStateId) ++summary.blocks;

    // 5. Validate (re-read) and move into place atomically (docs/safety.md).
    if (!o.overwrite && fs::exists(o.output))
        return make_error(ErrorCode::AlreadyExists,
                          "output exists; pass --overwrite to replace it: " + platform::path_to_utf8(o.output).value_or("?"));
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

std::string summary_to_json(const ConvertSummary& s, const ConvertOptions& o) {
    using translate::IssueKind;
    std::string j = "{\n";
    j += std::format("  \"input\": \"{}\",\n", json_escape(platform::path_to_utf8(o.input).value_or("")));
    j += std::format("  \"output\": \"{}\",\n", json_escape(platform::path_to_utf8(o.output).value_or("")));
    j += std::format("  \"exact\": {},\n", s.report.exact() ? "true" : "false");
    j += std::format("  \"blocks\": {},\n  \"chunks\": {},\n  \"subchunks\": {},\n  \"block_entities\": {},\n", s.blocks,
                     s.chunks, s.subchunks, s.block_entities);
    j += std::format("  \"origin\": [{}, {}, {}],\n  \"spawn\": [{}, {}, {}],\n", s.placement.origin.x,
                     s.placement.origin.y, s.placement.origin.z, s.placement.spawn.x, s.placement.spawn.y,
                     s.placement.spawn.z);
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

// Bedrock world reader. Formats and sources: docs/formats/bedrock-world.md ("Reading worlds").

#include <bridger/formats/bedrock/mcworld/bedrock_world_reader.hpp>
#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/bedrock/mcworld/leveldb_store.hpp>
#include <bridger/formats/bedrock/mcworld/subchunk_codec.hpp>
#include <bridger/io/endian.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/zip_archive.hpp>
#include <bridger/platform/paths.hpp>

#include <algorithm>
#include <fstream>
#include <unordered_map>

namespace bridger::formats::bedrock {
namespace fs = std::filesystem;
using io::nbt::NbtCompoundView;
using io::nbt::NbtView;

namespace {

constexpr u8 kLegacyVersionTag = 0x76; // "v": chunk version before 1.16.100 [1]
constexpr u8 kEntityTag = 0x32;
constexpr u8 kData3DTag = 0x2B;

// One biome storage of Data3D (runtime palette of i32 ids), Chunker PaletteUtil.readChunkPalette:
// bits 0 = no index words and no palette count, one id; bits 127 = no data for this subchunk.
// Fills `out` (Bridger order) and returns false for "no data".
Result<bool> read_biome_storage(std::span<const u8> d, usize& p, std::array<u32, kSectionVolume>& out) {
    auto need = [&](usize n) { return p + n <= d.size(); };
    if (!need(1)) return make_error(ErrorCode::Malformed, "Data3D truncated");
    const u8 header = d[p++];
    const u32 bits = header >> 1;
    if (bits == 127) return false;
    if (bits > 16) return make_error(ErrorCode::Malformed, "Data3D: invalid biome bit width");
    const u32 per_word = bits == 0 ? 0 : 32 / bits;
    const usize words = per_word == 0 ? 0 : (kSectionVolume + per_word - 1) / per_word;
    if (!need(words * 4)) return make_error(ErrorCode::Malformed, "Data3D truncated");
    const usize words_at = p;
    p += words * 4;
    u32 count = 1;
    if (bits != 0) {
        if (!need(4)) return make_error(ErrorCode::Malformed, "Data3D truncated");
        const i32 c = io::load_le<i32>(d.data() + p);
        p += 4;
        if (c < 1 || c > 4096) return make_error(ErrorCode::Malformed, "Data3D: invalid biome palette size");
        count = static_cast<u32>(c);
    }
    if (!need(usize{count} * 4)) return make_error(ErrorCode::Malformed, "Data3D truncated");
    std::vector<u32> palette(count);
    for (u32 i = 0; i < count; ++i) palette[i] = static_cast<u32>(io::load_le<i32>(d.data() + p + usize{i} * 4));
    p += usize{count} * 4;
    if (bits == 0) {
        out.fill(palette[0]);
        return true;
    }
    const u32 mask = (1u << bits) - 1;
    for (usize i = 0; i < kSectionVolume; ++i) { // i = x*256 + z*16 + y
        const u32 word = io::load_le<u32>(d.data() + words_at + (i / per_word) * 4);
        const u32 v = (word >> ((i % per_word) * bits)) & mask;
        if (v >= count) return make_error(ErrorCode::Malformed, "Data3D: biome index outside the palette");
        const usize x = i >> 8, z = (i >> 4) & 15, y = i & 15;
        out[section_index(static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z))] = palette[v];
    }
    return true;
}

} // namespace

struct BedrockWorldReader::Impl {
    fs::path dir;
    std::optional<LevelDbStore> db;
    BedrockLevelInfo level;
    std::vector<ChunkPos> chunks;
    bool other_dimensions = false;
    usize entities = 0;
};

BedrockWorldReader::BedrockWorldReader() : impl_(std::make_unique<Impl>()) {}
BedrockWorldReader::BedrockWorldReader(BedrockWorldReader&&) noexcept = default;
BedrockWorldReader& BedrockWorldReader::operator=(BedrockWorldReader&&) noexcept = default;
BedrockWorldReader::~BedrockWorldReader() = default;

const BedrockLevelInfo& BedrockWorldReader::level() const noexcept { return impl_->level; }
const std::vector<ChunkPos>& BedrockWorldReader::overworld_chunks() const noexcept { return impl_->chunks; }
bool BedrockWorldReader::has_other_dimensions() const noexcept { return impl_->other_dimensions; }
usize BedrockWorldReader::entity_count() const noexcept { return impl_->entities; }

Result<BedrockWorldReader> BedrockWorldReader::open(std::span<const u8> mcworld, const fs::path& work_dir) {
    BRIDGER_TRY(const std::vector<io::ZipEntry> entries, io::read_zip(mcworld));
    // Worlds exported by the game sometimes sit in one top-level folder: find level.dat.
    std::string prefix;
    bool found = false;
    for (const io::ZipEntry& e : entries)
        if (e.name == "level.dat" || e.name.ends_with("/level.dat")) {
            if (!found || e.name.size() < prefix.size() + 9) prefix = e.name.substr(0, e.name.size() - 9);
            found = true;
        }
    if (!found) return make_error(ErrorCode::Malformed, "mcworld: no level.dat in the archive");
    BedrockWorldReader r;
    r.impl_->dir = work_dir;
    std::vector<u8> level_bytes;
    for (const io::ZipEntry& e : entries) {
        if (!e.name.starts_with(prefix)) continue;
        const std::string rel = e.name.substr(prefix.size());
        if (rel == "level.dat") level_bytes = e.data;
        if (!rel.starts_with("db/")) continue;
        BRIDGER_TRY(const fs::path p, platform::path_from_utf8(rel));
        fs::create_directories((work_dir / p).parent_path());
        std::ofstream out(work_dir / p, std::ios::binary);
        out.write(reinterpret_cast<const char*>(e.data.data()), static_cast<std::streamsize>(e.data.size()));
        if (!out) return make_error(ErrorCode::IoError, "cannot unpack " + rel);
    }

    // level.dat: i32 storage version, i32 length, little-endian NBT.
    if (level_bytes.size() < 8) return make_error(ErrorCode::Malformed, "level.dat too short");
    BRIDGER_TRY(const io::nbt::NbtDocument ld,
                io::nbt::NbtDocument::parse(std::span<const u8>(level_bytes).subspan(8), Endian::Little));
    const NbtCompoundView c = ld.root().entries();
    if (const auto n = c.find("LevelName").as_string()) r.impl_->level.level_name = std::string(*n);
    const auto sx = c.find("SpawnX").as_int(), sy = c.find("SpawnY").as_int(), sz = c.find("SpawnZ").as_int();
    if (sx && sy && sz) r.impl_->level.spawn = {*sx, *sy, *sz};
    r.impl_->level.seed = c.find("RandomSeed").as_long().value_or(0);
    r.impl_->level.generator = c.find("Generator").as_int().value_or(1);

    if (!fs::exists(work_dir / "db" / "CURRENT")) return make_error(ErrorCode::Malformed, "mcworld: no LevelDB database (db/)");
    BRIDGER_TRY(LevelDbStore db, LevelDbStore::open(work_dir / "db", /*create_new=*/false));
    Impl& m = *r.impl_;
    BRIDGER_TRY_STATUS(db.for_each([&](std::string_view key, std::span<const u8>) {
        if (key.size() == 9 && (static_cast<u8>(key[8]) == static_cast<u8>(ChunkTag::Version) ||
                                static_cast<u8>(key[8]) == kLegacyVersionTag)) {
            m.chunks.push_back({io::load_le<i32>(key.data()), io::load_le<i32>(key.data() + 4)});
        } else if (key.size() == 13 && (static_cast<u8>(key[12]) == static_cast<u8>(ChunkTag::Version) ||
                                        static_cast<u8>(key[12]) == kLegacyVersionTag)) {
            m.other_dimensions = true;
        } else if (key.starts_with("actorprefix")) {
            ++m.entities;
        } else if (key.size() == 9 && static_cast<u8>(key[8]) == kEntityTag) {
            ++m.entities; // legacy storage: at least one entity
        }
    }));
    std::sort(m.chunks.begin(), m.chunks.end(), [](ChunkPos a, ChunkPos b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
    m.chunks.erase(std::unique(m.chunks.begin(), m.chunks.end()), m.chunks.end());
    m.db.emplace(std::move(db));
    return r;
}

Result<BedrockColumn> BedrockWorldReader::read_column(ChunkPos pos, std::shared_ptr<BlockStateRegistry> blocks,
                                                      std::shared_ptr<BiomeRegistry> biomes, u32 bedrock_data_version,
                                                      const StateLookupFn& lookup, u32 air, i32 default_biome) const {
    const LevelDbStore& db = *impl_->db;
    BedrockColumn out;
    out.pos = pos;
    out.min_y = kOverworldMinY;
    BRIDGER_TRY(out.column, Structure::create(std::move(blocks), std::move(biomes),
                                              Extent{16, static_cast<u32>(kOverworldSubchunks * 16), 16},
                                              {Edition::Bedrock, bedrock_data_version}));
    Structure& col = out.column;
    for (u32 i = 0; i < col.sections_y(); ++i) col.section(0, i, 0).blocks().fill(air);
    const std::string where = "chunk " + std::to_string(pos.x) + "," + std::to_string(pos.z);

    // Palette entry (raw LE NBT) -> pinned-palette index; unknown entries read as air and are counted.
    std::unordered_map<std::string, u32> resolved;
    auto resolve = [&](const std::vector<u8>& raw, u64 uses) -> u32 {
        const std::string k(raw.begin(), raw.end());
        if (const auto it = resolved.find(k); it != resolved.end()) return it->second;
        u32 v = air;
        std::string name = "?";
        if (auto doc = io::nbt::NbtDocument::parse(raw, Endian::Little)) {
            const NbtCompoundView e = doc->root().entries();
            name = std::string(e.find("name").as_string().value_or("?"));
            if (const auto idx = lookup(e)) v = *idx;
            else out.unknown_blocks[name] += uses;
        } else {
            out.unknown_blocks["(unreadable palette entry)"] += uses;
        }
        resolved.emplace(k, v);
        return v;
    };

    std::array<u32, kSectionVolume> values{};
    for (i32 sy = kOverworldMinSubchunk; sy < kOverworldMinSubchunk + kOverworldSubchunks; ++sy) {
        BRIDGER_TRY(const auto value, db.get(chunk_key(pos.x, pos.z, Dimension::Overworld, ChunkTag::SubChunkPrefix, static_cast<i8>(sy))));
        if (!value) continue;
        auto sub = decode_subchunk_raw(*value);
        if (!sub) {
            out.warnings.push_back(where + " subchunk " + std::to_string(sy) + " could not be read (" +
                                   sub.error().describe() + "); it was left empty");
            continue;
        }
        Section& sec = col.section(0, static_cast<u32>(sy - kOverworldMinSubchunk), 0);
        for (usize l = 0; l < sub->layers.size() && l < 2; ++l) {
            const RawSubchunkLayer& layer = sub->layers[l];
            std::vector<u64> uses(layer.palette.size(), 0);
            for (const u16 i : layer.indices)
                if (i < uses.size()) ++uses[i];
            std::vector<u32> map(layer.palette.size());
            for (usize p = 0; p < layer.palette.size(); ++p) map[p] = uses[p] ? resolve(layer.palette[p], uses[p]) : air;
            bool any = false;
            for (usize i = 0; i < kSectionVolume; ++i) {
                values[i] = layer.indices[i] < map.size() ? map[layer.indices[i]] : air;
                any |= values[i] != air;
            }
            if (l == 0) sec.blocks().assign(values);
            else if (any) sec.ensure_secondary(air).assign(values);
        }
        if (sub->layers.size() > 2) out.warnings.push_back(where + ": block layers beyond the second were ignored");
    }

    // Biomes (Data3D): heightmap (512 bytes), then one storage per subchunk from the bottom.
    BRIDGER_TRY(const auto d3, db.get(chunk_key(pos.x, pos.z, Dimension::Overworld, static_cast<ChunkTag>(kData3DTag))));
    if (d3 && d3->size() >= 512) {
        usize p = 512;
        std::array<u32, kSectionVolume> prev{};
        prev.fill(static_cast<u32>(default_biome));
        for (u32 i = 0; i < col.sections_y() && p < d3->size(); ++i) {
            auto has = read_biome_storage(*d3, p, values);
            if (!has) {
                out.warnings.push_back(where + ": biome data could not be read (" + has.error().describe() + ")");
                break;
            }
            // "No data" (bits 127): Bridger keeps the biomes of the subchunk below.
            if (*has) prev = values;
            col.section(0, i, 0).ensure_biomes(prev[0]).assign(prev);
        }
    }

    // Block entities: concatenated LE NBT roots with absolute positions.
    BRIDGER_TRY(const auto bes, db.get(chunk_key(pos.x, pos.z, Dimension::Overworld, ChunkTag::BlockEntity)));
    if (bes) {
        const std::span<const u8> all(*bes);
        usize p = 0;
        while (p < all.size()) {
            auto doc = io::nbt::NbtDocument::parse(all.subspan(p), Endian::Little);
            if (!doc) {
                out.warnings.push_back(where + ": block entity data could not be read; the rest was skipped");
                break;
            }
            p += doc->consumed_bytes();
            const NbtCompoundView c = doc->root().entries();
            const auto x = c.find("x").as_int(), y = c.find("y").as_int(), z = c.find("z").as_int();
            const auto id = c.find("id").as_string();
            if (!x || !y || !z || !id) continue;
            BlockEntity be;
            be.pos = {*x - pos.x * 16, *y - out.min_y, *z - pos.z * 16};
            be.id = std::string(*id);
            if (!col.contains(be.pos)) {
                out.warnings.push_back(where + ": block entity " + be.id + " outside its chunk was skipped");
                continue;
            }
            io::nbt::NbtWriter w(Endian::Little);
            w.begin_root_compound();
            for (const io::nbt::NbtEntry& e : c) {
                if (e.name == "x" || e.name == "y" || e.name == "z" || e.name == "isMovable") continue;
                if (e.name == "pairx" && e.value.as_int()) w.write_int("pairx", *e.value.as_int() - pos.x * 16);
                else if (e.name == "pairz" && e.value.as_int()) w.write_int("pairz", *e.value.as_int() - pos.z * 16);
                else w.write_view(e.name, e.value);
            }
            w.end_compound();
            BRIDGER_TRY(std::vector<u8> bytes, std::move(w).finish());
            be.data = RawNbt{std::move(bytes), Endian::Little};
            col.block_entities.push_back(std::move(be));
        }
    }
    return out;
}

std::optional<std::array<u32, kSectionVolume>> BedrockWorldReader::read_subchunk_blocks(ChunkPos pos, i32 sy,
                                                                                       const StateLookupFn& lookup,
                                                                                       u32 air) const {
    const auto value = impl_->db->get(chunk_key(pos.x, pos.z, Dimension::Overworld, ChunkTag::SubChunkPrefix, static_cast<i8>(sy)));
    if (!value || !*value) return std::nullopt;
    auto sub = decode_subchunk_raw(**value);
    if (!sub || sub->layers.empty()) return std::nullopt;
    const RawSubchunkLayer& layer = sub->layers[0];
    std::vector<u32> map(layer.palette.size(), air);
    for (usize p = 0; p < layer.palette.size(); ++p)
        if (auto doc = io::nbt::NbtDocument::parse(layer.palette[p], Endian::Little))
            map[p] = lookup(doc->root().entries()).value_or(air);
    std::array<u32, kSectionVolume> out{};
    for (usize i = 0; i < kSectionVolume; ++i) out[i] = layer.indices[i] < map.size() ? map[layer.indices[i]] : air;
    return out;
}

} // namespace bridger::formats::bedrock

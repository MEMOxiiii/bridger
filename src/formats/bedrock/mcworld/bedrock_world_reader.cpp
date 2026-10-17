// Bedrock world reader. Formats and sources: docs/formats/bedrock-world.md ("Reading worlds").

#include <bridger/formats/bedrock/mcworld/bedrock_world_reader.hpp>
#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/bedrock/mcworld/leveldb_store.hpp>
#include <bridger/formats/bedrock/mcworld/subchunk_codec.hpp>
#include <bridger/io/endian.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/zip_archive.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>

#include <algorithm>
#include <map>
#include <fstream>
#include <unordered_map>

namespace bridger::formats::bedrock {
namespace fs = std::filesystem;
using io::nbt::NbtCompoundView;
using io::nbt::NbtView;

// Layers of one subchunk in any historical layout (empty: no data).
struct SubchunkLayers {
    std::vector<RawSubchunkLayer> layers;
    bool legacy = false; // numeric ids or 1.2.13 single storage: the legacy extra-data layer applies
};

namespace {

constexpr u8 kLegacyVersionTag = 0x76; // "v": chunk version before 1.16.100 [1]
constexpr u8 kEntityTag = 0x32;
constexpr u8 kData3DTag = 0x2B;
constexpr u8 kData2DTag = 0x2D;         // heightmap + 2D biomes, before 1.18 [1]
constexpr u8 kData2DLegacyTag = 0x2E;   // heightmap + 2D biomes with colours, before 1.0 [1]
constexpr u8 kLegacyTerrainTag = 0x30;  // whole 128-high column of numeric ids, before 1.0 [1]
constexpr u8 kLegacyExtraDataTag = 0x34; // second block layer of numeric ids, before 1.2.13 [PocketMine-MP]

// Chunk versions (the Version/LegacyVersion byte) from PocketMine-MP's ChunkVersion.php.
constexpr u8 kChunkV1_0_0 = 3;                // first version with SubChunkPrefix keys
constexpr u8 kChunkCavesCliffsOffsetFirst = 23; // 1.16.220.50 - 1.16.230.50 experimental worlds stored
constexpr u8 kChunkCavesCliffsOffsetLast = 26;  // subchunk y at key index y + 4

// LegacyBlockExtraData: i32 count, then (i32 key, u16 value) pairs; value = id | data << 8. The key
// packs x/z/y in 4/4/8 bits from 1.0 and 4/4/7 bits before (PocketMine-MP LevelDB.php,
// deserializeExtraDataKey). Returns per-subchunk layers (index = subchunk y from 0).
std::map<i32, RawSubchunkLayer> read_extra_data(std::span<const u8> d, u8 chunk_version) {
    std::map<i32, RawSubchunkLayer> out;
    std::map<i32, std::map<u16, u16>> slots; // subchunk -> value -> palette index
    if (d.size() < 4) return out;
    const i32 count = io::load_le<i32>(d.data());
    for (i32 n = 0; n < count && 4 + static_cast<usize>(n + 1) * 6 <= d.size(); ++n) {
        const u8* e = d.data() + 4 + static_cast<usize>(n) * 6;
        const u32 key = io::load_le<u32>(e);
        const u16 value = io::load_le<u16>(e + 4);
        u32 x, z, y;
        if (chunk_version >= kChunkV1_0_0) { x = (key >> 12) & 15; z = (key >> 8) & 15; y = key & 0xFF; }
        else { x = (key >> 11) & 15; z = (key >> 7) & 15; y = key & 0x7F; }
        const i32 sy = static_cast<i32>(y >> 4);
        auto [it, fresh] = out.try_emplace(sy);
        RawSubchunkLayer& layer = it->second;
        if (fresh) {
            layer.indices.fill(0);
            layer.palette.push_back(legacy_palette_entry(0, 0)); // air
        }
        auto& slot = slots[sy];
        auto [sit, added] = slot.try_emplace(value, static_cast<u16>(layer.palette.size()));
        if (added) layer.palette.push_back(legacy_palette_entry(static_cast<u8>(value & 0xFF), static_cast<u8>((value >> 8) & 0xF)));
        layer.indices[((y & 15) << 8) | (z << 4) | x] = sit->second;
    }
    return out;
}

// 2D biomes (index z*16 + x) -> every section of the column.
void fill_2d_biomes(Structure& col, const std::array<u32, 256>& ids) {
    std::array<u32, kSectionVolume> values{};
    for (usize i = 0; i < kSectionVolume; ++i) values[i] = ids[i & 255];
    for (u32 sy = 0; sy < col.sections_y(); ++sy) col.section(0, sy, 0).ensure_biomes(values[0]).assign(values);
}

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
    std::vector<ChunkPos> chunks;               // Overworld
    std::vector<ChunkPos> nether, end;
    bool other_dimensions = false;
    usize entities = 0;
};

BedrockWorldReader::BedrockWorldReader() : impl_(std::make_unique<Impl>()) {}
BedrockWorldReader::BedrockWorldReader(BedrockWorldReader&&) noexcept = default;
BedrockWorldReader& BedrockWorldReader::operator=(BedrockWorldReader&&) noexcept = default;
BedrockWorldReader::~BedrockWorldReader() = default;

const BedrockLevelInfo& BedrockWorldReader::level() const noexcept { return impl_->level; }
const std::vector<ChunkPos>& BedrockWorldReader::chunks(Dimension dimension) const noexcept {
    return dimension == Dimension::Nether ? impl_->nether : dimension == Dimension::End ? impl_->end : impl_->chunks;
}
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

    return open_unpacked(std::move(r), level_bytes);
}

bool is_bedrock_world_folder(const fs::path& path) {
    std::error_code ec;
    const fs::path root = fs::is_directory(path, ec) ? path : path.parent_path();
    return fs::is_regular_file(root / "level.dat", ec) && fs::is_regular_file(root / "db" / "CURRENT", ec);
}

Result<BedrockWorldReader> BedrockWorldReader::open_path(const fs::path& input, const fs::path& work_dir) {
    std::error_code ec;
    if (!is_bedrock_world_folder(input)) {
        BRIDGER_TRY(const platform::MappedFile f, platform::MappedFile::open(input));
        return open(f.data(), work_dir);
    }
    // A world folder (as in the game's minecraftWorlds): db/ is copied, because LevelDB writes
    // (LOCK, LOG, recovery) even when only reading, and the input must never change.
    const fs::path root = fs::is_directory(input, ec) ? input : input.parent_path();
    fs::create_directories(work_dir / "db", ec);
    for (const auto& e : fs::directory_iterator(root / "db", ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().filename() == "LOCK") continue;
        fs::copy_file(e.path(), work_dir / "db" / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) return make_error(ErrorCode::IoError, "cannot copy the world database: " + ec.message());
    }
    if (ec) return make_error(ErrorCode::IoError, "cannot list the world database: " + ec.message());
    BRIDGER_TRY(const platform::MappedFile level, platform::MappedFile::open(root / "level.dat"));
    BedrockWorldReader r;
    r.impl_->dir = work_dir;
    return open_unpacked(std::move(r), std::vector<u8>(level.data().begin(), level.data().end()));
}

Result<BedrockWorldReader> BedrockWorldReader::open_unpacked(BedrockWorldReader r, const std::vector<u8>& level_bytes) {
    const fs::path work_dir = r.impl_->dir;
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
    for (const NbtView v : c.find("lastOpenedWithVersion").items())
        if (const auto n = v.as_int()) r.impl_->level.last_opened_version.push_back(*n);
    r.impl_->level.storage_version = c.find("StorageVersion").as_int().value_or(io::load_le<i32>(level_bytes.data()));

    if (!fs::exists(work_dir / "db" / "CURRENT")) return make_error(ErrorCode::Malformed, "mcworld: no LevelDB database (db/)");
    BRIDGER_TRY(LevelDbStore db, LevelDbStore::open(work_dir / "db", /*create_new=*/false));
    Impl& m = *r.impl_;
    BRIDGER_TRY_STATUS(db.for_each([&](std::string_view key, std::span<const u8>) {
        if (key.size() == 9 && (static_cast<u8>(key[8]) == static_cast<u8>(ChunkTag::Version) ||
                                static_cast<u8>(key[8]) == kLegacyVersionTag)) {
            m.chunks.push_back({io::load_le<i32>(key.data()), io::load_le<i32>(key.data() + 4)});
        } else if (key.size() == 13 && (static_cast<u8>(key[12]) == static_cast<u8>(ChunkTag::Version) ||
                                        static_cast<u8>(key[12]) == kLegacyVersionTag)) {
            const ChunkPos c{io::load_le<i32>(key.data()), io::load_le<i32>(key.data() + 4)};
            const i32 dim = io::load_le<i32>(key.data() + 8);
            if (dim == static_cast<i32>(Dimension::Nether)) m.nether.push_back(c);
            else if (dim == static_cast<i32>(Dimension::End)) m.end.push_back(c);
            m.other_dimensions = true;
        } else if (key.starts_with("actorprefix")) {
            ++m.entities;
        } else if (key.size() == 9 && static_cast<u8>(key[8]) == kEntityTag) {
            ++m.entities; // legacy storage: at least one entity
        }
    }));
    for (std::vector<ChunkPos>* list : {&m.chunks, &m.nether, &m.end}) {
        std::sort(list->begin(), list->end(), [](ChunkPos a, ChunkPos b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
    BRIDGER_TRY(m.level.local_player, db.get("~local_player"));
    m.db.emplace(std::move(db));
    return r;
}

Result<u8> BedrockWorldReader::chunk_version(ChunkPos pos, Dimension dim) const {
    BRIDGER_TRY(const auto v, impl_->db->get(chunk_key(pos.x, pos.z, dim, ChunkTag::Version)));
    if (v && !v->empty()) return (*v)[0];
    BRIDGER_TRY(const auto legacy, impl_->db->get(chunk_key(pos.x, pos.z, dim, static_cast<ChunkTag>(kLegacyVersionTag))));
    if (legacy && !legacy->empty()) return (*legacy)[0];
    return u8{40}; // no version key: read as a current chunk
}

Result<SubchunkLayers> BedrockWorldReader::subchunk_layers(ChunkPos pos, i32 sy, u8 version, const LegacyTerrain* terrain,
                                                           Dimension dim) const {
    SubchunkLayers out;
    if (terrain != nullptr) { // pre-1.0: the column holds subchunks 0-7
        if (sy < 0 || sy >= 8) return out;
        out.layers.push_back(terrain->subchunks[static_cast<usize>(sy)]);
        out.legacy = true;
    } else {
        // The Caves & Cliffs experiment shifted Overworld subchunk keys only.
        const i32 offset = dim == Dimension::Overworld && version >= kChunkCavesCliffsOffsetFirst &&
                                   version <= kChunkCavesCliffsOffsetLast
                               ? 4
                               : 0;
        BRIDGER_TRY(const auto value, impl_->db->get(chunk_key(pos.x, pos.z, dim, ChunkTag::SubChunkPrefix,
                                                               static_cast<i8>(sy + offset))));
        if (!value) return out;
        BRIDGER_TRY(RawSubchunk sub, decode_subchunk_raw(*value));
        out.legacy = sub.legacy || sub.version == 1;
        out.layers = std::move(sub.layers);
    }
    if (out.legacy && out.layers.size() == 1 && sy >= 0) {
        BRIDGER_TRY(const auto extra, impl_->db->get(chunk_key(pos.x, pos.z, dim, static_cast<ChunkTag>(kLegacyExtraDataTag))));
        if (extra) {
            auto layers = read_extra_data(*extra, version);
            if (const auto it = layers.find(sy); it != layers.end()) out.layers.push_back(std::move(it->second));
        }
    }
    return out;
}

Result<BedrockColumn> BedrockWorldReader::read_column(ChunkPos pos, std::shared_ptr<BlockStateRegistry> blocks,
                                                      std::shared_ptr<BiomeRegistry> biomes, u32 bedrock_data_version,
                                                      const StateLookupFn& lookup, u32 air, i32 default_biome,
                                                      Dimension dim) const {
    const LevelDbStore& db = *impl_->db;
    const DimensionRange range = dimension_range(dim);
    BedrockColumn out;
    out.pos = pos;
    out.min_y = range.min_y();
    BRIDGER_TRY(out.column, Structure::create(std::move(blocks), std::move(biomes),
                                              Extent{16, static_cast<u32>(range.subchunks * 16), 16},
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
            const auto val = e.find("val").as_short();
            if (const auto id = e.find(kLegacyIdEntryKey).as_int()) name = "legacy id " + std::to_string(*id) + ":" + std::to_string(val.value_or(0));
            else name = std::string(e.find("name").as_string().value_or("?")) + (val ? ":" + std::to_string(*val) : "");
            if (const auto idx = lookup(e)) v = *idx;
            else out.unknown_blocks[name] += uses;
        } else {
            out.unknown_blocks["(unreadable palette entry)"] += uses;
        }
        resolved.emplace(k, v);
        return v;
    };

    std::array<u32, kSectionVolume> values{};
    BRIDGER_TRY(const u8 version, chunk_version(pos, dim));
    // LegacyTerrain exists only in chunks older than 1.0 (no lookup for newer ones).
    std::optional<std::vector<u8>> legacy_terrain;
    if (version < kChunkV1_0_0) {
        BRIDGER_TRY(legacy_terrain, db.get(chunk_key(pos.x, pos.z, dim, static_cast<ChunkTag>(kLegacyTerrainTag))));
    }
    std::optional<LegacyTerrain> terrain;
    if (legacy_terrain) {
        auto t = decode_legacy_terrain(*legacy_terrain);
        if (t) terrain = std::move(*t);
        else out.warnings.push_back(where + ": the pre-1.0 terrain could not be read (" + t.error().describe() + "); it was left empty");
    }
    for (i32 sy = range.min_subchunk; sy < range.min_subchunk + range.subchunks; ++sy) {
        auto sub = subchunk_layers(pos, sy, version, terrain ? &*terrain : nullptr, dim);
        if (!sub) {
            out.warnings.push_back(where + " subchunk " + std::to_string(sy) + " could not be read (" +
                                   sub.error().describe() + "); it was left empty");
            continue;
        }
        if (sub->layers.empty()) continue;
        Section& sec = col.section(0, static_cast<u32>(sy - range.min_subchunk), 0);
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
    BRIDGER_TRY(const auto d3, db.get(chunk_key(pos.x, pos.z, dim, static_cast<ChunkTag>(kData3DTag))));
    // 2D biomes (before 1.18) are looked up only when there is no Data3D.
    std::optional<std::vector<u8>> d2, d2l;
    if (!d3) {
        BRIDGER_TRY(d2, db.get(chunk_key(pos.x, pos.z, dim, static_cast<ChunkTag>(kData2DTag))));
    }
    if (!d3 && !d2) {
        BRIDGER_TRY(d2l, db.get(chunk_key(pos.x, pos.z, dim, static_cast<ChunkTag>(kData2DLegacyTag))));
    }
    std::array<u32, 256> ids2d{};
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
    } else if (d2 && d2->size() >= 512 + 256) {
        // Data2D: heightmap (512 bytes), then 256 biome ids, one byte each (two bytes, little-endian,
        // when the value is 1024 bytes long: 1.21.40.23+ "Old" worlds) [1].
        const bool wide = d2->size() >= 512 + 512;
        for (usize i = 0; i < 256; ++i) ids2d[i] = wide ? io::load_le<u16>(d2->data() + 512 + i * 2) : (*d2)[512 + i];
        fill_2d_biomes(col, ids2d);
    } else if (d2l && d2l->size() >= 512 + 1024) {
        // Data2DLegacy: heightmap (512 bytes), then 256 entries of 4 bytes, the biome id first [1].
        for (usize i = 0; i < 256; ++i) ids2d[i] = (*d2l)[512 + i * 4];
        fill_2d_biomes(col, ids2d);
    } else if (terrain) {
        for (usize i = 0; i < 256; ++i) ids2d[i] = terrain->biomes[i];
        fill_2d_biomes(col, ids2d);
    }

    // Block entities: concatenated LE NBT roots with absolute positions.
    BRIDGER_TRY(const auto bes, db.get(chunk_key(pos.x, pos.z, dim, ChunkTag::BlockEntity)));
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

    // Actors: since 1.18.30 "digp" + x, z (+ dimension) lists 8-byte keys of "actorprefix" entries; before, the chunk's
    // Entity key (0x32) holds concatenated NBT roots (minecraft.wiki "Bedrock Edition level format"; Chunker
    // v1_18_30 ColumnReader). Each becomes a column-local Entity whose data is the actor's NBT as stored.
    const auto add_actor = [&](std::span<const u8> nbt, usize* consumed) {
        auto doc = io::nbt::NbtDocument::parse(nbt, Endian::Little);
        if (!doc) {
            out.warnings.push_back(where + ": an entity could not be read and was skipped");
            return false;
        }
        if (consumed) *consumed = doc->consumed_bytes();
        const NbtCompoundView c = doc->root().entries();
        const auto pos_list = c.find("Pos").items();
        const auto id = c.find("identifier").as_string();
        if (!id || pos_list.size() != 3) return true;
        Entity e;
        e.id = std::string(*id);
        const auto coord = [&](usize i) { return static_cast<f64>(pos_list.at(i).as_float().value_or(0.0f)); };
        e.pos = {coord(0) - pos.x * 16.0, coord(1) - out.min_y, coord(2) - pos.z * 16.0};
        e.data = RawNbt{std::vector<u8>(nbt.begin(), nbt.begin() + static_cast<std::ptrdiff_t>(doc->consumed_bytes())), Endian::Little};
        col.entities.push_back(std::move(e));
        return true;
    };
    std::string digp = "digp";
    std::array<u8, 4> le{};
    for (const i32 v : {pos.x, pos.z}) {
        io::store_le<i32>(le.data(), v);
        digp.append(reinterpret_cast<const char*>(le.data()), 4);
    }
    if (dim != Dimension::Overworld) {
        io::store_le<i32>(le.data(), static_cast<i32>(dim));
        digp.append(reinterpret_cast<const char*>(le.data()), 4);
    }
    BRIDGER_TRY(const auto keys, db.get(digp));
    if (keys) {
        for (usize k = 0; k + 8 <= keys->size(); k += 8) {
            std::string actor_key = "actorprefix";
            actor_key.append(reinterpret_cast<const char*>(keys->data() + k), 8);
            BRIDGER_TRY(const auto actor, db.get(actor_key));
            if (actor) (void)add_actor(*actor, nullptr);
        }
    }
    BRIDGER_TRY(const auto legacy_entities, db.get(chunk_key(pos.x, pos.z, dim, static_cast<ChunkTag>(kEntityTag))));
    if (legacy_entities) {
        const std::span<const u8> all(*legacy_entities);
        usize p = 0;
        while (p < all.size()) {
            usize used = 0;
            if (!add_actor(all.subspan(p), &used) || used == 0) break;
            p += used;
        }
    }
    return out;
}

std::optional<std::array<u32, kSectionVolume>> BedrockWorldReader::read_subchunk_blocks(ChunkPos pos, i32 sy,
                                                                                       const StateLookupFn& lookup,
                                                                                       u32 air, Dimension dim) const {
    const auto version = chunk_version(pos, dim);
    if (!version) return std::nullopt;
    std::optional<LegacyTerrain> terrain;
    if (sy >= 0 && sy < 8 && *version < kChunkV1_0_0) {
        if (const auto t = impl_->db->get(chunk_key(pos.x, pos.z, dim, static_cast<ChunkTag>(kLegacyTerrainTag))); t && *t)
            if (auto d = decode_legacy_terrain(**t)) terrain = std::move(*d);
    }
    auto sub = subchunk_layers(pos, sy, *version, terrain ? &*terrain : nullptr, dim);
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

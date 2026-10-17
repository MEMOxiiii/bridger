#include <bridger/formats/bedrock/mcworld/bedrock_world.hpp>
#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/bedrock/mcworld/leveldb_store.hpp>
#include <bridger/io/binary_writer.hpp>
#include <bridger/io/endian.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/zip_archive.hpp>
#include <bridger/platform/paths.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>

namespace bridger::formats::bedrock {
namespace fs = std::filesystem;
namespace {

constexpr usize bedrock_order(usize x, usize y, usize z) noexcept { return (x << 8) | (z << 4) | y; }

// One biome storage: like a block storage but with a runtime palette of i32 ids
// (header flag 1), as written by Chunker for Data3D (docs/formats/bedrock-world.md).
void write_biome_storage(io::BinaryWriter& w, const std::array<u32, kSectionVolume>& bedrock_ordered) {
    std::vector<u32> palette;
    std::array<u16, kSectionVolume> idx; // NOLINT(cppcoreguidelines-pro-type-member-init)
    for (usize i = 0; i < kSectionVolume; ++i) {
        auto it = std::find(palette.begin(), palette.end(), bedrock_ordered[i]);
        if (it == palette.end()) {
            palette.push_back(bedrock_ordered[i]);
            it = palette.end() - 1;
        }
        idx[i] = static_cast<u16>(it - palette.begin());
    }
    u32 bits = 1;
    for (const u32 b : {1u, 2u, 3u, 4u, 5u, 6u, 8u, 16u})
        if (palette.size() <= (usize{1} << b)) {
            bits = b;
            break;
        }
    const u32 per_word = 32 / bits;
    const usize words = (kSectionVolume + per_word - 1) / per_word;
    std::vector<u32> packed(words, 0);
    for (usize i = 0; i < kSectionVolume; ++i) packed[i / per_word] |= static_cast<u32>(idx[i]) << ((i % per_word) * bits);
    w.write<u8>(static_cast<u8>((bits << 1) | 1));
    for (const u32 word : packed) w.write<u32>(word);
    w.write<i32>(static_cast<i32>(palette.size()));
    for (const u32 id : palette) w.write<i32>(static_cast<i32>(id));
}

// Translated block entity (no positions) -> stored form with absolute positions.
Status append_block_entity(io::BinaryWriter& out, const BlockEntity& be, BlockPos world, BlockPos origin) {
    auto doc = io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian);
    if (!doc) return tl::unexpected(std::move(doc).error());
    io::nbt::NbtWriter w(Endian::Little);
    w.begin_root_compound("");
    for (const io::nbt::NbtEntry& e : doc->root().entries()) {
        if (e.name == "x" || e.name == "y" || e.name == "z" || e.name == "isMovable") continue; // regenerated
        if (e.name == "pairx" && e.value.as_int()) w.write_int("pairx", *e.value.as_int() + origin.x);
        else if (e.name == "pairz" && e.value.as_int()) w.write_int("pairz", *e.value.as_int() + origin.z);
        else w.write_view(e.name, e.value);
    }
    w.write_int("x", world.x);
    w.write_int("y", world.y);
    w.write_int("z", world.z);
    w.write_byte("isMovable", 1);
    w.end_compound();
    BRIDGER_TRY(const std::vector<u8> bytes, std::move(w).finish());
    out.write_bytes(bytes);
    return {};
}

Result<std::vector<u8>> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return make_error(ErrorCode::IoError, "cannot read " + p.string());
    return std::vector<u8>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Status write_file(const fs::path& p, std::span<const u8> bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) return make_error(ErrorCode::IoError, "cannot write " + p.string());
    return {};
}


using BiomeStorage = std::array<u32, kSectionVolume>;

// Per-column accumulation shared by write_new_world and WorldBuilder.
struct ColumnAccumulator {
    std::array<i32, 256> highest; // world y of the highest non-air block per (z<<4|x), or INT_MIN
    Dimension dim = Dimension::Overworld;
    DimensionRange range = dimension_range(Dimension::Overworld);
    std::vector<BiomeStorage> biomes; // one per subchunk of the range
    u8 chunk_version = kChunkVersion; // the target version's chunk Version byte

    explicit ColumnAccumulator(i32 default_biome, u8 version = kChunkVersion, Dimension d = Dimension::Overworld)
        : dim(d), range(dimension_range(d)), biomes(static_cast<usize>(range.subchunks)), chunk_version(version) {
        highest.fill(std::numeric_limits<i32>::min());
        for (auto& b : biomes) b.fill(static_cast<u32>(default_biome));
    }
    BiomeStorage& biome_storage(i32 sy) { return biomes[static_cast<usize>(sy - range.min_subchunk)]; }

    // Blocks/liquids in Bridger order (y*256+z*16+x). Skips empty subchunks.
    Status add_subchunk(EncodedColumn& out, i32 cx, i32 cz, i32 sy, const std::array<u32, kSectionVolume>& blocks,
                        const std::array<u32, kSectionVolume>* liquids, u32 air, const StateNbtFn& state_nbt) {
        const i32 y0 = sy * 16;
        bool any_block = false;
        for (i32 y = 15; y >= 0; --y)
            for (usize zx = 0; zx < 256; ++zx)
                if (blocks[(static_cast<usize>(y) << 8) | zx] != air) {
                    any_block = true;
                    if (highest[zx] < y0 + y) highest[zx] = y0 + y;
                }
        if (!any_block && liquids == nullptr) return {};
        PaletteLayer layer0;
        layer0.assign(blocks);
        PaletteLayer layer1;
        std::vector<const PaletteLayer*> layers{&layer0};
        if (liquids != nullptr) {
            layer1.assign(*liquids);
            layers.push_back(&layer1);
        }
        BRIDGER_TRY(const std::vector<u8> encoded, encode_subchunk(static_cast<i8>(sy), layers, state_nbt));
        out.add(chunk_key(cx, cz, dim, ChunkTag::SubChunkPrefix, static_cast<i8>(sy)), encoded);
        ++out.stats.subchunks;
        return {};
    }

    // Data3D, block entities (with their world positions), Version, FinalizedState.
    Status finish(EncodedColumn& out, i32 cx, i32 cz, std::span<const std::pair<const BlockEntity*, BlockPos>> entities,
                  BlockPos origin) {
        // Data3D: 256 i16 heights (z-major) relative to the dimension's bottom, then one biome storage per subchunk.
        io::BinaryWriter d3(Endian::Little);
        for (usize z = 0; z < 16; ++z)
            for (usize x = 0; x < 16; ++x) {
                const i32 h = highest[(z << 4) | x];
                d3.write<i16>(static_cast<i16>(h == std::numeric_limits<i32>::min() ? 0 : h - range.min_y() + 1));
            }
        for (const auto& storage : biomes) write_biome_storage(d3, storage);
        out.add(chunk_key(cx, cz, dim, ChunkTag::Data3D), d3.view());

        if (!entities.empty()) {
            io::BinaryWriter bew(Endian::Little);
            for (const auto& [be, world] : entities) {
                BRIDGER_TRY_STATUS(append_block_entity(bew, *be, world, origin));
                ++out.stats.block_entities;
            }
            out.add(chunk_key(cx, cz, dim, ChunkTag::BlockEntity), bew.view());
        }

        const u8 version = chunk_version;
        out.add(chunk_key(cx, cz, dim, ChunkTag::Version), std::span<const u8>(&version, 1));
        std::array<u8, 4> finalized{};
        io::store_le<i32>(finalized.data(), kFinalizedDone);
        out.add(chunk_key(cx, cz, dim, ChunkTag::FinalizedState), finalized);
        ++out.stats.chunks;
        return {};
    }
};

Status put_column(LevelDbStore& db, const EncodedColumn& column, WorldWriteStats& st) {
    BRIDGER_TRY_STATUS(db.put_batch(column.entries));
    st.chunks += column.stats.chunks;
    st.subchunks += column.stats.subchunks;
    st.block_entities += column.stats.block_entities;
    return {};
}

Status write_level_files(const fs::path& dir, const LevelDatOptions& level) {
    BRIDGER_TRY_STATUS(write_file(dir / "level.dat", encode_level_dat(level)));
    const std::string& name = level.level_name;
    return write_file(dir / "levelname.txt", std::span<const u8>(reinterpret_cast<const u8*>(name.data()), name.size()));
}

} // namespace

Result<std::vector<u8>> write_new_world(const Structure& s, const NewWorldOptions& o, const StateNbtFn& state_nbt,
                                        const fs::path& work_dir, WorldWriteStats* stats) {
    if (s.source().edition != Edition::Bedrock)
        return make_error(ErrorCode::InvalidArgument, "write_new_world needs a Bedrock structure");
    const Extent size = s.size();
    const BlockPos origin = o.origin;
    if (size.volume() > 0) {
        const i64 top = i64{origin.y} + size.y - 1;
        if (origin.y < kOverworldMinY || top > kOverworldMaxY)
            return make_error(ErrorCode::OutOfBounds, "structure spans y " + std::to_string(origin.y) + ".." +
                                                          std::to_string(top) + ", outside the build height " +
                                                          std::to_string(kOverworldMinY) + ".." +
                                                          std::to_string(kOverworldMaxY));
    }

    // Flat layer state per world y.
    std::vector<u32> flat;
    for (const FlatLayer& l : o.flat_layers) flat.insert(flat.end(), l.count, l.state);
    auto flat_at = [&](i32 y) -> u32 {
        const i32 i = y - kOverworldMinY;
        return (i >= 0 && static_cast<usize>(i) < flat.size()) ? flat[static_cast<usize>(i)] : o.air_state;
    };
    const i32 flat_top = kOverworldMinY + static_cast<i32>(flat.size()) - 1;

    fs::create_directories(work_dir / "db");
    WorldWriteStats st;
    {
        BRIDGER_TRY(LevelDbStore db, LevelDbStore::open(work_dir / "db", /*create_new=*/true));

        // Block entities by chunk column (absolute positions).
        std::map<std::pair<i32, i32>, std::vector<std::pair<const BlockEntity*, BlockPos>>> entities;
        for (const BlockEntity& be : s.block_entities) {
            const BlockPos w{be.pos.x + origin.x, be.pos.y + origin.y, be.pos.z + origin.z};
            entities[{floor_div16(w.x), floor_div16(w.z)}].push_back({&be, w});
        }

        if (size.volume() > 0) {
            const i32 cx0 = floor_div16(origin.x), cx1 = floor_div16(origin.x + static_cast<i32>(size.x) - 1);
            const i32 cz0 = floor_div16(origin.z), cz1 = floor_div16(origin.z + static_cast<i32>(size.z) - 1);
            std::array<u32, kSectionVolume> blocks;  // Bridger order
            std::array<u32, kSectionVolume> liquids; // Bridger order
            for (i32 cz = cz0; cz <= cz1; ++cz) {
                for (i32 cx = cx0; cx <= cx1; ++cx) {
                    ColumnAccumulator column(o.default_biome, o.chunk_version);
                    EncodedColumn encoded;
                    for (i32 sy = kOverworldMinSubchunk; sy < kOverworldMinSubchunk + kOverworldSubchunks; ++sy) {
                        const i32 y0 = sy * 16;
                        auto& biomes = column.biome_storage(sy);
                        const bool has_flat = y0 <= flat_top;
                        const i32 ly0 = std::max(y0, origin.y), ly1 = std::min(y0 + 15, origin.y + static_cast<i32>(size.y) - 1);
                        const bool has_structure = ly0 <= ly1;
                        if (!has_flat && !has_structure) continue;

                        blocks.fill(o.air_state);
                        liquids.fill(o.air_state);
                        bool any_liquid = false;
                        for (i32 y = 0; y < 16; ++y) {
                            const u32 f = flat_at(y0 + y);
                            if (f != o.air_state)
                                for (usize i = 0; i < 256; ++i) blocks[(static_cast<usize>(y) << 8) | i] = f;
                        }
                        if (has_structure) {
                            const i32 wx0 = std::max(cx * 16, origin.x), wx1 = std::min(cx * 16 + 15, origin.x + static_cast<i32>(size.x) - 1);
                            const i32 wz0 = std::max(cz * 16, origin.z), wz1 = std::min(cz * 16 + 15, origin.z + static_cast<i32>(size.z) - 1);
                            for (i32 wy = ly0; wy <= ly1; ++wy)
                                for (i32 wz = wz0; wz <= wz1; ++wz)
                                    for (i32 wx = wx0; wx <= wx1; ++wx) {
                                        const BlockPos local{wx - origin.x, wy - origin.y, wz - origin.z};
                                        const usize i = section_index(wx & 15, wy & 15, wz & 15);
                                        const u32 v = s.block(local);
                                        if (v != o.air_state || o.air_replaces) blocks[i] = v;
                                        if (const u32 l = s.has_secondary(local) ? s.secondary(local) : o.air_state;
                                            l != o.air_state) {
                                            liquids[i] = l;
                                            any_liquid = true;
                                        }
                                        if (const auto b = s.biome(local))
                                            biomes[bedrock_order(static_cast<usize>(wx & 15), static_cast<usize>(wy & 15),
                                                                 static_cast<usize>(wz & 15))] = *b;
                                    }
                        }
                        BRIDGER_TRY_STATUS(column.add_subchunk(encoded, cx, cz, sy, blocks, any_liquid ? &liquids : nullptr,
                                                               o.air_state, state_nbt));
                    }
                    const auto it = entities.find({cx, cz});
                    std::span<const std::pair<const BlockEntity*, BlockPos>> bes;
                    if (it != entities.end()) bes = it->second;
                    BRIDGER_TRY_STATUS(column.finish(encoded, cx, cz, bes, origin));
                    BRIDGER_TRY_STATUS(put_column(db, encoded, st));
                }
            }
        }
        BRIDGER_TRY_STATUS(db.compact());
    } // database closed: the directory is complete

    BRIDGER_TRY_STATUS(write_level_files(work_dir, o.level));

    std::vector<io::ZipEntry> entries;
    for (const auto& e : fs::recursive_directory_iterator(work_dir)) {
        if (!e.is_regular_file()) continue;
        BRIDGER_TRY(std::string rel, platform::path_to_utf8(fs::relative(e.path(), work_dir)));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        BRIDGER_TRY(std::vector<u8> data, read_file(e.path()));
        entries.push_back({std::move(rel), std::move(data)});
    }
    std::sort(entries.begin(), entries.end(), [](const io::ZipEntry& a, const io::ZipEntry& b) { return a.name < b.name; });
    if (stats != nullptr) *stats = st;
    return io::write_zip(entries);
}

// --- WorldBuilder -------------------------------------------------------------------

struct WorldBuilder::Impl {
    fs::path dir;
    std::optional<LevelDbStore> db;
    WorldWriteStats stats;
};

WorldBuilder::WorldBuilder() = default;
WorldBuilder::WorldBuilder(WorldBuilder&&) noexcept = default;
WorldBuilder& WorldBuilder::operator=(WorldBuilder&&) noexcept = default;
WorldBuilder::~WorldBuilder() = default;

Result<WorldBuilder> WorldBuilder::create(const fs::path& work_dir) {
    fs::create_directories(work_dir / "db");
    BRIDGER_TRY(LevelDbStore db, LevelDbStore::open(work_dir / "db", /*create_new=*/true, /*bulk_load=*/true));
    WorldBuilder b;
    b.impl_ = std::make_unique<Impl>();
    b.impl_->dir = work_dir;
    b.impl_->db.emplace(std::move(db));
    return b;
}

Result<EncodedColumn> WorldBuilder::encode_column(ChunkPos pos, i32 min_y, const Structure& column, u32 air_state,
                                                  i32 default_biome, const StateNbtFn& state_nbt, u8 chunk_version,
                                                  Dimension dimension) {
    if (column.source().edition != Edition::Bedrock)
        return make_error(ErrorCode::InvalidArgument, "encode_column needs a Bedrock column");
    if (column.size().x != 16 || column.size().z != 16 || (min_y & 15) != 0)
        return make_error(ErrorCode::InvalidArgument, "encode_column needs a 16-wide column starting on a section boundary");
    EncodedColumn out;
    ColumnAccumulator acc(default_biome, chunk_version, dimension);
    const DimensionRange range = dimension_range(dimension);
    auto blocks = std::make_unique<std::array<u32, kSectionVolume>>();
    auto liquids = std::make_unique<std::array<u32, kSectionVolume>>();
    auto biome_tmp = std::make_unique<std::array<u32, kSectionVolume>>();
    const i32 first_sy = min_y >> 4;
    const i32 sections = static_cast<i32>(column.sections_y());
    for (i32 i = 0; i < sections; ++i) {
        const i32 sy = first_sy + i;
        const Section& sec = column.section(0, static_cast<u32>(i), 0);
        sec.blocks().copy_to(*blocks);
        if (!range.contains_subchunk(sy)) {
            out.clipped_blocks +=
                static_cast<usize>(std::count_if(blocks->begin(), blocks->end(), [&](u32 v) { return v != air_state; }));
            continue;
        }
        const std::array<u32, kSectionVolume>* layer2 = nullptr;
        if (const PaletteLayer* l = sec.secondary_layer()) {
            l->copy_to(*liquids);
            if (std::any_of(liquids->begin(), liquids->end(), [&](u32 v) { return v != air_state; })) layer2 = liquids.get();
        }
        if (const PaletteLayer* b = sec.biome_layer()) {
            b->copy_to(*biome_tmp);
            auto& storage = acc.biome_storage(sy);
            for (usize j = 0; j < kSectionVolume; ++j) storage[bedrock_order(j & 15, j >> 8, (j >> 4) & 15)] = (*biome_tmp)[j];
        }
        BRIDGER_TRY_STATUS(acc.add_subchunk(out, pos.x, pos.z, sy, *blocks, layer2, air_state, state_nbt));
    }
    const BlockPos origin{pos.x * 16, min_y, pos.z * 16};
    std::vector<std::pair<const BlockEntity*, BlockPos>> bes;
    for (const BlockEntity& be : column.block_entities) {
        const BlockPos w{be.pos.x + origin.x, be.pos.y + origin.y, be.pos.z + origin.z};
        if (w.y < range.min_y() || w.y > range.max_y()) continue;
        bes.push_back({&be, w});
    }
    BRIDGER_TRY_STATUS(acc.finish(out, pos.x, pos.z, bes, origin));
    return out;
}

Status WorldBuilder::put(const EncodedColumn& column) {
    Impl& m = *impl_;
    if (!m.db) return make_error(ErrorCode::InvalidArgument, "WorldBuilder::put after finish");
    return put_column(*m.db, column, m.stats);
}

Status WorldBuilder::write_column(ChunkPos pos, i32 min_y, const Structure& column, u32 air_state, i32 default_biome,
                                  const StateNbtFn& state_nbt, usize* clipped_blocks) {
    BRIDGER_TRY(const EncodedColumn encoded, encode_column(pos, min_y, column, air_state, default_biome, state_nbt));
    if (clipped_blocks != nullptr) *clipped_blocks = encoded.clipped_blocks;
    return put(encoded);
}

Status WorldBuilder::finish(const LevelDatOptions& level) {
    Impl& m = *impl_;
    if (!m.db) return make_error(ErrorCode::InvalidArgument, "WorldBuilder::finish called twice");
    BRIDGER_TRY_STATUS(m.db->flush());
    m.db.reset(); // closed: the directory is complete
    return write_level_files(m.dir, level);
}

const WorldWriteStats& WorldBuilder::stats() const noexcept { return impl_->stats; }

Status validate_mcworld(std::span<const u8> mcworld, const fs::path& work_dir, WorldWriteStats* stats) {
    BRIDGER_TRY(const std::vector<io::ZipEntry> entries, io::read_zip(mcworld));
    bool has_level = false, has_db = false;
    for (const io::ZipEntry& e : entries) {
        BRIDGER_TRY(const fs::path rel, platform::path_from_utf8(e.name));
        const fs::path p = work_dir / rel;
        fs::create_directories(p.parent_path());
        BRIDGER_TRY_STATUS(write_file(p, e.data));
        if (e.name == "level.dat") {
            has_level = true;
            if (e.data.size() < 8) return make_error(ErrorCode::ValidationFailed, "level.dat too short");
            const i32 len = io::load_le<i32>(e.data.data() + 4);
            if (len < 0 || static_cast<usize>(len) != e.data.size() - 8)
                return make_error(ErrorCode::ValidationFailed, "level.dat length header mismatch");
            BRIDGER_TRY_STATUS(io::nbt::NbtDocument::parse(std::span<const u8>(e.data).subspan(8), Endian::Little));
        }
        if (e.name == "db/CURRENT") has_db = true;
    }
    if (!has_level || !has_db) return make_error(ErrorCode::ValidationFailed, "world is missing level.dat or db/");
    return validate_world_directory(work_dir, stats);
}

Status validate_world_directory(const fs::path& work_dir, WorldWriteStats* stats) {
    {
        BRIDGER_TRY(const std::vector<u8> level, read_file(work_dir / "level.dat"));
        if (level.size() < 8) return make_error(ErrorCode::ValidationFailed, "level.dat too short");
        const i32 len = io::load_le<i32>(level.data() + 4);
        if (len < 0 || static_cast<usize>(len) != level.size() - 8)
            return make_error(ErrorCode::ValidationFailed, "level.dat length header mismatch");
        BRIDGER_TRY_STATUS(io::nbt::NbtDocument::parse(std::span<const u8>(level).subspan(8), Endian::Little));
    }
    BRIDGER_TRY(LevelDbStore db, LevelDbStore::open(work_dir / "db", /*create_new=*/false));
    WorldWriteStats st;
    Status failure;
    BRIDGER_TRY_STATUS(db.for_each([&](std::string_view key, std::span<const u8> value) {
        if (!failure) return;
        // Chunk keys: x, z (8 bytes), a dimension (4 bytes) outside the Overworld, the tag, and a subchunk index after
        // SubChunkPrefix: 9/10 bytes in the Overworld, 13/14 elsewhere [1].
        const bool dim = key.size() == 13 || key.size() == 14;
        const usize tag_at = dim ? 12 : 8;
        if ((key.size() == 10 || key.size() == 14) && static_cast<u8>(key[tag_at]) == static_cast<u8>(ChunkTag::SubChunkPrefix)) {
            if (auto d = decode_subchunk(value); !d) failure = tl::unexpected(std::move(d).error());
            else ++st.subchunks;
        } else if ((key.size() == 9 || key.size() == 13) && static_cast<u8>(key[tag_at]) == static_cast<u8>(ChunkTag::BlockEntity)) {
            usize pos = 0;
            while (pos < value.size()) {
                auto doc = io::nbt::NbtDocument::parse(value.subspan(pos), Endian::Little);
                if (!doc) {
                    failure = tl::unexpected(std::move(doc).error());
                    return;
                }
                pos += doc->consumed_bytes();
                ++st.block_entities;
            }
        } else if ((key.size() == 9 || key.size() == 13) && static_cast<u8>(key[tag_at]) == static_cast<u8>(ChunkTag::Version)) {
            ++st.chunks;
        }
    }));
    if (!failure) return make_error(ErrorCode::ValidationFailed, "invalid world data: " + failure.error().describe());
    if (stats != nullptr) *stats = st;
    return {};
}

} // namespace bridger::formats::bedrock

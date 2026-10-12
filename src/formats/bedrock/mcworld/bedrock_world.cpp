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
        std::map<std::pair<i32, i32>, std::vector<const BlockEntity*>> entities;
        for (const BlockEntity& be : s.block_entities) {
            const BlockPos w{be.pos.x + origin.x, be.pos.y + origin.y, be.pos.z + origin.z};
            entities[{floor_div16(w.x), floor_div16(w.z)}].push_back(&be);
        }

        if (size.volume() > 0) {
            const i32 cx0 = floor_div16(origin.x), cx1 = floor_div16(origin.x + static_cast<i32>(size.x) - 1);
            const i32 cz0 = floor_div16(origin.z), cz1 = floor_div16(origin.z + static_cast<i32>(size.z) - 1);
            for (i32 cz = cz0; cz <= cz1; ++cz) {
                for (i32 cx = cx0; cx <= cx1; ++cx) {
                    std::array<i32, 256> highest; // world y of the highest non-air block per (x,z), or INT_MIN
                    highest.fill(std::numeric_limits<i32>::min());
                    std::array<std::array<u32, kSectionVolume>, kOverworldSubchunks> biome_storages;

                    for (i32 sy = kOverworldMinSubchunk; sy < kOverworldMinSubchunk + kOverworldSubchunks; ++sy) {
                        const i32 y0 = sy * 16;
                        auto& biomes = biome_storages[static_cast<usize>(sy - kOverworldMinSubchunk)];
                        biomes.fill(static_cast<u32>(o.default_biome));

                        const bool has_flat = y0 <= flat_top;
                        const i32 ly0 = std::max(y0, origin.y), ly1 = std::min(y0 + 15, origin.y + static_cast<i32>(size.y) - 1);
                        const bool has_structure = ly0 <= ly1;
                        if (!has_flat && !has_structure) continue;

                        std::array<u32, kSectionVolume> blocks;  // Bridger order
                        std::array<u32, kSectionVolume> liquids; // Bridger order
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
                        // Heights and emptiness.
                        bool any_block = false;
                        for (i32 y = 15; y >= 0; --y)
                            for (usize zx = 0; zx < 256; ++zx)
                                if (blocks[(static_cast<usize>(y) << 8) | zx] != o.air_state) {
                                    any_block = true;
                                    if (highest[zx] < y0 + y) highest[zx] = y0 + y;
                                }
                        if (!any_block && !any_liquid) continue;

                        PaletteLayer layer0;
                        layer0.assign(blocks);
                        PaletteLayer layer1;
                        std::vector<const PaletteLayer*> layers{&layer0};
                        if (any_liquid) {
                            layer1.assign(liquids);
                            layers.push_back(&layer1);
                        }
                        BRIDGER_TRY(const std::vector<u8> encoded, encode_subchunk(static_cast<i8>(sy), layers, state_nbt));
                        BRIDGER_TRY_STATUS(db.put(chunk_key(cx, cz, Dimension::Overworld, ChunkTag::SubChunkPrefix, static_cast<i8>(sy)), encoded));
                        ++st.subchunks;
                    }

                    // Data3D: 256 i16 heights (z-major) relative to the world bottom, then 24 biome storages.
                    io::BinaryWriter d3(Endian::Little);
                    for (usize z = 0; z < 16; ++z)
                        for (usize x = 0; x < 16; ++x) {
                            const i32 h = highest[(z << 4) | x];
                            d3.write<i16>(static_cast<i16>(h == std::numeric_limits<i32>::min() ? 0 : h - kOverworldMinY + 1));
                        }
                    for (const auto& storage : biome_storages) write_biome_storage(d3, storage);
                    BRIDGER_TRY_STATUS(db.put(chunk_key(cx, cz, Dimension::Overworld, ChunkTag::Data3D), d3.view()));

                    if (const auto it = entities.find({cx, cz}); it != entities.end()) {
                        io::BinaryWriter bew(Endian::Little);
                        for (const BlockEntity* be : it->second) {
                            const BlockPos w{be->pos.x + origin.x, be->pos.y + origin.y, be->pos.z + origin.z};
                            BRIDGER_TRY_STATUS(append_block_entity(bew, *be, w, origin));
                            ++st.block_entities;
                        }
                        BRIDGER_TRY_STATUS(db.put(chunk_key(cx, cz, Dimension::Overworld, ChunkTag::BlockEntity), bew.view()));
                    }

                    const u8 version = kChunkVersion;
                    BRIDGER_TRY_STATUS(db.put(chunk_key(cx, cz, Dimension::Overworld, ChunkTag::Version), std::span<const u8>(&version, 1)));
                    std::array<u8, 4> finalized{};
                    io::store_le<i32>(finalized.data(), kFinalizedDone);
                    BRIDGER_TRY_STATUS(db.put(chunk_key(cx, cz, Dimension::Overworld, ChunkTag::FinalizedState), finalized));
                    ++st.chunks;
                }
            }
        }
        BRIDGER_TRY_STATUS(db.compact());
    } // database closed: the directory is complete

    BRIDGER_TRY_STATUS(write_file(work_dir / "level.dat", encode_level_dat(o.level)));
    const std::string name = o.level.level_name;
    BRIDGER_TRY_STATUS(write_file(work_dir / "levelname.txt",
                                  std::span<const u8>(reinterpret_cast<const u8*>(name.data()), name.size())));

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

    BRIDGER_TRY(LevelDbStore db, LevelDbStore::open(work_dir / "db", /*create_new=*/false));
    WorldWriteStats st;
    Status failure;
    BRIDGER_TRY_STATUS(db.for_each([&](std::string_view key, std::span<const u8> value) {
        if (!failure) return;
        if (key.size() == 10 && static_cast<u8>(key[8]) == static_cast<u8>(ChunkTag::SubChunkPrefix)) {
            if (auto d = decode_subchunk(value); !d) failure = tl::unexpected(std::move(d).error());
            else ++st.subchunks;
        } else if (key.size() == 9 && static_cast<u8>(key[8]) == static_cast<u8>(ChunkTag::BlockEntity)) {
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
        } else if (key.size() == 9 && static_cast<u8>(key[8]) == static_cast<u8>(ChunkTag::Version)) {
            ++st.chunks;
        }
    }));
    if (!failure) return make_error(ErrorCode::ValidationFailed, "invalid world data: " + failure.error().describe());
    if (stats != nullptr) *stats = st;
    return {};
}

} // namespace bridger::formats::bedrock

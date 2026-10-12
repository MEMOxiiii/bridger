// Java world writer. Formats and sources: docs/formats/anvil.md ("Writing").

#include <bridger/formats/java/anvil/anvil_writer.hpp>
#include <bridger/io/compression.hpp>
#include <bridger/io/endian.hpp>
#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/platform/paths.hpp>

#include <algorithm>
#include <bit>
#include <fstream>

namespace bridger::formats::java {
namespace fs = std::filesystem;
using io::nbt::NbtWriter;
using io::nbt::TagType;

namespace {

constexpr usize kSector = 4096;

// 1.16+ packing: 64 / bits indices per long, never spanning two longs, LSB first.
std::vector<i64> pack(std::span<const u32> indices, u32 bits) {
    const usize per = 64 / bits;
    std::vector<i64> out((indices.size() + per - 1) / per, 0);
    for (usize i = 0; i < indices.size(); ++i)
        out[i / per] = static_cast<i64>(static_cast<u64>(out[i / per]) | (u64{indices[i]} << ((i % per) * bits)));
    return out;
}

u32 bits_for(usize n) { return n <= 1 ? 0 : static_cast<u32>(std::bit_width(n - 1)); }

Status write_file(const fs::path& p, std::span<const u8> bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) return make_error(ErrorCode::IoError, "cannot write " + platform::path_to_utf8(p).value_or("?"));
    return {};
}

} // namespace

Result<std::vector<u8>> encode_chunk(const Structure& col, ChunkPos pos, i32 min_y, i32 data_version) {
    if (col.source().edition != Edition::Java) return make_error(ErrorCode::InvalidArgument, "encode_chunk needs a Java column");
    if (col.size().x != 16 || col.size().z != 16 || (min_y & 15) != 0 || col.size().y % 16 != 0)
        return make_error(ErrorCode::InvalidArgument, "encode_chunk needs a 16-wide column on section boundaries");
    const BlockStateRegistry& reg = col.block_registry();
    BiomeRegistry& biome_names = col.biome_registry();
    const u32 plains = biome_names.intern("minecraft:plains"); // sections without biome data

    NbtWriter w(Endian::Big);
    w.begin_root_compound("");
    w.write_int("DataVersion", data_version);
    w.write_int("xPos", pos.x);
    w.write_int("yPos", min_y >> 4);
    w.write_int("zPos", pos.z);
    w.write_string("Status", "minecraft:full");
    w.write_long("LastUpdate", 0);
    w.write_long("InhabitedTime", 0);
    w.write_byte("isLightOn", 0); // the game computes light for chunks without it

    std::array<u32, kSectionVolume> ids{};
    std::array<u32, kSectionVolume> idx{};
    std::vector<u32> palette;
    w.begin_list("sections", TagType::Compound, col.sections_y());
    for (u32 sy = 0; sy < col.sections_y(); ++sy) {
        const Section& sec = col.section(0, sy, 0);
        w.begin_compound();
        w.write_byte("Y", static_cast<i8>((min_y >> 4) + static_cast<i32>(sy)));

        // block_states: palette of {id, properties} (the 26.3 BlockState codec) + packed indices.
        sec.blocks().copy_to(ids);
        palette.clear();
        for (usize i = 0; i < kSectionVolume; ++i) {
            auto it = std::find(palette.begin(), palette.end(), ids[i]);
            if (it == palette.end()) {
                palette.push_back(ids[i]);
                it = palette.end() - 1;
            }
            idx[i] = static_cast<u32>(it - palette.begin());
        }
        w.begin_compound("block_states");
        w.begin_list("palette", TagType::Compound, palette.size());
        for (const u32 id : palette) {
            const BlockState& st = reg.get(id);
            w.begin_compound();
            w.write_string("id", io::nbt::utf8_to_mutf8(st.id()).value_or(""));
            if (!st.properties().empty()) {
                w.begin_compound("properties");
                for (const BlockProperty& p : st.properties())
                    w.write_string(io::nbt::utf8_to_mutf8(p.name).value_or(""), io::nbt::utf8_to_mutf8(p.value).value_or(""));
                w.end_compound();
            }
            w.end_compound();
        }
        w.end_list();
        if (palette.size() > 1) w.write_long_array("data", pack(idx, std::max<u32>(4, bits_for(palette.size()))));
        w.end_compound();

        // biomes: 4x4x4 cells (the biome at each cell's lowest corner), palette of names.
        std::array<u32, 64> cells{};
        std::vector<u32> bpal;
        for (u32 c = 0; c < 64; ++c) {
            const i32 bx = static_cast<i32>(c & 3) * 4, bz = static_cast<i32>((c >> 2) & 3) * 4, by = static_cast<i32>(c >> 4) * 4;
            const u32 b = sec.biome(bx, by, bz).value_or(plains);
            auto it = std::find(bpal.begin(), bpal.end(), b);
            if (it == bpal.end()) {
                bpal.push_back(b);
                it = bpal.end() - 1;
            }
            cells[c] = static_cast<u32>(it - bpal.begin());
        }
        w.begin_compound("biomes");
        w.begin_list("palette", TagType::String, bpal.size());
        for (const u32 b : bpal) w.write_string({}, io::nbt::utf8_to_mutf8(biome_names.name(b)).value_or(""));
        w.end_list();
        if (bpal.size() > 1) w.write_long_array("data", pack(cells, bits_for(bpal.size())));
        w.end_compound();
        w.end_compound();
    }
    w.end_list();

    w.begin_list("block_entities", col.block_entities.empty() ? TagType::End : TagType::Compound, col.block_entities.size());
    for (const BlockEntity& be : col.block_entities) {
        w.begin_compound();
        w.write_string("id", io::nbt::utf8_to_mutf8(be.id).value_or(""));
        w.write_int("x", be.pos.x + pos.x * 16);
        w.write_int("y", be.pos.y + min_y);
        w.write_int("z", be.pos.z + pos.z * 16);
        w.write_byte("keepPacked", 0);
        if (!be.data.empty()) {
            BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian));
            for (const io::nbt::NbtEntry& e : doc.root().entries())
                if (e.name != "id" && e.name != "x" && e.name != "y" && e.name != "z" && e.name != "keepPacked")
                    w.write_view(e.name, e.value);
        }
        w.end_compound();
    }
    w.end_list();
    w.begin_compound("structures");
    w.begin_compound("References");
    w.end_compound();
    w.begin_compound("starts");
    w.end_compound();
    w.end_compound();
    w.end_compound();
    return std::move(w).finish();
}

Status RegionWriter::add(u32 lx, u32 lz, std::span<const u8> nbt) {
    if (lx > 31 || lz > 31) return make_error(ErrorCode::InvalidArgument, "region: local chunk coordinate out of range");
    BRIDGER_TRY(compressed_[lx + lz * 32], io::compress(nbt, io::Compression::Zlib, 6));
    return {};
}

usize RegionWriter::chunk_count() const noexcept {
    return static_cast<usize>(std::count_if(compressed_.begin(), compressed_.end(), [](const auto& c) { return !c.empty(); }));
}

Status RegionWriter::write(const fs::path& dir) const {
    std::vector<u8> file(2 * kSector, 0);
    for (u32 i = 0; i < 1024; ++i) {
        const std::vector<u8>& data = compressed_[i];
        if (data.empty()) continue;
        const usize offset_sectors = file.size() / kSector;
        std::vector<u8> payload(5);
        if (data.size() + 5 > 255 * kSector) {
            // Too large for the region: external .mcc file, compression byte + 128 (docs/formats/anvil.md).
            const i32 cx = rx_ * 32 + static_cast<i32>(i % 32), cz = rz_ * 32 + static_cast<i32>(i / 32);
            BRIDGER_TRY(const fs::path name, platform::path_from_utf8("c." + std::to_string(cx) + "." + std::to_string(cz) + ".mcc"));
            BRIDGER_TRY_STATUS(write_file(dir / name, data));
            io::store_be<i32>(payload.data(), 1);
            payload[4] = 2 | 0x80;
        } else {
            io::store_be<i32>(payload.data(), static_cast<i32>(data.size() + 1));
            payload[4] = 2; // zlib
            payload.insert(payload.end(), data.begin(), data.end());
        }
        payload.resize((payload.size() + kSector - 1) / kSector * kSector, 0);
        const usize sectors = payload.size() / kSector;
        if (offset_sectors > 0xFFFFFF) return make_error(ErrorCode::LimitExceeded, "region file too large");
        const u32 loc = static_cast<u32>(offset_sectors << 8) | static_cast<u32>(sectors);
        io::store_be<u32>(file.data() + 4 * i, loc);
        io::store_be<i32>(file.data() + kSector + 4 * i, 0); // timestamp
        file.insert(file.end(), payload.begin(), payload.end());
    }
    BRIDGER_TRY(const fs::path name, platform::path_from_utf8("r." + std::to_string(rx_) + "." + std::to_string(rz_) + ".mca"));
    return write_file(dir / name, file);
}

Result<std::vector<u8>> encode_java_level_dat(const JavaLevelOptions& o) {
    // Fields from minecraft.wiki "Java Edition level format" and the 26.3 server (PrimaryLevelData,
    // LevelData.RespawnData: yaw/pitch are floats). Everything not written takes the game's default.
    NbtWriter w(Endian::Big);
    w.begin_root_compound("");
    w.begin_compound("Data");
    w.write_int("DataVersion", o.data_version);
    w.write_int("version", 19133); // Anvil
    w.begin_compound("Version");
    w.write_int("Id", o.data_version);
    w.write_string("Name", o.version_name);
    w.write_string("Series", "main");
    w.write_byte("Snapshot", 0);
    w.end_compound();
    w.write_string("LevelName", io::nbt::utf8_to_mutf8(o.level_name).value_or("Bridger World"));
    w.write_long("LastPlayed", o.last_played_ms);
    w.write_int("GameType", o.game_type);
    w.write_byte("allowCommands", 1);
    w.write_byte("initialized", 1);
    w.write_long("Time", 0);
    w.begin_compound("spawn");
    w.write_string("dimension", "minecraft:overworld");
    const std::array<i32, 3> pos{o.spawn.x, o.spawn.y, o.spawn.z};
    w.write_int_array("pos", pos);
    w.write_float("yaw", 0.0f);
    w.write_float("pitch", 0.0f);
    w.end_compound();
    w.begin_compound("DataPacks");
    w.begin_list("Enabled", TagType::String, 1);
    w.write_string({}, "vanilla");
    w.end_list();
    w.begin_list("Disabled", TagType::End, 0);
    w.end_list();
    w.end_compound();
    w.end_compound();
    w.end_compound();
    BRIDGER_TRY(const std::vector<u8> raw, std::move(w).finish());
    return io::compress(raw, io::Compression::Gzip, 6);
}

Result<std::vector<u8>> encode_world_gen_settings(i64 seed, i32 data_version) {
    // Saved-data file {DataVersion, data: {...}} (26.3 SavedDataStorage); WorldOptions fields.
    // "dimensions" is not written. Whether 26.3 accepts that or falls back to its defaults with a
    // random seed (its logged behavior for an unreadable file) is an open in-game check (anvil.md).
    NbtWriter w(Endian::Big);
    w.begin_root_compound("");
    w.write_int("DataVersion", data_version);
    w.begin_compound("data");
    w.write_long("seed", seed);
    w.write_byte("generate_structures", 1);
    w.write_byte("bonus_chest", 0);
    w.end_compound();
    w.end_compound();
    BRIDGER_TRY(const std::vector<u8> raw, std::move(w).finish());
    return io::compress(raw, io::Compression::Gzip, 6);
}

} // namespace bridger::formats::java

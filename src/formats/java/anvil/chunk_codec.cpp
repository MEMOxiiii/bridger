// Java chunk NBT (1.18+), docs/formats/anvil.md.

#include "../nbt_helpers.hpp"

#include <bridger/formats/java/anvil/chunk_codec.hpp>

#include <array>
#include <bit>
#include <string>

namespace bridger::formats::java {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::TagType;

namespace {

// Packed indices: `bits` per entry, 64/bits entries per long, entries never span longs.
// Returns false if the array is too short for `count` entries.
bool unpack(io::nbt::NbtArrayView<i64> data, u32 bits, usize count, u32* out) {
    const usize per_long = 64 / bits;
    if (data.size() < (count + per_long - 1) / per_long) return false;
    const u64 mask = (u64{1} << bits) - 1;
    for (usize i = 0; i < count; ++i) {
        const u64 word = static_cast<u64>(data[i / per_long]);
        out[i] = static_cast<u32>((word >> ((i % per_long) * bits)) & mask);
    }
    return true;
}

u32 bits_for(usize palette_size) { return palette_size <= 1 ? 0 : static_cast<u32>(std::bit_width(palette_size - 1)); }

Result<BlockState> palette_state(NbtView entry) {
    // Compound {Name, Properties} / {id, properties}; plain strings are accepted for robustness.
    if (const auto s = entry.as_string()) {
        BRIDGER_TRY(std::string id, detail::utf8(*s));
        if (id.find(':') == std::string::npos) id = "minecraft:" + id;
        return BlockState(std::move(id), {});
    }
    if (!entry.is(TagType::Compound)) return make_error(ErrorCode::Malformed, "block palette entry is not a compound");
    return detail::state_from_compound(entry.entries());
}

} // namespace

Result<JavaChunk> decode_chunk(std::span<const u8> nbt, std::shared_ptr<BlockStateRegistry> blocks,
                               std::shared_ptr<BiomeRegistry> biomes, const io::nbt::NbtLimits& limits) {
    BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(nbt, Endian::Big, limits));
    if (!doc.root().is(TagType::Compound)) return make_error(ErrorCode::Malformed, "chunk: root is not a compound");
    const NbtCompoundView root = doc.root().entries();
    JavaChunk out;
    out.data_version = root.find("DataVersion").as_int().value_or(0);
    if (root.find("Level", TagType::Compound) || (out.data_version != 0 && out.data_version < kFirstFlatteningChunkDataVersion))
        return make_error(ErrorCode::UnsupportedVersion,
                          "chunk: pre-1.18 layout (DataVersion " + std::to_string(out.data_version) +
                              "); open and save the world in Java 1.18 or newer first");
    const auto x = root.find("xPos").as_int(), z = root.find("zPos").as_int();
    if (!x || !z) return make_error(ErrorCode::Malformed, "chunk: missing xPos/zPos");
    out.pos = {*x, *z};
    if (const auto s = root.find("Status").as_string()) out.full = (*s == "minecraft:full" || *s == "full");

    const NbtView sections = root.find("sections", TagType::List);
    const auto y_pos = root.find("yPos").as_int();
    // Section Y is a Byte, so the lowest section must be too (found by fuzzing: yPos -65526).
    if (y_pos && (*y_pos < -128 || *y_pos > 127)) return make_error(ErrorCode::Malformed, "chunk: yPos outside the section range");
    i32 min_sy = y_pos.value_or(INT32_MAX);
    i32 max_sy = INT32_MIN;
    for (const NbtView sec : sections.items()) {
        const auto y = sec.entries().find("Y").as_byte();
        if (!y) return make_error(ErrorCode::Malformed, "chunk: section without Y");
        min_sy = std::min<i32>(min_sy, *y);
        max_sy = std::max<i32>(max_sy, *y);
    }
    if (max_sy == INT32_MIN) { // no sections at all
        min_sy = min_sy == INT32_MAX ? -4 : min_sy;
        max_sy = min_sy;
    }
    if (max_sy - min_sy >= 256) return make_error(ErrorCode::Malformed, "chunk: section range too large");
    out.min_y = min_sy * 16;
    const u32 height = static_cast<u32>(max_sy - min_sy + 1) * 16;

    const SourceVersion source{Edition::Java, static_cast<u32>(out.data_version)};
    BRIDGER_TRY(out.column, Structure::create(blocks, biomes, Extent{16, height, 16}, source));
    Structure& col = out.column;

    std::array<u32, kSectionVolume> idx{};
    std::array<u32, 64> bidx{};
    std::vector<StateId> palette;
    std::vector<BiomeId> bpalette;
    for (const NbtView sec_view : sections.items()) {
        const NbtCompoundView sec = sec_view.entries();
        const i32 sy = *sec.find("Y").as_byte();
        Section& section = col.section(0, static_cast<u32>(sy - min_sy), 0);
        const std::string where = "chunk " + std::to_string(out.pos.x) + "," + std::to_string(out.pos.z) + " section " +
                                  std::to_string(sy) + ": ";

        if (const NbtView bs = sec.find("block_states", TagType::Compound)) {
            const NbtCompoundView c = bs.entries();
            palette.clear();
            for (const NbtView e : c.find("palette").items()) {
                if (palette.size() >= kSectionVolume) return make_error(ErrorCode::Malformed, where + "palette too large");
                BRIDGER_TRY(const BlockState st, palette_state(e));
                palette.push_back(blocks->intern(st));
            }
            if (palette.empty()) return make_error(ErrorCode::Malformed, where + "empty block palette");
            if (palette.size() == 1) {
                section.blocks().fill(palette[0]);
            } else {
                const auto data = c.find("data").as_long_array();
                if (!data || !unpack(*data, std::max<u32>(4, bits_for(palette.size())), kSectionVolume, idx.data()))
                    return make_error(ErrorCode::Malformed, where + "block data missing or too short");
                for (u32& v : idx) {
                    if (v >= palette.size()) return make_error(ErrorCode::Malformed, where + "block index outside the palette");
                    v = palette[v];
                }
                section.blocks().assign(idx);
            }
        }

        if (const NbtView bm = sec.find("biomes", TagType::Compound)) {
            const NbtCompoundView c = bm.entries();
            bpalette.clear();
            for (const NbtView e : c.find("palette").items()) {
                if (bpalette.size() >= 64) return make_error(ErrorCode::Malformed, where + "biome palette too large");
                const auto s = e.is(TagType::Compound) ? e.entries().find("Name").as_string() : e.as_string();
                if (!s) return make_error(ErrorCode::Malformed, where + "biome palette entry is not a string");
                BRIDGER_TRY(std::string name, detail::utf8(*s));
                if (name.find(':') == std::string::npos) name = "minecraft:" + name;
                bpalette.push_back(biomes->intern(name));
            }
            if (bpalette.size() == 1) {
                section.ensure_biomes(bpalette[0]).fill(bpalette[0]);
            } else if (bpalette.size() > 1) {
                const auto data = c.find("data").as_long_array();
                if (!data || !unpack(*data, bits_for(bpalette.size()), 64, bidx.data()))
                    return make_error(ErrorCode::Malformed, where + "biome data missing or too short");
                for (u32 i = 0; i < kSectionVolume; ++i) {
                    const u32 bx = (i & 15) >> 2, bz = ((i >> 4) & 15) >> 2, by = (i >> 8) >> 2;
                    const u32 b = bidx[by * 16 + bz * 4 + bx];
                    if (b >= bpalette.size()) return make_error(ErrorCode::Malformed, where + "biome index outside the palette");
                    idx[i] = bpalette[b];
                }
                section.ensure_biomes(bpalette[0]).assign(idx);
            }
        }
    }

    // Block entities use absolute world coordinates.
    for (const NbtView be_view : root.find("block_entities").items()) {
        const NbtCompoundView c = be_view.entries();
        const auto id = c.find("id").as_string();
        const auto pos = detail::xyz(c);
        if (!id || !pos) {
            out.warnings.push_back("chunk " + std::to_string(out.pos.x) + "," + std::to_string(out.pos.z) +
                                   ": block entity without id or position was not imported");
            continue;
        }
        const BlockPos p{(*pos)[0] - out.pos.x * 16, (*pos)[1] - out.min_y, (*pos)[2] - out.pos.z * 16};
        if (!col.contains(p)) {
            out.warnings.push_back("chunk " + std::to_string(out.pos.x) + "," + std::to_string(out.pos.z) +
                                   ": block entity outside the chunk was not imported");
            continue;
        }
        BlockEntity be;
        be.pos = p;
        BRIDGER_TRY(be.id, detail::utf8(*id));
        if (be.id.find(':') == std::string::npos) be.id = "minecraft:" + be.id;
        BRIDGER_TRY(be.data, detail::encode_compound(c, {"id", "x", "y", "z", "keepPacked"}));
        col.block_entities.push_back(std::move(be));
    }
    return out;
}

} // namespace bridger::formats::java

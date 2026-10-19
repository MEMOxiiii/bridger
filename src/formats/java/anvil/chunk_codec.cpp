// Java chunk NBT, docs/formats/anvil.md: 1.18+ and the older layouts ("Old worlds").

#include "../java_upgrade.hpp"
#include "../legacy_blocks.hpp"
#include "../nbt_helpers.hpp"

#include <bridger/formats/java/anvil/chunk_codec.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <set>
#include <string>
#include <unordered_map>

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

// 1.13 - 20w16a: indices span long boundaries (a 4096-entry run of `bits`-bit fields). The layout
// is told apart from the 20w17a+ one by the array length; for 4, 8 and 16 bits they are equal.
bool unpack_spanning(io::nbt::NbtArrayView<i64> data, u32 bits, usize count, u32* out) {
    if (data.size() * 64 < count * bits) return false;
    const u64 mask = (u64{1} << bits) - 1;
    for (usize i = 0; i < count; ++i) {
        const usize bit = i * bits, word = bit / 64, shift = bit % 64;
        u64 v = static_cast<u64>(data[word]) >> shift;
        if (shift + bits > 64) v |= static_cast<u64>(data[word + 1]) << (64 - shift);
        out[i] = static_cast<u32>(v & mask);
    }
    return true;
}

// Nibble arrays (Data, Add): the value of an even index is in the low four bits [anvil.md].
u8 nibble(std::span<const u8> a, usize i) { return static_cast<u8>((a[i >> 1] >> ((i & 1) * 4)) & 0xF); }

std::string chunk_where(ChunkPos p) { return "chunk " + std::to_string(p.x) + "," + std::to_string(p.z); }

// Java entity compounds -> core Entity (absolute "Pos" doubles -> column-local position).
Status read_entities(NbtView list, ChunkPos pos, i32 min_y, std::vector<Entity>& out, std::vector<std::string>& warnings) {
    for (const NbtView e : list.items()) {
        const NbtCompoundView c = e.entries();
        const auto id = c.find("id").as_string();
        const io::nbt::NbtListView p = c.find("Pos").items();
        if (!id || p.size() != 3 || !p.at(0).as_double()) {
            warnings.push_back("chunk " + std::to_string(pos.x) + "," + std::to_string(pos.z) + ": an entity without id or position was skipped");
            continue;
        }
        Entity ent;
        BRIDGER_TRY(ent.id, detail::utf8(*id));
        if (ent.id.find(':') == std::string::npos) {
            // Before 1.11 ids were CamelCase ("ItemFrame", "Painting", "Pig"): snake_case gives the 1.11+ name for
            // these (minecraft.wiki "Java Edition 1.11", entity renames); the rest is only reported.
            std::string snake;
            for (const char ch : ent.id) {
                if (ch >= 'A' && ch <= 'Z') {
                    if (!snake.empty()) snake += '_';
                    snake += static_cast<char>(ch - 'A' + 'a');
                } else {
                    snake += ch;
                }
            }
            ent.id = "minecraft:" + snake;
        }
        ent.pos = {*p.at(0).as_double() - pos.x * 16.0, *p.at(1).as_double() - min_y, *p.at(2).as_double() - pos.z * 16.0};
        BRIDGER_TRY(ent.data, detail::encode_compound(c, {"id"}));
        out.push_back(std::move(ent));
    }
    return {};
}

// Pre-1.18 chunks: everything under a "Level" compound; sections with a 1.13+ palette, or numeric
// ids (Anvil, 1.2 - 1.12), or one 128-high column of numeric ids (McRegion, Beta 1.3 - 1.1).
Result<JavaChunk> decode_old_chunk(NbtCompoundView level, i32 data_version, std::shared_ptr<BlockStateRegistry> blocks,
                                   std::shared_ptr<BiomeRegistry> biomes, const LegacyTables* legacy) {
    JavaChunk out;
    out.data_version = data_version;
    out.min_y = 0;
    const auto x = level.find("xPos").as_int(), z = level.find("zPos").as_int();
    if (!x || !z) return make_error(ErrorCode::Malformed, "chunk: missing xPos/zPos");
    out.pos = {*x, *z};
    const std::string where = chunk_where(out.pos);
    // Generation status: "full" (1.14+), "postprocessed"/"fullchunk" (1.13), else TerrainPopulated
    // (before 1.13; absent = populated).
    if (const auto st = level.find("Status").as_string()) {
        const std::string_view v = *st;
        out.full = v == "full" || v == "minecraft:full" || v == "postprocessed" || v == "fullchunk";
    } else {
        out.full = level.find("TerrainPopulated").as_byte().value_or(1) != 0;
    }

    const auto column_ids = level.find("Blocks").as_byte_array(); // McRegion: the whole column
    const bool mcregion = column_ids.has_value();
    const u32 height = mcregion ? 128 : 256;
    const SourceVersion source{Edition::Java, static_cast<u32>(std::max(0, data_version))};
    BRIDGER_TRY(out.column, Structure::create(blocks, biomes, Extent{16, height, 16}, source));
    Structure& col = out.column;

    // Numeric ids -> state, once per (id, data); approximations as in the legacy schematic reader.
    std::unordered_map<u32, StateId> numeric;
    u64 approximated = 0, unknown = 0;
    auto resolve = [&](u16 id, u8 dv) -> StateId {
        const u32 key = (u32{id} << 4) | dv;
        if (const auto it = numeric.find(key); it != numeric.end()) return it->second;
        std::optional<std::string> text = legacy->block(id, dv);
        if (!text && (dv & 8)) { text = legacy->block(id, static_cast<u8>(dv & 7)); approximated += text ? 1u : 0u; }
        if (!text && dv != 0) { text = legacy->block(id, 0); approximated += text ? 1u : 0u; }
        std::optional<BlockState> st;
        if (text) if (auto parsed = BlockState::parse(*text)) st = std::move(*parsed);
        if (!st) {
            ++unknown;
            st = BlockState("bridger:legacy_" + std::to_string(id) + "_" + std::to_string(dv), {});
        }
        const StateId sid = blocks->intern(*st);
        numeric.emplace(key, sid);
        return sid;
    };
    auto need_legacy = [&]() -> Status {
        if (legacy == nullptr) return make_error(ErrorCode::UnsupportedVersion, where + ": numeric block ids need the legacy table");
        return {};
    };

    std::array<u32, kSectionVolume> idx{};
    if (mcregion) {
        BRIDGER_TRY_STATUS(need_legacy());
        const auto data = level.find("Data").as_byte_array();
        if (column_ids->size() < 32768 || !data || data->size() < 16384) return make_error(ErrorCode::Malformed, where + ": Blocks/Data too short");
        for (u32 sy = 0; sy < 8; ++sy) {
            for (u32 i = 0; i < kSectionVolume; ++i) { // Bridger order y*256 + z*16 + x
                const usize bx = i & 15, bz = (i >> 4) & 15, by = sy * 16 + (i >> 8);
                const usize at = (bx << 11) | (bz << 7) | by; // McRegion: x*2048 + z*128 + y [anvil.md]
                idx[i] = resolve((*column_ids)[at], nibble(*data, at));
            }
            col.section(0, sy, 0).blocks().assign(idx);
        }
    }

    std::vector<StateId> palette;
    for (const NbtView sec_view : level.find("Sections").items()) {
        if (mcregion) break;
        const NbtCompoundView sec = sec_view.entries();
        const auto y = sec.find("Y").as_byte();
        if (!y) return make_error(ErrorCode::Malformed, where + ": section without Y");
        if (*y < 0 || *y >= 16) continue; // 1.14+ keep light-only sections at -1 and 16
        Section& section = col.section(0, static_cast<u32>(*y), 0);
        const std::string swhere = where + " section " + std::to_string(*y) + ": ";

        if (const NbtView pal = sec.find("Palette", TagType::List)) { // 1.13 - 1.17
            palette.clear();
            for (const NbtView e : pal.items()) {
                if (palette.size() >= kSectionVolume) return make_error(ErrorCode::Malformed, swhere + "palette too large");
                if (!e.is(TagType::Compound)) return make_error(ErrorCode::Malformed, swhere + "palette entry is not a compound");
                BRIDGER_TRY(BlockState st, detail::state_from_compound(e.entries()));
                palette.push_back(blocks->intern(detail::upgraded(std::move(st), data_version)));
            }
            if (palette.empty()) continue;
            if (palette.size() == 1) {
                section.blocks().fill(palette[0]);
                continue;
            }
            const auto data = sec.find("BlockStates").as_long_array();
            const u32 bits = std::max<u32>(4, bits_for(palette.size()));
            const usize packed = (kSectionVolume + 64 / bits - 1) / (64 / bits);
            bool ok = false;
            if (data && data->size() == packed) ok = unpack(*data, bits, kSectionVolume, idx.data());
            else if (data && data->size() == kSectionVolume * bits / 64) ok = unpack_spanning(*data, bits, kSectionVolume, idx.data());
            if (!ok) return make_error(ErrorCode::Malformed, swhere + "BlockStates missing or of the wrong length");
            for (u32& v : idx) {
                if (v >= palette.size()) return make_error(ErrorCode::Malformed, swhere + "block index outside the palette");
                v = palette[v];
            }
            section.blocks().assign(idx);
        } else if (const auto ids = sec.find("Blocks").as_byte_array()) { // 1.2 - 1.12
            BRIDGER_TRY_STATUS(need_legacy());
            const auto data = sec.find("Data").as_byte_array();
            const auto add = sec.find("Add").as_byte_array();
            if (ids->size() < kSectionVolume || (data && data->size() < kSectionVolume / 2) || (add && add->size() < kSectionVolume / 2))
                return make_error(ErrorCode::Malformed, swhere + "Blocks/Data/Add too short");
            for (u32 i = 0; i < kSectionVolume; ++i) { // YZX: the same order as Bridger's
                u16 id = (*ids)[i];
                if (add) id = static_cast<u16>(id | (nibble(*add, i) << 8));
                idx[i] = resolve(id, data ? nibble(*data, i) : u8{0});
            }
            section.blocks().assign(idx);
        }
    }
    if (approximated > 0) out.warnings.push_back(where + ": " + std::to_string(approximated) + " legacy id/data pair(s) used the nearest data value");
    if (unknown > 0) out.warnings.push_back(where + ": " + std::to_string(unknown) + " legacy block id(s) are unknown; they are reported as unmapped");

    // Biomes: 1024 ints (19w36a+: 4 x 4 x 4 cells, index (y/4)*16 + (z/4)*4 + x/4), 256 ints (1.13 -
    // 1.14) or 256 bytes (1.2 - 1.12), index z*16 + x. Old numeric ids, upgraded by java_upgrade.cpp.
    std::unordered_map<i32, BiomeId> biome_ids;
    usize merged = 0;
    std::set<i32> unknown_biomes;
    auto biome = [&](i32 id) -> BiomeId {
        if (const auto it = biome_ids.find(id); it != biome_ids.end()) return it->second;
        auto b = detail::legacy_biome(id);
        if (!b) {
            if (id != -1 && id != 255) unknown_biomes.insert(id);
            b = detail::legacy_biome(1); // plains, as the game fills missing entries
        } else if (b->merged) {
            ++merged;
        }
        const BiomeId v = biomes->intern(std::string(b->name));
        biome_ids.emplace(id, v);
        return v;
    };
    const NbtView bv = level.find("Biomes");
    std::vector<i32> raw;
    if (const auto ints = bv.as_int_array()) raw = ints->to_vector();
    else if (const auto bytes = bv.as_byte_array()) for (const u8 b : *bytes) raw.push_back(b);
    if (raw.size() == 1024 || raw.size() == 256) {
        for (u32 sy = 0; sy < col.sections_y(); ++sy) {
            for (u32 i = 0; i < kSectionVolume; ++i) {
                const u32 bx = i & 15, bz = (i >> 4) & 15, by = sy * 16 + (i >> 8);
                const usize at = raw.size() == 1024 ? ((by >> 2) << 4) | ((bz >> 2) << 2) | (bx >> 2) : (bz << 4) | bx;
                idx[i] = biome(raw[at]);
            }
            col.section(0, sy, 0).ensure_biomes(idx[0]).assign(idx);
        }
    }
    if (merged > 0) out.warnings.push_back(where + ": biomes that 1.18 merged into their base biome were converted to it");
    for (const i32 id : unknown_biomes) out.warnings.push_back(where + ": unknown biome id " + std::to_string(id) + " read as plains");

    // Block entities (absolute coordinates). Numeric-id chunks get the legacy rewrite and fixups.
    const bool numeric_ids = mcregion || !numeric.empty() || data_version < 1444;
    for (const NbtView be_view : level.find("TileEntities").items()) {
        const NbtCompoundView c = be_view.entries();
        const auto pos = detail::xyz(c);
        if (!pos) {
            out.warnings.push_back(where + ": block entity without a position was not imported");
            continue;
        }
        const BlockPos p{(*pos)[0] - out.pos.x * 16, (*pos)[1] - out.min_y, (*pos)[2] - out.pos.z * 16};
        if (!col.contains(p)) {
            out.warnings.push_back(where + ": block entity outside the chunk was not imported");
            continue;
        }
        if (numeric_ids && legacy != nullptr) {
            BRIDGER_TRY(std::optional<BlockEntity> be, detail::legacy_block_entity(c, p, *legacy, out.warnings));
            if (be) col.block_entities.push_back(std::move(*be));
            continue;
        }
        const auto id = c.find("id").as_string();
        if (!id) {
            out.warnings.push_back(where + ": block entity without an id was not imported");
            continue;
        }
        BlockEntity be;
        be.pos = p;
        BRIDGER_TRY(const std::string raw_id, detail::utf8(*id));
        be.id = detail::normalize_block_entity_id(raw_id);
        BRIDGER_TRY(be.data, detail::encode_compound(c, {"id", "x", "y", "z", "keepPacked"}));
        col.block_entities.push_back(std::move(be));
    }
    if (numeric_ids) detail::fix_legacy_blocks(col, out.warnings);
    BRIDGER_TRY_STATUS(read_entities(level.find("Entities"), out.pos, out.min_y, col.entities, out.warnings));
    return out;
}

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
                               std::shared_ptr<BiomeRegistry> biomes, const io::nbt::NbtLimits& limits, const LegacyTables* legacy) {
    BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(nbt, Endian::Big, limits));
    if (!doc.root().is(TagType::Compound)) return make_error(ErrorCode::Malformed, "chunk: root is not a compound");
    const NbtCompoundView root = doc.root().entries();
    JavaChunk out;
    out.data_version = root.find("DataVersion").as_int().value_or(0);
    if (const NbtView level = root.find("Level", TagType::Compound))
        return decode_old_chunk(level.entries(), out.data_version, std::move(blocks), std::move(biomes), legacy);
    if (out.data_version != 0 && out.data_version < kFirstFlatteningChunkDataVersion)
        return make_error(ErrorCode::Malformed, "chunk: DataVersion " + std::to_string(out.data_version) + " without the Level compound of its time");
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
                BRIDGER_TRY(BlockState st, palette_state(e));
                palette.push_back(blocks->intern(detail::upgraded(std::move(st), out.data_version)));
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

Result<std::vector<Entity>> decode_entity_chunk(std::span<const u8> nbt, ChunkPos pos, i32 min_y, const io::nbt::NbtLimits& limits) {
    BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(nbt, Endian::Big, limits));
    std::vector<Entity> out;
    std::vector<std::string> warnings;
    BRIDGER_TRY_STATUS(read_entities(doc.root().entries().find("Entities"), pos, min_y, out, warnings));
    return out;
}

} // namespace bridger::formats::java

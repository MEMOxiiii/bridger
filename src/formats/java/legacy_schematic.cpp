// Legacy .schematic reader. Format and sources: docs/formats/legacy-schematic.md.

#include "legacy_blocks.hpp"
#include "nbt_helpers.hpp"

#include <bridger/formats/java/legacy_schematic.hpp>

#include <array>
#include <map>
#include <string>
#include <unordered_map>

namespace bridger::formats::java {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::TagType;

Result<StructureFile> read_legacy_schematic(std::span<const u8> file_bytes, std::shared_ptr<BlockStateRegistry> blocks,
                                            std::shared_ptr<BiomeRegistry> biomes, const LegacyTables& tables,
                                            const StructureReadOptions& opt) {
    auto bad = [](const std::string& why) { return make_error(ErrorCode::Malformed, "schematic: " + why); };
    BRIDGER_TRY(const detail::JavaNbt nbt, detail::parse_java_nbt(file_bytes, opt.decompress, opt.nbt, "schematic"));
    const NbtCompoundView root = nbt.doc.root().entries();

    StructureFile out{FormatId::LegacySchematic, std::nullopt, {}, {}};
    const auto materials = root.find("Materials").as_string();
    if (materials && *materials != "Alpha")
        return make_error(ErrorCode::UnsupportedFormat, "schematic: materials '" + std::string(*materials) +
                                                            "' are not supported (only Alpha)");
    if (root.find("Add")) return make_error(ErrorCode::UnsupportedFormat, "schematic: the deprecated 'Add' tag is not supported");

    const auto w = root.find("Width").as_short(), h = root.find("Height").as_short(), l = root.find("Length").as_short();
    if (!w || !h || !l) return bad("missing Width/Height/Length");
    const Extent extent{static_cast<u16>(*w), static_cast<u16>(*h), static_cast<u16>(*l)};
    if (extent.volume() > opt.max_volume) return make_error(ErrorCode::LimitExceeded, "schematic: volume exceeds the limit");
    const auto ids = root.find("Blocks").as_byte_array();
    const auto data = root.find("Data").as_byte_array();
    if (!ids || !data) return bad("missing Blocks/Data");
    const u64 volume = extent.volume();
    if (ids->size() != volume || data->size() != volume) return bad("Blocks/Data length differs from Width*Height*Length");
    const auto add = root.find("AddBlocks").as_byte_array();
    if (add && add->size() < (volume + 1) / 2) return bad("AddBlocks too short");

    BRIDGER_TRY(out.structure, Structure::create(blocks, std::move(biomes), extent, {Edition::Java, 0}));
    Structure& s = out.structure;
    if (const auto ox = root.find("WEOffsetX").as_int(), oy = root.find("WEOffsetY").as_int(), oz = root.find("WEOffsetZ").as_int();
        ox && oy && oz)
        s.offset = {*ox, *oy, *oz};

    // (id, data) -> state, resolved once per pair.
    std::unordered_map<u32, StateId> cache;
    u64 approximated = 0, unknown = 0;
    auto resolve = [&](u16 id, u8 dv) -> StateId {
        const u32 key = (u32{id} << 4) | dv;
        if (const auto it = cache.find(key); it != cache.end()) return it->second;
        std::optional<std::string> text = tables.block(id, dv);
        if (!text && (dv & 8)) { text = tables.block(id, static_cast<u8>(dv & 7)); approximated += text ? 1u : 0u; }
        if (!text && dv != 0) { text = tables.block(id, 0); approximated += text ? 1u : 0u; }
        StateId sid;
        if (text) {
            auto parsed = BlockState::parse(*text);
            sid = parsed ? blocks->intern(*parsed) : blocks->intern(BlockState("bridger:legacy_" + std::to_string(id) + "_" + std::to_string(dv), {}));
        } else {
            ++unknown;
            sid = blocks->intern(BlockState("bridger:legacy_" + std::to_string(id) + "_" + std::to_string(dv), {}));
        }
        cache.emplace(key, sid);
        return sid;
    };

    // Index order: (y * Length + z) * Width + x.
    for (u32 y = 0; y < extent.y; ++y)
        for (u32 z = 0; z < extent.z; ++z)
            for (u32 x = 0; x < extent.x; ++x) {
                const usize i = (static_cast<usize>(y) * extent.z + z) * extent.x + x;
                u16 id = (*ids)[i];
                if (add) {
                    const u8 b = (*add)[i >> 1];
                    id |= static_cast<u16>(((i & 1) == 0 ? (b >> 4) & 0xF : b & 0xF) << 8); // even index: high nibble
                }
                s.set_block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)},
                            resolve(id, static_cast<u8>((*data)[i] & 0xF)));
            }
    if (approximated > 0)
        out.warnings.push_back(std::to_string(approximated) + " legacy id/data pair(s) were not in the table and used the "
                                                              "nearest data value");
    if (unknown > 0)
        out.warnings.push_back(std::to_string(unknown) + " legacy block id(s) are unknown; they are reported as unmapped");

    // Block entities, rewritten into the modern Java form the translator expects; then the states
    // pre-1.13 kept in them or split across two blocks (legacy_blocks.hpp).
    for (const NbtView entry : root.find("TileEntities").items()) {
        const NbtCompoundView c = entry.entries();
        const auto p = detail::xyz(c);
        if (!p) { out.warnings.push_back("a block entity without id/x/y/z was not imported"); continue; }
        const BlockPos pos{(*p)[0], (*p)[1], (*p)[2]};
        if (!s.contains(pos)) { out.warnings.push_back("a block entity outside the schematic was not imported"); continue; }
        BRIDGER_TRY(std::optional<BlockEntity> be, detail::legacy_block_entity(c, pos, tables, out.warnings));
        if (be) s.block_entities.push_back(std::move(*be));
    }
    detail::fix_legacy_blocks(s, out.warnings);

    for (const NbtView entry : root.find("Entities").items()) {
        const NbtCompoundView c = entry.entries();
        const NbtView pos = c.find("Pos", TagType::List);
        const auto id = c.find("id").as_string();
        if (!pos || pos.items().size() != 3 || pos.items().element_type() != TagType::Double || !id) {
            out.warnings.push_back("an entity without a valid id/Pos was not imported");
            continue;
        }
        Entity e;
        e.pos = {*pos.items().at(0).as_double(), *pos.items().at(1).as_double(), *pos.items().at(2).as_double()};
        BRIDGER_TRY(e.id, detail::utf8(*id));
        s.entities.push_back(std::move(e));
    }
    return out;
}

} // namespace bridger::formats::java

// Java structure (.nbt) reader. Format and sources: docs/formats/structure-nbt.md.

#include "nbt_helpers.hpp"

#include <bridger/formats/java/structure_nbt.hpp>

#include <string>

namespace bridger::formats::java {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::TagType;

Result<StructureFile> read_structure_nbt(std::span<const u8> file_bytes, std::shared_ptr<BlockStateRegistry> blocks,
                                         std::shared_ptr<BiomeRegistry> biomes, const StructureReadOptions& opt) {
    auto bad = [](const std::string& why) { return make_error(ErrorCode::Malformed, "structure nbt: " + why); };
    BRIDGER_TRY(const detail::JavaNbt nbt, detail::parse_java_nbt(file_bytes, opt.decompress, opt.nbt, "structure nbt"));
    const NbtCompoundView root = nbt.doc.root().entries();

    StructureFile out{FormatId::StructureNbt, std::nullopt, {}, {}};
    if (const auto dv = root.find("DataVersion").as_int()) out.data_version = *dv;

    const auto size = detail::int3(root.find("size"));
    if (!size || (*size)[0] < 0 || (*size)[1] < 0 || (*size)[2] < 0) return bad("missing or invalid 'size'");
    const Extent extent{static_cast<u32>((*size)[0]), static_cast<u32>((*size)[1]), static_cast<u32>((*size)[2])};
    if (extent.volume() > opt.max_volume) return make_error(ErrorCode::LimitExceeded, "structure nbt: volume exceeds the limit");

    // A single "palette", or "palettes" (random variants, e.g. shipwrecks): the first is used.
    NbtView palette_view = root.find("palette", TagType::List);
    if (!palette_view) {
        const NbtView palettes = root.find("palettes", TagType::List);
        if (!palettes || palettes.items().empty()) return bad("no 'palette' or 'palettes'");
        palette_view = palettes.items().at(0);
        if (palettes.items().size() > 1)
            out.warnings.push_back("structure has " + std::to_string(palettes.items().size()) +
                                   " palette variants; the first one was used");
    }
    std::vector<StateId> palette;
    for (const NbtView entry : palette_view.items()) {
        if (palette.size() >= opt.max_palette_size) return make_error(ErrorCode::LimitExceeded, "structure nbt: palette too large");
        BRIDGER_TRY(const BlockState state, detail::state_from_compound(entry.entries()));
        palette.push_back(blocks->intern(state));
    }

    const SourceVersion source{Edition::Java, static_cast<u32>(out.data_version.value_or(0))};
    BRIDGER_TRY(out.structure, Structure::create(std::move(blocks), std::move(biomes), extent, source));
    Structure& s = out.structure;

    // Blocks are listed individually; unlisted positions are structure void (kept as air).
    for (const NbtView entry : root.find("blocks").items()) {
        const NbtCompoundView b = entry.entries();
        const auto state = b.find("state").as_int();
        const auto pos = detail::int3(b.find("pos"));
        if (!state || !pos) return bad("block without 'state' or 'pos'");
        if (*state < 0 || static_cast<usize>(*state) >= palette.size()) return bad("block state index outside the palette");
        const BlockPos p{(*pos)[0], (*pos)[1], (*pos)[2]};
        if (!s.contains(p)) {
            out.warnings.push_back("block at " + std::to_string(p.x) + "," + std::to_string(p.y) + "," + std::to_string(p.z) +
                                   " is outside the structure size and was not imported");
            continue;
        }
        s.set_block(p, palette[static_cast<usize>(*state)]);
        if (const NbtView data = b.find("nbt", TagType::Compound)) {
            const NbtCompoundView c = data.entries();
            const auto id = c.find("id").as_string();
            if (!id) {
                out.warnings.push_back("block entity without an id at " + std::to_string(p.x) + "," + std::to_string(p.y) +
                                       "," + std::to_string(p.z) + " was not imported");
                continue;
            }
            BlockEntity be;
            be.pos = p;
            BRIDGER_TRY(be.id, detail::utf8(*id));
            BRIDGER_TRY(be.data, detail::encode_compound(c, {"id", "x", "y", "z"}));
            s.block_entities.push_back(std::move(be));
        }
    }

    for (const NbtView entry : root.find("entities").items()) {
        const NbtCompoundView e = entry.entries();
        const NbtView pos = e.find("pos", TagType::List);
        const NbtCompoundView data = e.find("nbt").entries();
        const auto id = data.find("id").as_string();
        if (!pos || pos.items().size() != 3 || pos.items().element_type() != TagType::Double || !id)
            return bad("entity needs 'pos' (3 doubles) and 'nbt.id'");
        Entity ent;
        ent.pos = {*pos.items().at(0).as_double(), *pos.items().at(1).as_double(), *pos.items().at(2).as_double()};
        BRIDGER_TRY(ent.id, detail::utf8(*id));
        BRIDGER_TRY(ent.data, detail::encode_compound(data, {"id", "Pos", "UUID"}));
        s.entities.push_back(std::move(ent));
    }
    return out;
}

} // namespace bridger::formats::java

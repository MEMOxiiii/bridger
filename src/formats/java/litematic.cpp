// Litematica reader. Format and sources: docs/formats/litematic.md (behavior of
// Litematica's LitematicaSchematic / LitematicaBlockStateContainer / LitematicaBitArray).

#include "nbt_helpers.hpp"

#include <bridger/formats/java/litematic.hpp>
#include <bridger/io/endian.hpp>

#include <algorithm>
#include <bit>
#include <string>

namespace bridger::formats::java {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::TagType;

namespace {

struct Region {
    std::string name;
    std::array<i32, 3> min{};
    std::array<u32, 3> size{};
    NbtCompoundView tag{nullptr, Endian::Big};
};

} // namespace

Result<StructureFile> read_litematic(std::span<const u8> file_bytes, std::shared_ptr<BlockStateRegistry> blocks,
                                     std::shared_ptr<BiomeRegistry> biomes, const StructureReadOptions& opt) {
    auto bad = [](const std::string& why) { return make_error(ErrorCode::Malformed, "litematic: " + why); };
    BRIDGER_TRY(const detail::JavaNbt nbt, detail::parse_java_nbt(file_bytes, opt.decompress, opt.nbt, "litematic"));
    const NbtCompoundView root = nbt.doc.root().entries();

    StructureFile out{FormatId::Litematic, std::nullopt, {}, {}};
    const auto version = root.find("Version").as_int();
    if (!version) return bad("missing 'Version'");
    // Version 1 stored block entities differently; versions 2..7 share the layout read here.
    if (*version < 2 || *version > 7)
        return make_error(ErrorCode::UnsupportedVersion, "litematic: unsupported format version " + std::to_string(*version));
    if (const auto dv = root.find("MinecraftDataVersion").as_int()) out.data_version = *dv;

    // Regions: Position + Size (either may be negative); min corner = min(pos, pos + size -/+ 1).
    std::vector<Region> regions;
    for (const io::nbt::NbtEntry& e : root.find("Regions").entries()) {
        const NbtCompoundView r = e.value.entries();
        const auto pos = detail::xyz(r.find("Position").entries());
        const auto size = detail::xyz(r.find("Size").entries());
        if (!pos || !size) return bad("region without Position/Size");
        Region region;
        BRIDGER_TRY(region.name, detail::utf8(e.name));
        for (int a = 0; a < 3; ++a) {
            const i64 s = (*size)[a];
            if (s == 0) return bad("region has a zero size");
            const i64 end = i64{(*pos)[a]} + (s > 0 ? s - 1 : s + 1);
            region.min[a] = static_cast<i32>(std::min<i64>((*pos)[a], end));
            region.size[a] = static_cast<u32>(s > 0 ? s : -s);
        }
        region.tag = r;
        regions.push_back(std::move(region));
    }
    if (regions.empty()) return bad("no regions");

    std::array<i64, 3> lo{INT64_MAX, INT64_MAX, INT64_MAX}, hi{INT64_MIN, INT64_MIN, INT64_MIN};
    for (const Region& r : regions)
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min<i64>(lo[a], r.min[a]);
            hi[a] = std::max<i64>(hi[a], i64{r.min[a]} + r.size[a] - 1);
        }
    const Extent extent{static_cast<u32>(hi[0] - lo[0] + 1), static_cast<u32>(hi[1] - lo[1] + 1),
                        static_cast<u32>(hi[2] - lo[2] + 1)};
    if (extent.volume() > opt.max_volume) return make_error(ErrorCode::LimitExceeded, "litematic: volume exceeds the limit");
    if (regions.size() > 1)
        out.warnings.push_back(std::to_string(regions.size()) + " regions were merged into one structure");

    const SourceVersion source{Edition::Java, static_cast<u32>(out.data_version.value_or(0))};
    BRIDGER_TRY(out.structure, Structure::create(blocks, std::move(biomes), extent, source));
    Structure& s = out.structure;
    if (const NbtView meta = root.find("Metadata", TagType::Compound)) {
        const NbtCompoundView m = meta.entries();
        if (const auto n = m.find("Name").as_string()) { BRIDGER_TRY(s.metadata.name, detail::utf8(*n)); }
        if (const auto a = m.find("Author").as_string()) { BRIDGER_TRY(s.metadata.author, detail::utf8(*a)); }
        if (const auto t = m.find("TimeCreated").as_long()) s.metadata.date_ms = *t;
        BRIDGER_TRY(s.metadata.original, detail::encode_compound(m));
    }

    for (const Region& r : regions) {
        const std::array<i32, 3> off{static_cast<i32>(r.min[0] - lo[0]), static_cast<i32>(r.min[1] - lo[1]),
                                     static_cast<i32>(r.min[2] - lo[2])};
        std::vector<StateId> palette;
        for (const NbtView entry : r.tag.find("BlockStatePalette").items()) {
            if (palette.size() >= opt.max_palette_size) return make_error(ErrorCode::LimitExceeded, "litematic: palette too large");
            BRIDGER_TRY(const BlockState state, detail::state_from_compound(entry.entries()));
            palette.push_back(blocks->intern(state));
        }
        if (palette.empty()) return bad("region '" + r.name + "' has an empty palette");
        const auto data = r.tag.find("BlockStates").as_long_array();
        if (!data) return bad("region '" + r.name + "' has no BlockStates");

        // bits = max(2, bits needed for palette.size() - 1); entries may span two longs, LSB first.
        const u32 bits = std::max<u32>(2, static_cast<u32>(std::bit_width(palette.size() - 1)));
        const u64 volume = u64{r.size[0]} * r.size[1] * r.size[2];
        if (static_cast<u64>(data->size()) * 64 < volume * bits) return bad("region '" + r.name + "' BlockStates too short");
        const u64 mask = (u64{1} << bits) - 1;
        auto at = [&](u64 index) -> u64 {
            const u64 start = index * bits;
            const usize word = static_cast<usize>(start >> 6);
            const unsigned shift = static_cast<unsigned>(start & 63);
            u64 v = static_cast<u64>((*data)[word]) >> shift;
            if (shift + bits > 64) v |= static_cast<u64>((*data)[word + 1]) << (64 - shift);
            return v & mask;
        };
        const u32 sx = r.size[0], sy = r.size[1], sz = r.size[2];
        for (u32 y = 0; y < sy; ++y)
            for (u32 z = 0; z < sz; ++z)
                for (u32 x = 0; x < sx; ++x) {
                    const u64 idx = at((u64{y} * sz + z) * sx + x);
                    if (idx >= palette.size()) return bad("region '" + r.name + "' references a palette index out of range");
                    s.set_block({off[0] + static_cast<i32>(x), off[1] + static_cast<i32>(y), off[2] + static_cast<i32>(z)},
                                palette[static_cast<usize>(idx)]);
                }

        // Block entities: x/y/z relative to the region's min corner.
        for (const NbtView entry : r.tag.find("TileEntities").items()) {
            const NbtCompoundView c = entry.entries();
            const auto p = detail::xyz(c);
            const auto id = c.find("id").as_string();
            if (!p) return bad("block entity without x/y/z");
            BlockEntity be;
            be.pos = {off[0] + (*p)[0], off[1] + (*p)[1], off[2] + (*p)[2]};
            if (!s.contains(be.pos)) {
                out.warnings.push_back("block entity outside its region was not imported");
                continue;
            }
            if (id) {
                BRIDGER_TRY(be.id, detail::utf8(*id));
            } else {
                // Litematica omits the id for some versions; it is implied by the block.
                be.id = std::string(s.block_registry().get(s.block(be.pos)).id());
            }
            BRIDGER_TRY(be.data, detail::encode_compound(c, {"id", "x", "y", "z"}));
            s.block_entities.push_back(std::move(be));
        }
        for (const NbtView entry : r.tag.find("Entities").items()) {
            const NbtCompoundView c = entry.entries();
            const NbtView pos = c.find("Pos", TagType::List);
            const auto id = c.find("id").as_string();
            if (!pos || pos.items().size() != 3 || pos.items().element_type() != TagType::Double || !id) {
                out.warnings.push_back("an entity without a valid id/Pos in region '" + r.name + "' was not imported");
                continue;
            }
            Entity e;
            e.pos = {off[0] + *pos.items().at(0).as_double(), off[1] + *pos.items().at(1).as_double(),
                     off[2] + *pos.items().at(2).as_double()};
            BRIDGER_TRY(e.id, detail::utf8(*id));
            BRIDGER_TRY(e.data, detail::encode_compound(c, {"id", "Pos", "UUID"}));
            s.entities.push_back(std::move(e));
        }
    }
    return out;
}

} // namespace bridger::formats::java

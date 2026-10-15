// Sponge schematic reader. Every field and rule below is from
// docs/formats/sponge-schem.md, which cites the specification and WorldEdit's
// observed behavior.

#include "java_upgrade.hpp"

#include <bridger/formats/java/sponge_schem.hpp>
#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>

namespace bridger::formats::java {
namespace {

using io::nbt::NbtCompoundView;
using io::nbt::NbtDocument;
using io::nbt::NbtView;
using io::nbt::TagType;

constexpr u32 kInvalidId = std::numeric_limits<u32>::max();

tl::unexpected<Error> malformed(std::string message) {
    return make_error(ErrorCode::Malformed, "schem: " + std::move(message));
}

Result<std::string> utf8(std::string_view raw) { return io::nbt::mutf8_to_utf8(raw); }

Result<u16> required_dimension(NbtCompoundView c, std::string_view name) {
    const NbtView v = c.find(name, TagType::Short);
    if (!v) return malformed("missing required Short '" + std::string(name) + "'");
    return static_cast<u16>(*v.as_short()); // "unsigned short ... read with value & 0xFFFF"
}

Result<std::optional<std::array<i32, 3>>> int3(NbtCompoundView c, std::string_view name) {
    const NbtView v = c.find(name);
    if (!v) return std::optional<std::array<i32, 3>>{};
    const auto arr = v.as_int_array();
    if (!arr || arr->size() != 3) return malformed("'" + std::string(name) + "' must be an Int array of 3 values");
    return std::optional<std::array<i32, 3>>{std::array<i32, 3>{(*arr)[0], (*arr)[1], (*arr)[2]}};
}

// Encodes the entries of `c` (minus `excluded` keys) as an unnamed big-endian root compound.
Result<RawNbt> encode_compound(NbtCompoundView c, std::initializer_list<std::string_view> excluded = {}) {
    io::nbt::NbtWriter w(Endian::Big);
    w.begin_root_compound();
    bool any = false;
    for (const io::nbt::NbtEntry& e : c) {
        if (std::find(excluded.begin(), excluded.end(), e.name) != excluded.end()) continue;
        w.write_view(e.name, e.value);
        any = true;
    }
    w.end_compound();
    if (!any) return RawNbt{};
    BRIDGER_TRY(std::vector<u8> bytes, std::move(w).finish());
    return RawNbt{std::move(bytes), Endian::Big};
}

// v1/v2 store a block entity's fields next to Pos/Id ("flattened", as WorldEdit
// writes them); the v2 text describes an "Extra" compound instead. Accept both: if
// the only remaining entry is an "Extra" compound, its contents are the data.
Result<RawNbt> flattened_data(NbtCompoundView c, std::initializer_list<std::string_view> excluded) {
    usize remaining = 0;
    NbtView extra;
    for (const io::nbt::NbtEntry& e : c) {
        if (std::find(excluded.begin(), excluded.end(), e.name) != excluded.end()) continue;
        ++remaining;
        if (e.name == "Extra" && e.value.is(TagType::Compound)) extra = e.value;
    }
    if (remaining == 1 && extra) return encode_compound(*extra.as_compound());
    return encode_compound(c, excluded);
}

// Palette compound (name -> Int index) to a dense index -> id table.
template <typename InternFn>
Result<std::vector<u32>> read_palette(NbtCompoundView palette, const SchemReadOptions& opt, InternFn intern,
                                      std::string_view what) {
    std::vector<u32> table;
    for (const io::nbt::NbtEntry& e : palette) {
        const auto index = e.value.as_int();
        if (!index) return malformed(std::string(what) + " palette value is not an Int");
        if (*index < 0) return malformed(std::string(what) + " palette index is negative");
        if (static_cast<u32>(*index) >= opt.max_palette_size)
            return make_error(ErrorCode::LimitExceeded, "schem: " + std::string(what) + " palette index too large");
        const auto i = static_cast<usize>(*index);
        if (i >= table.size()) table.resize(i + 1, kInvalidId);
        if (table[i] != kInvalidId) return malformed(std::string(what) + " palette index used twice");
        BRIDGER_TRY(const std::string name, utf8(e.name));
        BRIDGER_TRY(table[i], intern(name));
    }
    return table;
}

// Decodes W*H*L varint palette indices (x fastest, then z, then y) into one layer
// of every section. Works slab by slab (16 rows of y) so memory stays at W*L*16.
template <typename LayerFn>
Status decode_volume(std::span<const u8> data, const std::vector<u32>& palette, Structure& s, u32 fill,
                     LayerFn layer_of, std::string_view what) {
    const Extent size = s.size();
    const u32 sx = s.sections_x();
    const u32 sz = s.sections_z();
    std::vector<u32> slab(static_cast<usize>(sx) * sz * kSectionVolume);
    const u8* p = data.data();
    const u8* const end = p + data.size();

    for (u32 sy = 0; sy < s.sections_y(); ++sy) {
        std::fill(slab.begin(), slab.end(), fill);
        const u32 y_end = std::min<u32>(16, size.y - sy * 16);
        for (u32 yy = 0; yy < y_end; ++yy) {
            for (u32 z = 0; z < size.z; ++z) {
                u32* row = slab.data() + (static_cast<usize>(z >> 4) * sx) * kSectionVolume;
                for (u32 x = 0; x < size.x; ++x) {
                    // Inline varint (LEB128, max 5 bytes for a 32-bit index).
                    u32 value = 0;
                    unsigned shift = 0;
                    for (;;) {
                        if (p == end) return malformed(std::string(what) + " data is shorter than Width*Height*Length");
                        const u8 b = *p++;
                        if (shift == 28 && (b & 0xF0) != 0) return malformed(std::string(what) + " data has an invalid varint");
                        value |= static_cast<u32>(b & 0x7F) << shift;
                        if ((b & 0x80) == 0) break;
                        shift += 7;
                    }
                    if (value >= palette.size() || palette[value] == kInvalidId)
                        return malformed(std::string(what) + " data references palette index " + std::to_string(value) +
                                         ", which is not in the palette");
                    row[(x >> 4) * kSectionVolume + section_index(static_cast<i32>(x & 15), static_cast<i32>(yy),
                                                                  static_cast<i32>(z & 15))] = palette[value];
                }
            }
        }
        for (u32 cz = 0; cz < sz; ++cz)
            for (u32 cx = 0; cx < sx; ++cx)
                layer_of(s.section(cx, sy, cz))
                    .assign(std::span<const u32, kSectionVolume>(
                        slab.data() + (static_cast<usize>(cz) * sx + cx) * kSectionVolume, kSectionVolume));
    }
    if (p != end) return malformed(std::string(what) + " data is longer than Width*Height*Length");
    return {};
}

} // namespace

Result<SchemFile> read_sponge_schem(std::span<const u8> file_bytes, std::shared_ptr<BlockStateRegistry> block_registry,
                                    std::shared_ptr<BiomeRegistry> biome_registry, const SchemReadOptions& opt) {
    SchemInfo info;

    // --- Container: gzip (per spec); raw NBT is accepted too.
    info.compression = io::detect_compression(file_bytes);
    std::vector<u8> decompressed;
    std::span<const u8> nbt_bytes = file_bytes;
    if (info.compression == io::Compression::Gzip) {
        BRIDGER_TRY(decompressed, io::decompress(file_bytes, io::Compression::Gzip, opt.decompress));
        nbt_bytes = decompressed;
    } else if (info.compression != io::Compression::None) {
        return make_error(ErrorCode::UnsupportedFormat, "schem: expected gzip-compressed NBT, found " +
                                                            std::string(io::to_string(info.compression)));
    }

    BRIDGER_TRY(const NbtDocument doc, NbtDocument::parse(nbt_bytes, Endian::Big, opt.nbt));
    const auto root = doc.root().as_compound();
    if (!root) return malformed("root tag is not a compound");
    // v3 nests everything in a "Schematic" compound inside an unnamed root.
    NbtCompoundView top = *root;
    if (const NbtView wrapped = root->find("Schematic", TagType::Compound)) top = *wrapped.as_compound();

    // --- Header
    const NbtView version = top.find("Version", TagType::Int);
    if (!version) return malformed("missing required Int 'Version'");
    info.version = *version.as_int();
    if (info.version < 1 || info.version > 3)
        return make_error(ErrorCode::UnsupportedVersion,
                          "schem: unsupported Sponge schematic version " + std::to_string(info.version));
    if (const NbtView dv = top.find("DataVersion", TagType::Int)) info.data_version = *dv.as_int();
    else if (info.version >= 2) info.warnings.emplace_back("required field DataVersion is missing");

    BRIDGER_TRY(info.width, required_dimension(top, "Width"));
    BRIDGER_TRY(info.height, required_dimension(top, "Height"));
    BRIDGER_TRY(info.length, required_dimension(top, "Length"));
    const Extent size{info.width, info.height, info.length};
    if (size.volume() > opt.max_volume)
        return make_error(ErrorCode::LimitExceeded, "schem: volume of " + std::to_string(size.volume()) +
                                                        " blocks exceeds the limit of " + std::to_string(opt.max_volume));
    BRIDGER_TRY(const auto offset, int3(top, "Offset"));
    if (offset) info.offset = *offset;

    const SourceVersion source{Edition::Java, static_cast<u32>(info.data_version.value_or(0))};
    BRIDGER_TRY(Structure structure, Structure::create(std::move(block_registry), std::move(biome_registry), size, source));
    structure.offset = {info.offset[0], info.offset[1], info.offset[2]};

    // --- Metadata (all optional)
    if (const NbtView meta = top.find("Metadata", TagType::Compound)) {
        const NbtCompoundView m = *meta.as_compound();
        BRIDGER_TRY(structure.metadata.original, encode_compound(m));
        if (const NbtView v = m.find("Name", TagType::String)) { BRIDGER_TRY(structure.metadata.name, utf8(*v.as_string())); }
        if (const NbtView v = m.find("Author", TagType::String)) { BRIDGER_TRY(structure.metadata.author, utf8(*v.as_string())); }
        if (const NbtView v = m.find("Date", TagType::Long)) structure.metadata.date_ms = *v.as_long();
        if (const NbtView v = m.find("RequiredMods", TagType::List)) {
            for (const NbtView mod : v.items()) {
                if (const auto s = mod.as_string()) {
                    BRIDGER_TRY(std::string name, utf8(*s));
                    structure.metadata.required_mods.push_back(std::move(name));
                }
            }
        }
        const NbtView wx = m.find("WEOffsetX", TagType::Int);
        const NbtView wy = m.find("WEOffsetY", TagType::Int);
        const NbtView wz = m.find("WEOffsetZ", TagType::Int);
        if (wx && wy && wz) info.worldedit_offset = std::array<i32, 3>{*wx.as_int(), *wy.as_int(), *wz.as_int()};
        if (const NbtView we = m.find("WorldEdit", TagType::Compound)) {
            BRIDGER_TRY(const auto origin, int3(*we.as_compound(), "Origin"));
            info.worldedit_origin = origin;
        }
    }

    // --- Blocks: v3 groups them in a "Blocks" container.
    NbtCompoundView blocks = top;
    std::string_view data_key = "BlockData";
    if (info.version == 3) {
        const NbtView b = top.find("Blocks", TagType::Compound);
        if (!b) return malformed("missing 'Blocks' container");
        blocks = *b.as_compound();
        data_key = "Data";
    }
    const NbtView palette_view = blocks.find("Palette", TagType::Compound);
    if (!palette_view)
        return make_error(ErrorCode::UnsupportedFormat,
                          "schem: no block palette (files relying on global numeric ids are not supported)");
    BlockStateRegistry& registry = structure.block_registry();
    BRIDGER_TRY(const std::vector<u32> palette,
                read_palette(*palette_view.as_compound(), opt,
                             [&registry, dv = info.data_version.value_or(0)](const std::string& name) -> Result<u32> {
                                 BRIDGER_TRY(BlockState state, BlockState::parse(name));
                                 return registry.intern(detail::upgraded(std::move(state), dv)); // older DataVersions
                             },
                             "block"));
    info.palette_size = static_cast<usize>(std::count_if(palette.begin(), palette.end(), [](u32 v) { return v != kInvalidId; }));
    if (info.version < 3) {
        if (const NbtView pm = top.find("PaletteMax", TagType::Int); pm && *pm.as_int() != static_cast<i32>(info.palette_size))
            info.warnings.push_back("PaletteMax is " + std::to_string(*pm.as_int()) + " but the palette has " +
                                    std::to_string(info.palette_size) + " entries");
    }
    const NbtView block_data = blocks.find(data_key, TagType::ByteArray);
    if (!block_data) return malformed("missing block data '" + std::string(data_key) + "'");
    BRIDGER_TRY_STATUS(decode_volume(*block_data.as_byte_array(), palette, structure, kAirStateId,
                                     [](Section& s) -> PaletteLayer& { return s.blocks(); }, "block"));

    // --- Block entities: "TileEntities" (v1), "BlockEntities" (v2, v3 inside Blocks).
    NbtView be_list = blocks.find("BlockEntities", TagType::List);
    if (!be_list && info.version == 1) be_list = top.find("TileEntities", TagType::List);
    if (be_list) {
        for (const NbtView item : be_list.items()) {
            const auto c = item.as_compound();
            if (!c) return malformed("block entity is not a compound");
            BRIDGER_TRY(const auto pos, int3(*c, "Pos"));
            if (!pos) return malformed("block entity without 'Pos'");
            const NbtView id = c->find("Id", TagType::String);
            if (!id) return malformed("block entity without 'Id'");
            BlockEntity be;
            be.pos = {(*pos)[0], (*pos)[1], (*pos)[2]};
            BRIDGER_TRY(be.id, utf8(*id.as_string()));
            if (info.version == 3) {
                if (const NbtView data = c->find("Data", TagType::Compound)) { BRIDGER_TRY(be.data, encode_compound(*data.as_compound())); }
            } else {
                BRIDGER_TRY(be.data, flattened_data(*c, {"Pos", "Id", "ContentVersion"}));
            }
            if (!structure.contains(be.pos)) {
                info.warnings.push_back("block entity " + be.id + " at " + std::to_string(be.pos.x) + "," +
                                        std::to_string(be.pos.y) + "," + std::to_string(be.pos.z) +
                                        " is outside the schematic and was not imported");
                continue;
            }
            structure.block_entities.push_back(std::move(be));
        }
    }
    info.block_entity_count = structure.block_entities.size();

    // --- Entities (v2, v3)
    if (const NbtView list = top.find("Entities", TagType::List); list && info.version >= 2) {
        for (const NbtView item : list.items()) {
            const auto c = item.as_compound();
            if (!c) return malformed("entity is not a compound");
            const NbtView pos = c->find("Pos", TagType::List);
            const auto pos_list = pos ? pos.as_list() : std::nullopt;
            if (!pos_list || pos_list->element_type() != TagType::Double || pos_list->size() != 3)
                return malformed("entity 'Pos' must be a list of 3 doubles");
            const NbtView id = c->find("Id", TagType::String);
            if (!id) return malformed("entity without 'Id'");
            Entity e;
            e.pos = {*pos_list->at(0).as_double(), *pos_list->at(1).as_double(), *pos_list->at(2).as_double()};
            BRIDGER_TRY(e.id, utf8(*id.as_string()));
            if (info.version == 3) {
                if (const NbtView data = c->find("Data", TagType::Compound)) { BRIDGER_TRY(e.data, encode_compound(*data.as_compound())); }
            } else {
                BRIDGER_TRY(e.data, flattened_data(*c, {"Pos", "Id"}));
            }
            structure.entities.push_back(std::move(e));
        }
    }
    info.entity_count = structure.entities.size();

    // --- Biomes: v3 "Biomes" container (per block); v2 BiomePalette/BiomeData (per column).
    BiomeRegistry& biome_names = structure.biome_registry();
    auto intern_biome = [&biome_names](const std::string& name) -> Result<u32> { return biome_names.intern(name); };
    if (info.version == 3) {
        if (const NbtView b = top.find("Biomes", TagType::Compound)) {
            const NbtCompoundView biomes = *b.as_compound();
            const NbtView pal = biomes.find("Palette", TagType::Compound);
            const NbtView data = biomes.find("Data", TagType::ByteArray);
            if (!pal || !data) return malformed("'Biomes' needs 'Palette' and 'Data'");
            BRIDGER_TRY(const std::vector<u32> bpal, read_palette(*pal.as_compound(), opt, intern_biome, "biome"));
            info.biome_palette_size = bpal.size();
            info.biomes_3d = true;
            const u32 fill = bpal.empty() ? 0 : bpal.front();
            BRIDGER_TRY_STATUS(decode_volume(*data.as_byte_array(), bpal, structure, fill,
                                             [fill](Section& s) -> PaletteLayer& { return s.ensure_biomes(fill); }, "biome"));
        }
    } else if (const NbtView data = top.find("BiomeData", TagType::ByteArray)) {
        const NbtView pal = top.find("BiomePalette", TagType::Compound);
        if (!pal) return malformed("'BiomeData' without 'BiomePalette'");
        BRIDGER_TRY(const std::vector<u32> bpal, read_palette(*pal.as_compound(), opt, intern_biome, "biome"));
        info.biome_palette_size = bpal.size();
        if (const NbtView pm = top.find("BiomePaletteMax", TagType::Int); pm && *pm.as_int() != static_cast<i32>(bpal.size()))
            info.warnings.push_back("BiomePaletteMax does not match the biome palette size");
        // Column biomes (x + z*Width) applied to every y: decode into a 1-high
        // temporary volume, then copy each column up through the sections.
        BRIDGER_TRY(Structure columns, Structure::create(std::make_shared<BlockStateRegistry>(), structure.biome_registry_ptr(),
                                                         Extent{size.x, size.y == 0 ? 0u : 1u, size.z}, source));
        const u32 fill = bpal.empty() ? 0 : bpal.front();
        BRIDGER_TRY_STATUS(decode_volume(*data.as_byte_array(), bpal, columns, fill,
                                         [fill](Section& s) -> PaletteLayer& { return s.ensure_biomes(fill); }, "biome"));
        for (u32 cz = 0; cz < structure.sections_z(); ++cz) {
            for (u32 cx = 0; cx < structure.sections_x(); ++cx) {
                std::array<u32, kSectionVolume> values; // NOLINT(cppcoreguidelines-pro-type-member-init)
                if (size.y != 0) columns.section(cx, 0, cz).biome_layer()->copy_to(values);
                for (usize yy = 1; yy < 16; ++yy)
                    std::copy_n(values.begin(), 256, values.begin() + static_cast<std::ptrdiff_t>(yy * 256));
                for (u32 cy = 0; cy < structure.sections_y(); ++cy) structure.section(cx, cy, cz).ensure_biomes(fill).assign(values);
            }
        }
    }

    return SchemFile{std::move(info), std::move(structure)};
}

} // namespace bridger::formats::java

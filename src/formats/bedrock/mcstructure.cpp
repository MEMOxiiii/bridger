// .mcstructure reader/writer. Format and sources: docs/formats/mcstructure.md.

#include <bridger/formats/bedrock/mcstructure.hpp>
#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>

namespace bridger::formats::bedrock {

using io::nbt::NbtCompoundView;
using io::nbt::NbtView;
using io::nbt::NbtWriter;
using io::nbt::TagType;

namespace {

constexpr usize mc_index(u32 x, u32 y, u32 z, const Extent& s) noexcept {
    return (static_cast<usize>(x) * s.y + y) * s.z + z;
}

} // namespace

Result<std::vector<u8>> write_mcstructure(const Structure& s, u32 air_state, const StateNbtFn& state_nbt) {
    if (s.source().edition != Edition::Bedrock) return make_error(ErrorCode::InvalidArgument, "write_mcstructure needs a Bedrock structure");
    const Extent size = s.size();
    const usize volume = static_cast<usize>(size.volume());
    if (volume > static_cast<usize>(INT32_MAX)) return make_error(ErrorCode::LimitExceeded, "mcstructure: too large");

    std::vector<u32> palette;
    std::unordered_map<u32, i32> slot_of;
    auto slot = [&](u32 state) {
        const auto [it, inserted] = slot_of.emplace(state, static_cast<i32>(palette.size()));
        if (inserted) palette.push_back(state);
        return it->second;
    };
    std::vector<i32> layer0(volume), layer1(volume, -1);
    for (u32 x = 0; x < size.x; ++x)
        for (u32 y = 0; y < size.y; ++y)
            for (u32 z = 0; z < size.z; ++z) {
                const BlockPos p{static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)};
                const usize i = mc_index(x, y, z, size);
                layer0[i] = slot(s.block(p));
                if (s.has_secondary(p))
                    if (const u32 l = s.secondary(p); l != air_state) layer1[i] = slot(l);
            }

    NbtWriter w(Endian::Little);
    w.begin_root_compound("");
    w.write_int("format_version", 1);
    w.begin_list("size", TagType::Int, 3);
    w.write_int({}, static_cast<i32>(size.x));
    w.write_int({}, static_cast<i32>(size.y));
    w.write_int({}, static_cast<i32>(size.z));
    w.end_list();
    w.begin_compound("structure");
    w.begin_list("block_indices", TagType::List, 2);
    for (const auto* layer : {&layer0, &layer1}) {
        w.begin_list({}, TagType::Int, layer->size());
        for (const i32 v : *layer) w.write_int({}, v);
        w.end_list();
    }
    w.end_list();
    w.begin_list("entities", TagType::End, 0); // entities are reported by translation, not written yet
    w.end_list();
    w.begin_compound("palette");
    w.begin_compound("default");
    w.begin_list("block_palette", TagType::Compound, palette.size());
    for (const u32 state : palette) {
        BRIDGER_TRY(const std::span<const u8> nbt, state_nbt(state));
        BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(nbt, Endian::Little));
        w.write_view({}, doc.root());
    }
    w.end_list();
    w.begin_compound("block_position_data");
    for (const BlockEntity& be : s.block_entities) {
        if (!s.contains(be.pos)) continue;
        BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian));
        const usize i = mc_index(static_cast<u32>(be.pos.x), static_cast<u32>(be.pos.y), static_cast<u32>(be.pos.z), size);
        w.begin_compound(std::to_string(i));
        w.begin_compound("block_entity_data");
        for (const io::nbt::NbtEntry& e : doc.root().entries()) {
            if (e.name == "x" || e.name == "y" || e.name == "z" || e.name == "isMovable") continue;
            w.write_view(e.name, e.value); // pairx/pairz stay structure-local: replaced on load like x/y/z
        }
        // "Position tags are saved but replaced upon loading" (Bedrock Wiki).
        w.write_int("x", be.pos.x);
        w.write_int("y", be.pos.y);
        w.write_int("z", be.pos.z);
        w.write_byte("isMovable", 1);
        w.end_compound();
        w.end_compound();
    }
    w.end_compound();
    w.end_compound(); // default
    w.end_compound(); // palette
    w.end_compound(); // structure
    w.begin_list("structure_world_origin", TagType::Int, 3);
    for (int a = 0; a < 3; ++a) w.write_int({}, 0);
    w.end_list();
    w.end_compound();
    return std::move(w).finish();
}

Result<McStructureFile> read_mcstructure(std::span<const u8> bytes, std::shared_ptr<BlockStateRegistry> blocks,
                                         std::shared_ptr<BiomeRegistry> biomes, u32 bedrock_data_version,
                                         const StateLookupFn& lookup, u32 air_state, const McStructureReadOptions& opt) {
    auto bad = [](const std::string& why) { return make_error(ErrorCode::Malformed, "mcstructure: " + why); };
    BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(bytes, Endian::Little, opt.nbt));
    const NbtCompoundView root = doc.root().entries();
    McStructureFile out;
    if (const auto fv = root.find("format_version").as_int(); fv && *fv != 1)
        return make_error(ErrorCode::UnsupportedVersion, "mcstructure: format_version " + std::to_string(*fv));

    const NbtView size_view = root.find("size", TagType::List);
    if (!size_view || size_view.items().size() != 3 || size_view.items().element_type() != TagType::Int)
        return bad("'size' must be a list of 3 ints");
    std::array<i32, 3> sz{};
    for (usize a = 0; a < 3; ++a) sz[a] = *size_view.items().at(a).as_int();
    if (sz[0] < 0 || sz[1] < 0 || sz[2] < 0) return bad("negative size");
    const Extent extent{static_cast<u32>(sz[0]), static_cast<u32>(sz[1]), static_cast<u32>(sz[2])};
    if (extent.volume() > opt.max_volume) return make_error(ErrorCode::LimitExceeded, "mcstructure: volume exceeds the limit");
    if (const NbtView o = root.find("structure_world_origin", TagType::List); o && o.items().size() == 3 &&
                                                                              o.items().element_type() == TagType::Int)
        for (usize a = 0; a < 3; ++a) out.world_origin[a] = *o.items().at(a).as_int();

    const NbtCompoundView st = root.find("structure").entries();
    const NbtCompoundView def = st.find("palette").entries().find("default").entries();

    // Palette -> Bedrock state ids (unknown entries become air and are reported).
    std::vector<u32> palette;
    std::set<std::string> unknown_names;
    for (const NbtView entry : def.find("block_palette").items()) {
        const auto id = lookup(entry.entries());
        if (!id) {
            const auto name = entry.entries().find("name").as_string();
            unknown_names.insert(std::string(name.value_or("?")));
        }
        palette.push_back(id.value_or(air_state));
    }

    BRIDGER_TRY(out.structure, Structure::create(std::move(blocks), std::move(biomes), extent,
                                                 {Edition::Bedrock, bedrock_data_version}));
    Structure& s = out.structure;
    const NbtView layers = st.find("block_indices", TagType::List);
    if (!layers || layers.items().empty()) return bad("missing block_indices");
    const usize volume = static_cast<usize>(extent.volume());
    u64 voids = 0;
    usize layer_no = 0;
    for (const NbtView layer : layers.items()) {
        const auto list = layer.as_list();
        if (!list || (list->size() != volume) || (volume > 0 && list->element_type() != TagType::Int))
            return bad("block_indices layer has the wrong size or type");
        if (layer_no > 1) { out.warnings.push_back("block layers beyond the second were ignored"); break; }
        usize i = 0;
        for (u32 x = 0; x < extent.x; ++x)
            for (u32 y = 0; y < extent.y; ++y)
                for (u32 z = 0; z < extent.z; ++z, ++i) {
                    const i32 v = *list->at(i).as_int();
                    const BlockPos p{static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)};
                    if (v < 0) {
                        if (layer_no == 0) { ++voids; s.set_block(p, air_state); }
                        continue;
                    }
                    if (static_cast<usize>(v) >= palette.size()) return bad("block index outside the palette");
                    const u32 state = palette[static_cast<usize>(v)];
                    if (layer_no == 0) {
                        s.set_block(p, state);
                    } else if (state != air_state) {
                        Section& sec = s.section(x >> 4, y >> 4, z >> 4);
                        (void)sec.ensure_secondary(air_state);
                        sec.set_secondary(static_cast<i32>(x & 15), static_cast<i32>(y & 15), static_cast<i32>(z & 15), state);
                    }
                }
        ++layer_no;
    }
    if (voids > 0) out.warnings.push_back(std::to_string(voids) + " structure-void positions were read as air");
    for (const auto& name : unknown_names)
        out.warnings.push_back("Bedrock block " + name + " is not in the pinned palette and was read as air");

    for (const io::nbt::NbtEntry& e : def.find("block_position_data").entries()) {
        const std::string key(e.name);
        usize idx = 0;
        try {
            idx = static_cast<usize>(std::stoull(key));
        } catch (...) {
            return bad("block_position_data key is not an index");
        }
        if (idx >= volume) return bad("block_position_data index out of range");
        const NbtView data = e.value.entries().find("block_entity_data", TagType::Compound);
        if (!data) continue;
        BlockEntity be;
        const usize z = idx % extent.z, y = (idx / extent.z) % extent.y, x = idx / (static_cast<usize>(extent.z) * extent.y);
        be.pos = {static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)};
        be.id = std::string(data.entries().find("id").as_string().value_or(""));
        NbtWriter w(Endian::Little);
        w.begin_root_compound();
        // Positions are dropped (be.pos is structure-local); the double-chest partner is stored
        // in world coordinates like x/y/z and becomes structure-local too.
        for (const io::nbt::NbtEntry& f : data.entries()) {
            if (f.name == "x" || f.name == "y" || f.name == "z") continue;
            if (f.name == "pairx" && f.value.as_int()) w.write_int("pairx", *f.value.as_int() - out.world_origin[0]);
            else if (f.name == "pairz" && f.value.as_int()) w.write_int("pairz", *f.value.as_int() - out.world_origin[2]);
            else w.write_view(f.name, f.value);
        }
        w.end_compound();
        BRIDGER_TRY(std::vector<u8> encoded, std::move(w).finish());
        be.data = RawNbt{std::move(encoded), Endian::Little};
        s.block_entities.push_back(std::move(be));
    }
    for (const NbtView ent : st.find("entities").items()) {
        const NbtCompoundView c = ent.entries();
        Entity e;
        e.id = std::string(c.find("identifier").as_string().value_or("?"));
        if (const NbtView pos = c.find("Pos", TagType::List); pos && pos.items().size() == 3 &&
                                                                pos.items().element_type() == TagType::Float)
            e.pos = {static_cast<f64>(*pos.items().at(0).as_float()) - out.world_origin[0], static_cast<f64>(*pos.items().at(1).as_float()) - out.world_origin[1],
                     static_cast<f64>(*pos.items().at(2).as_float()) - out.world_origin[2]};
        s.entities.push_back(std::move(e));
    }
    return out;
}

} // namespace bridger::formats::bedrock

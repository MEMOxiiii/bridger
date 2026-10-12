// Java structure writers. Formats and sources: docs/formats/sponge-schem.md (v3),
// structure-nbt.md, litematic.md. Block states are written the way Java 26.3 writes them:
// palette compounds {id, properties} (NbtUtils.writeBlockState in the 26.3 server), and
// "namespace:id[k=v,...]" strings in Sponge palettes.

#include <bridger/formats/java/java_writers.hpp>
#include <bridger/io/compression.hpp>
#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>
#include <bridger/io/varint.hpp>

#include <algorithm>
#include <bit>
#include <unordered_map>

namespace bridger::formats::java {
namespace {

using io::nbt::NbtWriter;
using io::nbt::TagType;

Status check_java(const Structure& s) {
    if (s.source().edition != Edition::Java) return make_error(ErrorCode::InvalidArgument, "Java writers need a Java structure");
    return {};
}

// Distinct block ids in index order (air first when present, as Litematica expects).
struct Palette {
    std::vector<StateId> ids;
    std::unordered_map<StateId, u32> index;
    u32 add(StateId id) {
        const auto [it, inserted] = index.emplace(id, static_cast<u32>(ids.size()));
        if (inserted) ids.push_back(id);
        return it->second;
    }
};

template <typename Fn>
void for_each_block(const Structure& s, Fn&& fn) { // y, z, x order
    const Extent e = s.size();
    for (u32 y = 0; y < e.y; ++y)
        for (u32 z = 0; z < e.z; ++z)
            for (u32 x = 0; x < e.x; ++x)
                fn(x, y, z, s.block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)}));
}

// UTF-8 -> modified UTF-8 for NBT strings.
std::string utf8_or_empty(std::string_view s) { return io::nbt::utf8_to_mutf8(s).value_or(""); }

void write_state_compound(NbtWriter& w, const BlockState& st) {
    w.begin_compound();
    w.write_string("id", utf8_or_empty(st.id()));
    if (!st.properties().empty()) {
        w.begin_compound("properties");
        for (const BlockProperty& p : st.properties()) w.write_string(utf8_or_empty(p.name), utf8_or_empty(p.value));
        w.end_compound();
    }
    w.end_compound();
}

// Copies the entries of a block entity's stored NBT into the open compound.
Status write_be_data(NbtWriter& w, const BlockEntity& be) {
    if (be.data.empty()) return {};
    BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(be.data.bytes, be.data.endian));
    for (const io::nbt::NbtEntry& e : doc.root().entries())
        if (e.name != "id" && e.name != "x" && e.name != "y" && e.name != "z") w.write_view(e.name, e.value);
    return {};
}

Result<std::vector<u8>> gzip(NbtWriter&& w) {
    BRIDGER_TRY(const std::vector<u8> raw, std::move(w).finish());
    return io::compress(raw, io::Compression::Gzip, 6);
}

} // namespace

Result<std::vector<u8>> write_sponge_schem(const Structure& s, const JavaWriteOptions& o) {
    BRIDGER_TRY_STATUS(check_java(s));
    const Extent e = s.size();
    if (e.x > 0xFFFF || e.y > 0xFFFF || e.z > 0xFFFF)
        return make_error(ErrorCode::LimitExceeded, "schem: dimensions above 65535 cannot be stored");
    const BlockStateRegistry& reg = s.block_registry();

    // Block data: varints in x + z*W + y*W*L order, which is for_each_block's order.
    Palette pal;
    std::vector<u8> data;
    data.reserve(static_cast<usize>(e.volume()));
    for_each_block(s, [&](u32, u32, u32, StateId id) { io::encode_varint32(pal.add(id), data); });

    NbtWriter w(Endian::Big);
    w.begin_root_compound("");
    w.begin_compound("Schematic");
    w.write_int("Version", 3);
    w.write_int("DataVersion", o.data_version);
    w.write_short("Width", static_cast<i16>(static_cast<u16>(e.x)));
    w.write_short("Height", static_cast<i16>(static_cast<u16>(e.y)));
    w.write_short("Length", static_cast<i16>(static_cast<u16>(e.z)));
    const std::array<i32, 3> offset{s.offset.x, s.offset.y, s.offset.z};
    w.write_int_array("Offset", offset);
    w.begin_compound("Metadata");
    if (!o.name.empty()) w.write_string("Name", utf8_or_empty(o.name));
    if (!o.author.empty()) w.write_string("Author", utf8_or_empty(o.author));
    if (o.time_ms != 0) w.write_long("Date", o.time_ms);
    w.end_compound();

    w.begin_compound("Blocks");
    w.begin_compound("Palette");
    for (usize i = 0; i < pal.ids.size(); ++i) w.write_int(utf8_or_empty(reg.canonical(pal.ids[i])), static_cast<i32>(i));
    w.end_compound();
    w.write_byte_array("Data", data);
    w.begin_list("BlockEntities", s.block_entities.empty() ? TagType::End : TagType::Compound, s.block_entities.size());
    for (const BlockEntity& be : s.block_entities) {
        w.begin_compound();
        const std::array<i32, 3> pos{be.pos.x, be.pos.y, be.pos.z};
        w.write_int_array("Pos", pos);
        w.write_string("Id", utf8_or_empty(be.id));
        w.begin_compound("Data");
        BRIDGER_TRY_STATUS(write_be_data(w, be));
        w.end_compound();
        w.end_compound();
    }
    w.end_list();
    w.end_compound(); // Blocks

    if (s.has_biome_data()) {
        Palette bpal;
        std::vector<u8> bdata;
        bdata.reserve(static_cast<usize>(e.volume()));
        const BiomeId plains = s.biome_registry().intern("minecraft:plains"); // sections without biome data
        for_each_block(s, [&](u32 x, u32 y, u32 z, StateId) {
            const auto b = s.biome({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)});
            io::encode_varint32(bpal.add(b.value_or(plains)), bdata);
        });
        w.begin_compound("Biomes");
        w.begin_compound("Palette");
        for (usize i = 0; i < bpal.ids.size(); ++i) w.write_int(utf8_or_empty(s.biome_registry().name(bpal.ids[i])), static_cast<i32>(i));
        w.end_compound();
        w.write_byte_array("Data", bdata);
        w.end_compound();
    }
    w.begin_list("Entities", TagType::End, 0);
    w.end_list();
    w.end_compound(); // Schematic
    w.end_compound();
    return gzip(std::move(w));
}

Result<std::vector<u8>> write_structure_nbt(const Structure& s, const JavaWriteOptions& o) {
    BRIDGER_TRY_STATUS(check_java(s));
    const Extent e = s.size();
    const BlockStateRegistry& reg = s.block_registry();
    Palette pal;
    for_each_block(s, [&](u32, u32, u32, StateId id) { pal.add(id); });
    std::unordered_map<u64, const BlockEntity*> be_at;
    auto key = [](i32 x, i32 y, i32 z) { return (u64(u32(x)) << 42) ^ (u64(u32(y)) << 21) ^ u64(u32(z)); };
    for (const BlockEntity& be : s.block_entities)
        if (s.contains(be.pos)) be_at.emplace(key(be.pos.x, be.pos.y, be.pos.z), &be);

    NbtWriter w(Endian::Big);
    w.begin_root_compound("");
    w.write_int("DataVersion", o.data_version);
    w.begin_list("size", TagType::Int, 3);
    w.write_int({}, static_cast<i32>(e.x));
    w.write_int({}, static_cast<i32>(e.y));
    w.write_int({}, static_cast<i32>(e.z));
    w.end_list();
    w.begin_list("palette", TagType::Compound, pal.ids.size());
    for (const StateId id : pal.ids) write_state_compound(w, reg.get(id));
    w.end_list();
    // Every position is listed (air included), so placing the structure reproduces it exactly.
    w.begin_list("blocks", e.volume() == 0 ? TagType::End : TagType::Compound, static_cast<usize>(e.volume()));
    Status failure;
    for_each_block(s, [&](u32 x, u32 y, u32 z, StateId id) {
        w.begin_compound();
        w.begin_list("pos", TagType::Int, 3);
        w.write_int({}, static_cast<i32>(x));
        w.write_int({}, static_cast<i32>(y));
        w.write_int({}, static_cast<i32>(z));
        w.end_list();
        w.write_int("state", static_cast<i32>(pal.index.at(id)));
        if (const auto it = be_at.find(key(static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z))); it != be_at.end()) {
            w.begin_compound("nbt");
            w.write_string("id", utf8_or_empty(it->second->id));
            if (auto st = write_be_data(w, *it->second); !st && failure) failure = st;
            w.end_compound();
        }
        w.end_compound();
    });
    BRIDGER_TRY_STATUS(failure);
    w.end_list();
    w.begin_list("entities", TagType::End, 0);
    w.end_list();
    w.end_compound();
    return gzip(std::move(w));
}

Result<std::vector<u8>> write_litematic(const Structure& s, const JavaWriteOptions& o) {
    BRIDGER_TRY_STATUS(check_java(s));
    const Extent e = s.size();
    if (e.x > static_cast<u32>(INT32_MAX) || e.y > static_cast<u32>(INT32_MAX) || e.z > static_cast<u32>(INT32_MAX))
        return make_error(ErrorCode::LimitExceeded, "litematic: dimensions too large");
    const BlockStateRegistry& reg = s.block_registry();
    Palette pal;
    pal.add(kAirStateId); // Litematica's container palette starts with air
    u64 non_air = 0;
    for_each_block(s, [&](u32, u32, u32, StateId id) {
        pal.add(id);
        non_air += id != kAirStateId ? 1 : 0;
    });
    // max(2, bits for the palette); entries may span two longs, LSB first (docs/formats/litematic.md).
    const u32 bits = std::max<u32>(2, static_cast<u32>(std::bit_width(pal.ids.size() - 1)));
    std::vector<i64> packed(static_cast<usize>((e.volume() * bits + 63) / 64), 0);
    u64 index = 0;
    for_each_block(s, [&](u32, u32, u32, StateId id) { // (y * sizeZ + z) * sizeX + x == iteration order
        const u64 v = pal.index.at(id);
        const u64 start = index * bits;
        const usize word = static_cast<usize>(start >> 6);
        const unsigned shift = static_cast<unsigned>(start & 63);
        packed[word] = static_cast<i64>(static_cast<u64>(packed[word]) | (v << shift));
        if (shift + bits > 64) packed[word + 1] = static_cast<i64>(static_cast<u64>(packed[word + 1]) | (v >> (64 - shift)));
        ++index;
    });

    NbtWriter w(Endian::Big);
    w.begin_root_compound("");
    w.write_int("MinecraftDataVersion", o.data_version);
    w.write_int("Version", 7);
    w.write_int("SubVersion", 1);
    w.begin_compound("Metadata");
    w.write_string("Name", utf8_or_empty(o.name.empty() ? std::string("Bridger") : o.name));
    w.write_string("Author", utf8_or_empty(o.author));
    w.write_string("Description", "");
    w.write_int("RegionCount", 1);
    w.write_long("TotalVolume", static_cast<i64>(e.volume()));
    w.write_int("TotalBlocks", static_cast<i32>(std::min<u64>(non_air, INT32_MAX)));
    w.write_long("TimeCreated", o.time_ms);
    w.write_long("TimeModified", o.time_ms);
    w.begin_compound("EnclosingSize");
    w.write_int("x", static_cast<i32>(e.x));
    w.write_int("y", static_cast<i32>(e.y));
    w.write_int("z", static_cast<i32>(e.z));
    w.end_compound();
    w.end_compound();

    w.begin_compound("Regions");
    w.begin_compound(utf8_or_empty(o.name.empty() ? std::string("Bridger") : o.name));
    w.begin_compound("Position");
    w.write_int("x", 0);
    w.write_int("y", 0);
    w.write_int("z", 0);
    w.end_compound();
    w.begin_compound("Size");
    w.write_int("x", static_cast<i32>(e.x));
    w.write_int("y", static_cast<i32>(e.y));
    w.write_int("z", static_cast<i32>(e.z));
    w.end_compound();
    w.begin_list("BlockStatePalette", TagType::Compound, pal.ids.size());
    for (const StateId id : pal.ids) {
        // Litematica reads palettes with Minecraft's own NbtUtils, so the 26.3 spelling is used too.
        write_state_compound(w, reg.get(id));
    }
    w.end_list();
    w.write_long_array("BlockStates", packed);
    w.begin_list("TileEntities", s.block_entities.empty() ? TagType::End : TagType::Compound, s.block_entities.size());
    for (const BlockEntity& be : s.block_entities) {
        w.begin_compound();
        w.write_int("x", be.pos.x);
        w.write_int("y", be.pos.y);
        w.write_int("z", be.pos.z);
        w.write_string("id", utf8_or_empty(be.id));
        BRIDGER_TRY_STATUS(write_be_data(w, be));
        w.end_compound();
    }
    w.end_list();
    w.begin_list("Entities", TagType::End, 0);
    w.end_list();
    w.begin_list("PendingBlockTicks", TagType::End, 0);
    w.end_list();
    w.begin_list("PendingFluidTicks", TagType::End, 0);
    w.end_list();
    w.end_compound(); // region
    w.end_compound(); // Regions
    w.end_compound();
    return gzip(std::move(w));
}

} // namespace bridger::formats::java

#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/bedrock/mcworld/subchunk_codec.hpp>
#include <bridger/io/binary_reader.hpp>
#include <bridger/io/binary_writer.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include <algorithm>
#include <unordered_map>

namespace bridger::formats::bedrock {
namespace {

constexpr std::array<u32, 8> kAllowedBits{1, 2, 3, 4, 5, 6, 8, 16};

constexpr usize bridger_index(usize bedrock_index) noexcept {
    // Bedrock XZY (y fastest): i = x*256 + z*16 + y  ->  Bridger: y*256 + z*16 + x
    const usize x = bedrock_index >> 8, z = (bedrock_index >> 4) & 15, y = bedrock_index & 15;
    return (y << 8) | (z << 4) | x;
}

u32 bits_for(usize palette_size) {
    for (const u32 b : kAllowedBits)
        if (palette_size <= (usize{1} << b)) return b;
    return 16;
}

std::string canonical_state(io::nbt::NbtCompoundView c) {
    std::string name(c.find("name").as_string().value_or("?"));
    std::vector<std::pair<std::string, std::string>> props;
    for (const io::nbt::NbtEntry& e : c.find("states").entries()) {
        std::string v;
        if (const auto b = e.value.as_byte()) v = std::to_string(*b);
        else if (const auto i = e.value.as_int()) v = std::to_string(*i);
        else if (const auto s = e.value.as_string()) v = std::string(*s);
        else v = "?";
        props.emplace_back(std::string(e.name), std::move(v));
    }
    std::sort(props.begin(), props.end());
    if (!props.empty()) {
        name += '[';
        for (usize i = 0; i < props.size(); ++i) name += (i ? "," : "") + props[i].first + "=" + props[i].second;
        name += ']';
    }
    return name;
}

} // namespace

Result<std::vector<u8>> encode_subchunk(i8 y_index, std::span<const PaletteLayer* const> layers, const StateNbtFn& state_nbt) {
    io::BinaryWriter w(Endian::Little);
    w.write<u8>(kSubchunkFormat);
    w.write<u8>(static_cast<u8>(layers.size()));
    w.write<i8>(y_index);

    for (const PaletteLayer* layer : layers) {
        PaletteLayer compact = *layer;
        compact.compact(); // drop unused entries so the bit width is minimal
        const auto palette = compact.palette();
        const u32 bits = bits_for(palette.size());
        const u32 per_word = 32 / bits;
        const usize words = (kSectionVolume + per_word - 1) / per_word;

        // Palette slot of each value, in Bedrock order.
        std::array<u32, kSectionVolume> values; // NOLINT(cppcoreguidelines-pro-type-member-init)
        compact.copy_to(values);
        std::unordered_map<u32, u32> slot_of;
        slot_of.reserve(palette.size());
        for (usize s = 0; s < palette.size(); ++s) slot_of.emplace(palette[s], static_cast<u32>(s));
        std::vector<u32> packed(words, 0);
        for (usize i = 0; i < kSectionVolume; ++i) {
            const u32 slot = slot_of.at(values[bridger_index(i)]);
            packed[i / per_word] |= slot << ((i % per_word) * bits);
        }
        w.write<u8>(static_cast<u8>(bits << 1)); // runtime flag 0: persistent NBT palette
        for (const u32 word : packed) w.write<u32>(word);
        w.write<i32>(static_cast<i32>(palette.size()));
        for (const u32 state : palette) {
            BRIDGER_TRY(const std::span<const u8> nbt, state_nbt(state));
            w.write_bytes(nbt);
        }
    }
    return std::move(w).take();
}

namespace {

// One persistent block storage (docs/formats/bedrock-world.md, "Subchunks"). Bits 0 (one entry, no
// index words, no palette count) is not written by the game but by other tools (PocketMine-MP);
// some of those files carry a palette count of 1 anyway, which is accepted.
Result<RawSubchunkLayer> read_storage(io::BinaryReader& r, std::span<const u8> data) {
    BRIDGER_TRY(const u8 header, r.read<u8>());
    if (header & 1) return make_error(ErrorCode::UnsupportedFormat, "runtime palette in a stored subchunk");
    const u32 bits = header >> 1;
    RawSubchunkLayer layer;
    i32 count = 1;
    if (bits == 0) {
        layer.indices.fill(0);
        if (r.position() < data.size() && data[r.position()] != static_cast<u8>(io::nbt::TagType::Compound)) {
            BRIDGER_TRY(count, r.read<i32>());
            if (count != 1) return make_error(ErrorCode::Malformed, "subchunk: uniform storage with a palette size other than 1");
        }
    } else {
        if (std::find(kAllowedBits.begin(), kAllowedBits.end(), bits) == kAllowedBits.end())
            return make_error(ErrorCode::Malformed, "subchunk: invalid bits per index " + std::to_string(bits));
        const u32 per_word = 32 / bits;
        const usize words = (kSectionVolume + per_word - 1) / per_word;
        for (usize wi = 0, i = 0; wi < words; ++wi) {
            BRIDGER_TRY(const u32 word, r.read<u32>());
            for (u32 k = 0; k < per_word && i < kSectionVolume; ++k, ++i)
                layer.indices[bridger_index(i)] = static_cast<u16>((word >> (k * bits)) & ((1u << bits) - 1));
        }
        BRIDGER_TRY(count, r.read<i32>());
        if (count < 1 || count > static_cast<i32>(kSectionVolume))
            return make_error(ErrorCode::Malformed, "subchunk: invalid palette size");
    }
    for (i32 p = 0; p < count; ++p) {
        const auto rest = data.subspan(r.position());
        BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(rest, Endian::Little));
        layer.palette.emplace_back(rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(doc.consumed_bytes()));
        BRIDGER_TRY_STATUS(r.skip(doc.consumed_bytes()));
    }
    for (const u16 idx : layer.indices)
        if (idx >= layer.palette.size()) return make_error(ErrorCode::Malformed, "subchunk: index outside the palette");
    return layer;
}

} // namespace

std::vector<u8> legacy_palette_entry(u8 id, u8 data) {
    io::nbt::NbtWriter w(Endian::Little);
    w.begin_root_compound();
    w.write_int(kLegacyIdEntryKey, id);
    w.write_short("val", static_cast<i16>(data & 0xF));
    w.end_compound();
    return std::move(w).finish().value_or(std::vector<u8>{});
}

RawSubchunkLayer legacy_layer(std::span<const u8> ids, std::span<const u8> data, usize (*bedrock_offset)(usize x, usize y, usize z, usize base),
                              usize base) {
    RawSubchunkLayer layer;
    std::vector<i32> slot(256 * 16, -1);
    for (usize x = 0; x < 16; ++x)
        for (usize z = 0; z < 16; ++z)
            for (usize y = 0; y < 16; ++y) {
                const usize i = bedrock_offset(x, y, z, base);
                const u8 id = ids[i];
                // Nibble arrays: even index in the low four bits (PocketMine-MP ext-chunkutils2, SubChunkConverter.h).
                const u8 meta = static_cast<u8>((data[i >> 1] >> ((i & 1) * 4)) & 0xF);
                i32& s = slot[usize{id} * 16 + meta];
                if (s < 0) {
                    s = static_cast<i32>(layer.palette.size());
                    layer.palette.push_back(legacy_palette_entry(id, meta));
                }
                layer.indices[(y << 8) | (z << 4) | x] = static_cast<u16>(s);
            }
    return layer;
}

Result<RawSubchunk> decode_subchunk_raw(std::span<const u8> data) {
    io::BinaryReader r(data, Endian::Little);
    RawSubchunk out;
    BRIDGER_TRY(out.version, r.read<u8>());
    switch (out.version) {
    case 0: case 2: case 3: case 4: case 5: case 6: case 7: {
        // 1.0 - 1.2.x: 4096 block ids then 2048 bytes of data nibbles, XZY order (light may follow and
        // is ignored). Versions 2-7 were written by a third-party converter and are read like 0
        // (PocketMine-MP LevelDB.php, SubChunkVersion).
        if (data.size() < 1 + 4096 + 2048) return make_error(ErrorCode::Malformed, "legacy subchunk truncated");
        out.legacy = true;
        out.layers.push_back(legacy_layer(data.subspan(1, 4096), data.subspan(1 + 4096, 2048),
                                          [](usize x, usize y, usize z, usize) { return (x << 8) | (z << 4) | y; }, 0));
        return out;
    }
    case 1: { // 1.2.13 betas and release: one storage, no layer count
        BRIDGER_TRY(RawSubchunkLayer layer, read_storage(r, data));
        out.layers.push_back(std::move(layer));
        return out;
    }
    case 8: case 9: break;
    default:
        return make_error(ErrorCode::UnsupportedVersion, "subchunk version " + std::to_string(out.version));
    }
    BRIDGER_TRY(const u8 layer_count, r.read<u8>());
    if (out.version == 9) {
        BRIDGER_TRY(out.y_index, r.read<i8>());
    }
    for (u8 l = 0; l < layer_count; ++l) {
        BRIDGER_TRY(RawSubchunkLayer layer, read_storage(r, data));
        out.layers.push_back(std::move(layer));
    }
    return out;
}

Result<LegacyTerrain> decode_legacy_terrain(std::span<const u8> value) {
    // Pre-1.0 column, 16 x 128 x 16, XZY (i = x*2048 + z*128 + y): 32768 ids, 16384 bytes of data
    // nibbles, 16384 sky light, 16384 block light, 256 heightmap, 256 big-endian u32 biome entries
    // (id in the top byte, RGB colour below) [1][PocketMine-MP LevelDB.php].
    constexpr usize kIds = 32768, kData = 16384, kLight = 32768, kHeight = 256, kBiomes = 1024;
    if (value.size() < kIds + kData + kLight + kHeight + kBiomes) return make_error(ErrorCode::Malformed, "LegacyTerrain truncated");
    LegacyTerrain out;
    const auto ids = value.first(kIds), data = value.subspan(kIds, kData);
    for (usize sy = 0; sy < 8; ++sy)
        out.subchunks[sy] = legacy_layer(ids, data, [](usize x, usize y, usize z, usize base) { return (x << 11) | (z << 7) | (base + y); }, sy * 16);
    const usize b = kIds + kData + kLight + kHeight;
    for (usize i = 0; i < 256; ++i) out.biomes[i] = value[b + i * 4]; // index z*16 + x
    return out;
}

Result<DecodedSubchunk> decode_subchunk(std::span<const u8> data) {
    io::BinaryReader r(data, Endian::Little);
    DecodedSubchunk out;
    BRIDGER_TRY(out.version, r.read<u8>());
    if (out.version != 8 && out.version != 9)
        return make_error(ErrorCode::UnsupportedVersion, "subchunk version " + std::to_string(out.version));
    BRIDGER_TRY(const u8 layer_count, r.read<u8>());
    if (out.version == 9) {
        BRIDGER_TRY(out.y_index, r.read<i8>());
    }
    for (u8 l = 0; l < layer_count; ++l) {
        BRIDGER_TRY(const u8 header, r.read<u8>());
        if (header & 1) return make_error(ErrorCode::UnsupportedFormat, "runtime palette in a stored subchunk");
        const u32 bits = header >> 1;
        if (std::find(kAllowedBits.begin(), kAllowedBits.end(), bits) == kAllowedBits.end())
            return make_error(ErrorCode::Malformed, "subchunk: invalid bits per index " + std::to_string(bits));
        const u32 per_word = 32 / bits;
        const usize words = (kSectionVolume + per_word - 1) / per_word;
        DecodedLayer layer;
        for (usize wi = 0, i = 0; wi < words; ++wi) {
            BRIDGER_TRY(const u32 word, r.read<u32>());
            for (u32 k = 0; k < per_word && i < kSectionVolume; ++k, ++i)
                layer.indices[bridger_index(i)] = static_cast<u16>((word >> (k * bits)) & ((1u << bits) - 1));
        }
        BRIDGER_TRY(const i32 count, r.read<i32>());
        if (count < 0 || count > static_cast<i32>(kSectionVolume))
            return make_error(ErrorCode::Malformed, "subchunk: invalid palette size");
        for (i32 p = 0; p < count; ++p) {
            const auto rest = data.subspan(r.position());
            BRIDGER_TRY(const io::nbt::NbtDocument doc, io::nbt::NbtDocument::parse(rest, Endian::Little));
            layer.palette.push_back(canonical_state(doc.root().entries()));
            BRIDGER_TRY_STATUS(r.skip(doc.consumed_bytes()));
        }
        for (const u16 idx : layer.indices)
            if (idx >= layer.palette.size()) return make_error(ErrorCode::Malformed, "subchunk: index outside the palette");
        out.layers.push_back(std::move(layer));
    }
    return out;
}

} // namespace bridger::formats::bedrock

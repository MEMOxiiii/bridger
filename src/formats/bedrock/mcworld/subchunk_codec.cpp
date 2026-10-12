#include <bridger/formats/bedrock/mcworld/chunk_key.hpp>
#include <bridger/formats/bedrock/mcworld/subchunk_codec.hpp>
#include <bridger/io/binary_reader.hpp>
#include <bridger/io/binary_writer.hpp>
#include <bridger/io/nbt/nbt_view.hpp>

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

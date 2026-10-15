// Fuzz target: Bedrock subchunk values of every version (0-9), the pre-1.0 LegacyTerrain column,
// and the block state upgrader on every palette entry the decoder accepted (old worlds,
// docs/formats/bedrock-world.md). The first byte picks the decoder.

#include <bridger/formats/bedrock/mcworld/subchunk_codec.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/translate/bedrock_upgrade.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace bridger;
using namespace bridger::formats::bedrock;

namespace {

void upgrade_all(const RawSubchunkLayer& layer) {
    static const translate::BedrockUpgrader* up = translate::BedrockUpgrader::embedded().value();
    for (const u16 i : layer.indices)
        if (i >= layer.palette.size()) std::abort();
    for (const auto& entry : layer.palette) {
        io::nbt::NbtLimits limits;
        limits.max_tags = 1u << 16;
        auto doc = io::nbt::NbtDocument::parse(entry, Endian::Little, limits);
        if (!doc || !doc->root().is(io::nbt::TagType::Compound)) continue;
        if (const auto st = up->from_entry(doc->root().entries())) (void)st->properties();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 1) return 0;
    const std::span<const u8> rest(data + 1, size - 1);
    if (data[0] & 1) {
        if (auto t = decode_legacy_terrain(rest))
            for (const auto& layer : t->subchunks) upgrade_all(layer);
    } else if (auto sub = decode_subchunk_raw(rest)) {
        for (const auto& layer : sub->layers) upgrade_all(layer);
    }
    return 0;
}

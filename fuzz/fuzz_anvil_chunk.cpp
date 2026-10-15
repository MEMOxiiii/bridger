// Fuzz target: the Java chunk decoder (every layout: 1.18+ sections, the pre-1.18 Level compound,
// numeric ids, McRegion columns) on raw chunk NBT, as stored in a region file after
// decompression. Accepted chunks are re-encoded with Bridger's writer (always the 1.18+ layout)
// and decoded again: every block must survive the round trip.

#include <bridger/formats/java/anvil/anvil_writer.hpp>
#include <bridger/convert/pipeline.hpp>
#include <bridger/formats/java/anvil/chunk_codec.hpp>
#include <bridger/translate/mapping_data.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace bridger;
using namespace bridger::formats::java;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    bridger::io::nbt::NbtLimits limits;
    limits.max_tags = 1u << 18;
    static const LegacyTables legacy = bridger::convert::legacy_tables(*bridger::translate::MappingData::embedded().value());
    auto chunk = decode_chunk(std::span<const u8>(data, size), std::make_shared<BlockStateRegistry>(),
                              std::make_shared<BiomeRegistry>(), limits, &legacy);
    if (!chunk) return 0;
    const Structure& s = chunk->column;
    if (s.size().x != 16 || s.size().z != 16) std::abort();
    const BlockStateRegistry& reg = s.block_registry();
    for (u32 sy = 0; sy < s.sections_y(); ++sy) {
        const Section& sec = s.section(0, sy, 0);
        for (const u32 id : sec.blocks().palette())
            if (id >= reg.size()) std::abort();
    }
    for (const BlockEntity& be : s.block_entities)
        if (!s.contains(be.pos)) std::abort();

    const auto encoded = encode_chunk(s, chunk->pos, chunk->min_y, std::max(chunk->data_version, kFirstFlatteningChunkDataVersion));
    if (!encoded) return 0; // e.g. a block entity whose data cannot be re-parsed
    auto again = decode_chunk(*encoded, std::make_shared<BlockStateRegistry>(), std::make_shared<BiomeRegistry>(), limits);
    if (!again || again->column.size() != s.size()) std::abort();
    for (u32 y = 0; y < s.size().y; ++y)
        for (u32 z = 0; z < 16; ++z)
            for (u32 x = 0; x < 16; ++x) {
                const BlockPos p{static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)};
                if (reg.canonical(s.block(p)) != again->column.block_registry().canonical(again->column.block(p))) std::abort();
            }
    return 0;
}

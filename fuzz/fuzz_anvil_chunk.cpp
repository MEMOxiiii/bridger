// Fuzz target: the Java chunk decoder (1.18+ sections) on raw chunk NBT, as stored in a
// region file after decompression.

#include <bridger/formats/java/anvil/chunk_codec.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace bridger;
using namespace bridger::formats::java;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    bridger::io::nbt::NbtLimits limits;
    limits.max_tags = 1u << 18;
    auto chunk = decode_chunk(std::span<const u8>(data, size), std::make_shared<BlockStateRegistry>(),
                              std::make_shared<BiomeRegistry>(), limits);
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
    return 0;
}

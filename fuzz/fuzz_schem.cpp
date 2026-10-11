// Fuzz target: the Sponge .schem reader on raw (uncompressed) NBT input. It must
// either reject the input with an error or produce a structure whose every
// position can be read.

#include <bridger/formats/java/sponge_schem.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace bridger;
using namespace bridger::formats::java;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    SchemReadOptions opt;
    opt.max_volume = 1u << 16;
    opt.max_palette_size = 1u << 12;
    opt.decompress.max_output_bytes = 1u << 22;
    opt.nbt.max_tags = 1u << 18;

    auto file = read_sponge_schem(std::span<const u8>(data, size), std::make_shared<BlockStateRegistry>(),
                                  std::make_shared<BiomeRegistry>(), opt);
    if (!file) return 0;
    const Structure& s = file->structure;
    const BlockStateRegistry& reg = s.block_registry();
    for (u32 y = 0; y < s.size().y; ++y)
        for (u32 z = 0; z < s.size().z; ++z)
            for (u32 x = 0; x < s.size().x; ++x) {
                const StateId id = s.block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)});
                if (id >= reg.size()) std::abort();
            }
    for (const BlockEntity& be : s.block_entities)
        if (!s.contains(be.pos)) std::abort();
    return 0;
}

// Fuzz target: the .mcstructure reader. Palette entries resolve by a cheap stand-in
// lookup (name length) so every code path after the palette is reached.

#include <bridger/formats/bedrock/mcstructure.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace bridger;
using namespace bridger::formats::bedrock;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    McStructureReadOptions opt;
    opt.max_volume = 1u << 16;
    opt.nbt.max_tags = 1u << 18;
    const StateLookupFn lookup = [](bridger::io::nbt::NbtCompoundView e) -> std::optional<u32> {
        const auto name = e.find("name").as_string();
        if (!name || name->size() % 5 == 0) return std::nullopt;
        return static_cast<u32>(name->size() % 64);
    };
    auto file = read_mcstructure(std::span<const u8>(data, size), std::make_shared<BlockStateRegistry>(),
                                 std::make_shared<BiomeRegistry>(), 18100000, lookup, 0, opt);
    if (!file) return 0;
    const Structure& s = file->structure;
    for (u32 y = 0; y < s.size().y; ++y)
        for (u32 z = 0; z < s.size().z; ++z)
            for (u32 x = 0; x < s.size().x; ++x)
                (void)s.block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)});
    for (const BlockEntity& be : s.block_entities)
        if (!s.contains(be.pos)) std::abort();
    return 0;
}

// Fuzz target: the structure .nbt, .litematic and legacy .schematic readers. The first
// byte picks the reader; the rest is the file. Each must either reject the input with an
// error or produce a structure whose every position and block entity is in range.

#include <bridger/formats/java/legacy_schematic.hpp>
#include <bridger/formats/structure_file.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace bridger;
using namespace bridger::formats;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 1) return 0;
    static constexpr FormatId kFormats[] = {FormatId::StructureNbt, FormatId::Litematic, FormatId::LegacySchematic};
    const FormatId format = kFormats[data[0] % 3];
    StructureReadOptions opt;
    opt.max_volume = 1u << 16;
    opt.max_palette_size = 1u << 12;
    opt.decompress.max_output_bytes = 1u << 22;
    opt.nbt.max_tags = 1u << 18;
    // Legacy ids resolve to a fixed name so the fuzzer exercises the whole reader.
    const java::LegacyTables legacy{
        [](u16 id, u8) -> std::optional<std::string> {
            if (id % 7 == 0) return std::nullopt;
            return id == 0 ? std::string("minecraft:air") : std::string("minecraft:stone");
        },
        [](u16 id, u16) -> std::optional<std::string> {
            if (id % 5 == 0) return std::nullopt;
            return std::string("minecraft:stick");
        }};

    auto file = read_java_structure(std::span<const u8>(data + 1, size - 1), format, std::make_shared<BlockStateRegistry>(),
                                    std::make_shared<BiomeRegistry>(), &legacy, opt);
    if (!file) return 0;
    const Structure& s = file->structure;
    const BlockStateRegistry& reg = s.block_registry();
    for (u32 y = 0; y < s.size().y; ++y)
        for (u32 z = 0; z < s.size().z; ++z)
            for (u32 x = 0; x < s.size().x; ++x)
                if (s.block({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z)}) >= reg.size()) std::abort();
    for (const BlockEntity& be : s.block_entities)
        if (!s.contains(be.pos)) std::abort();
    return 0;
}

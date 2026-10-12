// Fuzz target: the .mcstructure reader, then Bedrock -> Java translation and the Java writers
// on whatever it accepted. Palette entries resolve by a cheap stand-in lookup (name length)
// to real Bedrock state indices, so every code path after the palette is reached.

#include <bridger/formats/bedrock/mcstructure.hpp>
#include <bridger/formats/java/java_writers.hpp>
#include <bridger/translate/mapping_data.hpp>
#include <bridger/translate/translator.hpp>

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

    static const translate::MappingData* mapping = translate::MappingData::embedded().value();
    translate::ConversionReport report;
    auto java = translate::translate_to_java(s, *mapping, {}, report);
    if (!java) return 0;
    if (java->size() != s.size()) std::abort();
    const formats::java::JavaWriteOptions wo{static_cast<i32>(mapping->java_data_version()), "fuzz", "fuzz", 0};
    if (!formats::java::write_sponge_schem(*java, wo) && java->size().x <= 0xFFFF && java->size().y <= 0xFFFF &&
        java->size().z <= 0xFFFF)
        std::abort();
    (void)formats::java::write_litematic(*java, wo);
    return 0;
}

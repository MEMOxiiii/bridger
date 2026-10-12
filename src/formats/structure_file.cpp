#include <bridger/formats/java/legacy_schematic.hpp>
#include <bridger/formats/java/litematic.hpp>
#include <bridger/formats/java/sponge_schem.hpp>
#include <bridger/formats/java/structure_nbt.hpp>
#include <bridger/formats/structure_file.hpp>

namespace bridger::formats {

Result<StructureFile> read_java_structure(std::span<const u8> file_bytes, FormatId format,
                                          std::shared_ptr<BlockStateRegistry> blocks,
                                          std::shared_ptr<BiomeRegistry> biomes, const java::LegacyTables* legacy,
                                          const StructureReadOptions& options) {
    switch (format) {
    case FormatId::SpongeSchem: {
        java::SchemReadOptions so;
        so.decompress = options.decompress;
        so.nbt = options.nbt;
        so.max_volume = options.max_volume;
        so.max_palette_size = options.max_palette_size;
        BRIDGER_TRY(java::SchemFile schem, java::read_sponge_schem(file_bytes, std::move(blocks), std::move(biomes), so));
        return StructureFile{FormatId::SpongeSchem, schem.info.data_version, std::move(schem.info.warnings),
                             std::move(schem.structure)};
    }
    case FormatId::StructureNbt: return java::read_structure_nbt(file_bytes, std::move(blocks), std::move(biomes), options);
    case FormatId::Litematic: return java::read_litematic(file_bytes, std::move(blocks), std::move(biomes), options);
    case FormatId::LegacySchematic:
        if (legacy == nullptr) return make_error(ErrorCode::InvalidArgument, "reading .schematic needs the legacy id tables");
        return java::read_legacy_schematic(file_bytes, std::move(blocks), std::move(biomes), *legacy, options);
    default: return make_error(ErrorCode::UnsupportedFormat, "not a Java structure format");
    }
}

} // namespace bridger::formats

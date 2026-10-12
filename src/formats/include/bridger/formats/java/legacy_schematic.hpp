#pragma once

// MCEdit / old WorldEdit schematics (.schematic, numeric block ids before 1.13),
// docs/formats/legacy-schematic.md. Numeric ids are turned into Java states of the
// pinned version with the legacy table in the mapping data; ids the table does not know
// become placeholder states "bridger:legacy_<id>_<data>", which translation reports.

#include <bridger/formats/structure_file.hpp>

#include <functional>
#include <optional>
#include <string_view>

namespace bridger::formats::java {

// Lookups into the legacy table (provided by the caller: bridger_formats cannot depend
// on bridger_translate, which owns the mapping data).
struct LegacyTables {
    std::function<std::optional<std::string>(u16 id, u8 data)> block;   // canonical Java state string
    std::function<std::optional<std::string>(u16 id, u16 damage)> item; // Java item id
};

[[nodiscard]] Result<StructureFile> read_legacy_schematic(std::span<const u8> file_bytes,
                                                          std::shared_ptr<BlockStateRegistry> blocks,
                                                          std::shared_ptr<BiomeRegistry> biomes,
                                                          const LegacyTables& tables,
                                                          const StructureReadOptions& options = {});

} // namespace bridger::formats::java

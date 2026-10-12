#pragma once

// Writers for Java structure files: Sponge .schem (v3), structure .nbt and .litematic.
// Input: a Java structure (block ids in its registry, biome ids in its biome registry,
// block entities as Java NBT without id/x/y/z). Output: the gzip-compressed file bytes.
// Formats: docs/formats/sponge-schem.md, structure-nbt.md, litematic.md.

#include <bridger/core/result.hpp>
#include <bridger/core/structure.hpp>

#include <string>
#include <vector>

namespace bridger::formats::java {

struct JavaWriteOptions {
    i32 data_version = 0; // the pinned Java DataVersion of the states in the structure
    std::string name;     // metadata (where the format has it)
    std::string author = "Bridger";
    i64 time_ms = 0;      // creation time in ms since the epoch (0: omitted where optional)
};

[[nodiscard]] Result<std::vector<u8>> write_sponge_schem(const Structure& s, const JavaWriteOptions& options);
[[nodiscard]] Result<std::vector<u8>> write_structure_nbt(const Structure& s, const JavaWriteOptions& options);
[[nodiscard]] Result<std::vector<u8>> write_litematic(const Structure& s, const JavaWriteOptions& options);

} // namespace bridger::formats::java

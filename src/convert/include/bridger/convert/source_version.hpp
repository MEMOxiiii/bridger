#pragma once

// Which game version saved a file, as shown before converting it (docs/versions.md, "Version names").

#include <bridger/core/types.hpp>
#include <bridger/formats/format.hpp>

#include <optional>
#include <string>
#include <vector>

namespace bridger::convert {

struct GameVersionInfo {
    Edition edition = Edition::Java;
    std::string name;   // "1.20.1", "26.50", "before 1.13"; empty: the file records no version
    std::string detail; // how it is known: "DataVersion 3465", "last opened with 1.21.50", ...
    bool exact = false; // the file names one version (not a range or a lower bound)
};

// Java: DataVersion (structures, chunks, level.dat), the name level.dat stores, or level.dat's format number.
[[nodiscard]] GameVersionInfo java_version_info(formats::FormatId format, std::optional<i32> data_version,
                                                std::optional<std::string> version_name = {},
                                                std::optional<i32> storage_format = {});
// Bedrock world: level.dat lastOpenedWithVersion.
[[nodiscard]] GameVersionInfo bedrock_world_version_info(const std::vector<i32>& last_opened);
// .mcstructure: the highest block state version in its palette (packed a.b.c.d bytes).
[[nodiscard]] GameVersionInfo bedrock_block_version_info(i32 block_version);

// "Java 1.20.1 (DataVersion 3465)", "Bedrock: not recorded".
[[nodiscard]] std::string describe(const GameVersionInfo& v);

} // namespace bridger::convert

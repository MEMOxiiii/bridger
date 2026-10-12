#pragma once

// The conversion pipeline: read -> place -> translate -> write -> validate -> atomic move.
// Supported paths are listed in docs/formats/README.md and by `bridger formats`.

#include <bridger/convert/placement.hpp>
#include <bridger/convert/placement_policy.hpp>
#include <bridger/core/result.hpp>
#include <bridger/formats/format.hpp>
#include <bridger/translate/conversion_report.hpp>
#include <bridger/translate/translator.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bridger::convert {

struct ConvertOptions {
    std::filesystem::path input;
    std::filesystem::path output;
    std::optional<formats::FormatId> input_format;  // default: detected from extension + bytes
    std::optional<formats::FormatId> output_format; // default: from the output extension
    std::optional<std::filesystem::path> into_world; // place into a copy of this existing .mcworld
    translate::Fallback fallback = translate::Fallback::Air;
    PlacementPolicy placement;
    std::string level_name; // new worlds; default: the input file name
    bool overwrite = false; // an existing output is replaced only when asked
    usize threads = 0;      // worker threads for whole worlds (0 = all hardware threads)
};

struct ConvertSummary {
    formats::FormatId input_format = formats::FormatId::SpongeSchem;
    formats::FormatId output_format = formats::FormatId::McWorld;
    translate::ConversionReport report;
    std::vector<std::string> warnings; // reader/writer warnings (never silently dropped)
    std::optional<Placement> placement; // world outputs only
    u64 blocks = 0;                     // non-air blocks in the source
    usize chunks = 0;
    usize subchunks = 0;
    usize block_entities = 0;
    std::optional<i32> source_data_version;
    u32 target_data_version = 0; // the pinned Java DataVersion of the mapping data
};

[[nodiscard]] Result<ConvertSummary> convert(const ConvertOptions& options);

// Kept for 0.1.0 callers: convert() with .schem -> new .mcworld.
[[nodiscard]] Result<ConvertSummary> convert_schem_to_new_mcworld(const ConvertOptions& options);

// Machine-readable report (--report report.json).
[[nodiscard]] std::string summary_to_json(const ConvertSummary& summary, const ConvertOptions& options);

} // namespace bridger::convert

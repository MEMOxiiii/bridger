#pragma once

// The conversion pipeline: read -> place -> translate -> write -> validate -> atomic move.
// v0.1 supports exactly one path: Sponge .schem -> new Bedrock world (.mcworld).

#include <bridger/convert/placement.hpp>
#include <bridger/convert/placement_policy.hpp>
#include <bridger/core/result.hpp>
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
    translate::Fallback fallback = translate::Fallback::Air;
    PlacementPolicy placement;
    std::string level_name; // default: the input file name
    bool overwrite = false;  // an existing output is replaced only when asked
};

struct ConvertSummary {
    translate::ConversionReport report;
    std::vector<std::string> warnings; // reader warnings (never silently dropped)
    Placement placement;
    u64 blocks = 0;          // non-air blocks written
    usize chunks = 0;
    usize subchunks = 0;
    usize block_entities = 0;
    std::optional<i32> source_data_version;
    u32 target_data_version = 0; // the pinned Java DataVersion of the mapping data
};

[[nodiscard]] Result<ConvertSummary> convert_schem_to_new_mcworld(const ConvertOptions& options);

// Machine-readable report (--report report.json).
[[nodiscard]] std::string summary_to_json(const ConvertSummary& summary, const ConvertOptions& options);

} // namespace bridger::convert

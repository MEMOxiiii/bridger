#pragma once

// The conversion pipeline: read -> place -> translate -> write -> validate -> atomic move.
// Supported paths are listed in docs/formats/README.md and by `bridger formats`.

#include <bridger/convert/placement.hpp>
#include <bridger/convert/placement_policy.hpp>
#include <bridger/convert/source_version.hpp>
#include <bridger/core/result.hpp>
#include <bridger/formats/bedrock/mcstructure.hpp>
#include <bridger/formats/java/legacy_schematic.hpp>
#include <bridger/formats/format.hpp>
#include <bridger/translate/conversion_report.hpp>
#include <bridger/translate/translator.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
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
    // Game version to write for ("1.20.1", "1.21.50", "26.50"); empty: the newest (the pinned version).
    // Lists: translate::JavaTargets / BedrockTargets (docs/versions.md, "Target versions").
    std::string target_version;
    bool overwrite = false; // an existing output is replaced only when asked
    usize threads = 0;      // worker threads for whole worlds (0 = all hardware threads)
    // Called on the converting thread with the overall fraction done (0..1) and the current step.
    std::function<void(float fraction, std::string_view step)> progress;
    // Checked between steps and chunk batches; when set, convert() stops with ErrorCode::Cancelled
    // before anything is moved into place (no partial output is left behind).
    const std::atomic<bool>* cancel = nullptr;
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
    GameVersionInfo source_version; // which game version saved the input
    std::string target_version;     // what the output was written for: "Java 1.20.1", "Bedrock 1.21.50"
};

[[nodiscard]] Result<ConvertSummary> convert(const ConvertOptions& options);

// Any structure file (.schem, .nbt, .litematic, .schematic, .mcstructure) as a Java structure,
// for viewing: Bedrock files are translated to Java; what cannot be shown exactly is in `report`.
struct ViewStructure {
    formats::FormatId format = formats::FormatId::SpongeSchem;
    Structure java;
    std::vector<std::string> warnings;
    translate::ConversionReport report;
    GameVersionInfo source_version;
};
[[nodiscard]] Result<ViewStructure> load_view_structure(const std::filesystem::path& input);

// Resolves a Bedrock palette entry {name, states} to its index in the pinned palette (for
// readers of Bedrock files; also used by `bridger info`).
[[nodiscard]] formats::bedrock::StateLookupFn bedrock_state_lookup(const translate::MappingData& mapping);
// Pre-1.13 numeric block/item ids -> pinned Java states and items (the mapping data's legacy table).
[[nodiscard]] formats::java::LegacyTables legacy_tables(const translate::MappingData& mapping);

// Kept for 0.1.0 callers: convert() with .schem -> new .mcworld.
[[nodiscard]] Result<ConvertSummary> convert_schem_to_new_mcworld(const ConvertOptions& options);

// Machine-readable report (--report report.json).
[[nodiscard]] std::string summary_to_json(const ConvertSummary& summary, const ConvertOptions& options);

} // namespace bridger::convert

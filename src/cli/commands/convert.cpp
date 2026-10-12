// `bridger convert <input> <output> [options]`. The pipeline decides which format
// combinations are supported (see `bridger formats`).

#include "commands.hpp"

#include <bridger/convert/pipeline.hpp>
#include <bridger/core/log.hpp>
#include <bridger/formats/detect.hpp>
#include <bridger/platform/atomic_replace.hpp>
#include <bridger/platform/paths.hpp>

#include <cstdio>
#include <format>

namespace bridger::cli {
namespace {

void print_report(const convert::ConvertSummary& s) {
    using translate::IssueKind;
    if (s.placement)
        std::puts(std::format("Placed at {}, {}, {} (spawn {}, {}, {})", s.placement->origin.x, s.placement->origin.y,
                              s.placement->origin.z, s.placement->spawn.x, s.placement->spawn.y, s.placement->spawn.z)
                      .c_str());
    std::puts(std::format("{} -> {}: {} blocks, {} chunks ({} subchunks), {} block entities",
                          formats::info(s.input_format).name, formats::info(s.output_format).name, s.blocks, s.chunks,
                          s.subchunks, s.block_entities)
                  .c_str());
    for (const auto& w : s.warnings) log::warn("{}", w);
    if (s.report.exact()) {
        std::puts("Conversion report: everything was converted exactly.");
        return;
    }
    std::puts("Conversion report (not converted exactly):");
    for (const IssueKind kind : {IssueKind::UnmappedBlock, IssueKind::LossyBlock, IssueKind::UnmappedBlockEntity,
                                 IssueKind::UnmappedEntity, IssueKind::UnmappedBiome, IssueKind::DroppedSecondaryLayer,
                                 IssueKind::RemovedBlockEntity, IssueKind::RemovedEntity, IssueKind::ClippedBlock}) {
        const auto* subjects = s.report.issues(kind);
        if (subjects == nullptr) continue;
        std::puts(std::format("  {}: {}", translate::to_string(kind), s.report.total(kind)).c_str());
        usize shown = 0;
        for (const auto& [subject, entry] : *subjects) {
            if (shown++ == 10) {
                std::puts(std::format("    ... and {} more (see --report)", subjects->size() - 10).c_str());
                break;
            }
            const BlockPos& p = entry.examples.front();
            std::puts(std::format("    {} x{} (e.g. at {}, {}, {})", subject, entry.count, p.x, p.y, p.z).c_str());
        }
    }
}

} // namespace

int run_convert(const ConvertArgs& a) {
    convert::ConvertOptions o;
    auto in = platform::path_from_utf8(a.input);
    auto out = platform::path_from_utf8(a.output);
    if (!in || !out) {
        log::error("invalid path (not UTF-8)");
        return kExitError;
    }
    o.input = *in;
    o.output = *out;
    o.overwrite = a.overwrite;
    o.threads = a.threads;
    o.level_name = a.level_name;
    o.fallback = a.fallback == "marker" ? translate::Fallback::Marker : translate::Fallback::Air;
    if (a.fallback != "air" && a.fallback != "marker") {
        log::error("invalid --fallback '{}' (expected air or marker)", a.fallback);
        return kExitError;
    }
    if (auto air = convert::parse_air_mode(a.air); air) o.placement.air = *air;
    else {
        log::error("{}", air.error().message());
        return kExitError;
    }
    if (a.clip && a.shift_to_fit) {
        log::error("--clip and --shift-to-fit are mutually exclusive");
        return kExitError;
    }
    o.placement.height = a.clip ? convert::HeightPolicy::Clip
                         : a.shift_to_fit ? convert::HeightPolicy::ShiftToFit
                                          : convert::HeightPolicy::Fail;
    if (!a.place_at.empty()) {
        auto pos = convert::parse_block_pos(a.place_at);
        if (!pos) {
            log::error("{}", pos.error().message());
            return kExitError;
        }
        o.placement.place_at = *pos;
    }
    if (!a.into_world.empty()) {
        auto world = platform::path_from_utf8(a.into_world);
        if (!world) {
            log::error("invalid --into-world path");
            return kExitError;
        }
        o.into_world = *world;
    }
    o.placement.clear_entities = a.clear_entities;
    if (!a.format.empty()) {
        o.output_format = formats::format_from_name(a.format);
        if (!o.output_format) {
            log::error("unknown --format '{}' (see `bridger formats`)", a.format);
            return kExitError;
        }
    }

    auto summary = convert::convert(o);
    if (!summary) {
        log::error("{}", summary.error().describe());
        return summary.error().code() == ErrorCode::NotImplemented ? kExitNotImplemented : kExitError;
    }
    print_report(*summary);

    if (!a.report_path.empty()) {
        auto report_path = platform::path_from_utf8(a.report_path);
        const std::string json = convert::summary_to_json(*summary, o);
        const auto bytes = std::span<const u8>(reinterpret_cast<const u8*>(json.data()), json.size());
        if (!report_path ||
            !platform::write_file_atomically(*report_path, bytes, [](const std::filesystem::path&) -> Status { return {}; })) {
            log::error("could not write the report to {}", a.report_path);
            return kExitError;
        }
        std::puts(std::format("Report written to {}", a.report_path).c_str());
    }
    std::puts(std::format("Done: {}", a.output).c_str());
    return kExitOk;
}

} // namespace bridger::cli

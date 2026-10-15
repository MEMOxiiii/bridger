// `bridger convert <input> <output> [options]`. The pipeline decides which format
// combinations are supported (see `bridger formats`).

#include "commands.hpp"

#include <bridger/translate/targets.hpp>

#include <bridger/convert/pipeline.hpp>
#include <bridger/core/log.hpp>
#include <bridger/formats/detect.hpp>
#include <bridger/platform/atomic_replace.hpp>
#include <bridger/platform/paths.hpp>

#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <format>
#include <optional>

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
        if (s.warnings.empty()) std::puts("Conversion report: everything was converted exactly.");
        else
            std::puts(std::format("Conversion report: the translation recorded no problems, but {} warning(s) above describe data "
                                  "that was skipped or dropped. The conversion is not exact.",
                                  s.warnings.size())
                          .c_str());
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

// Windows paths compare case-insensitively; existing files are compared by identity (links, 8.3 names).
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec)) return std::filesystem::equivalent(a, b, ec);
    auto norm = [](const std::filesystem::path& p) {
        std::error_code e;
        std::wstring w = std::filesystem::weakly_canonical(std::filesystem::absolute(p, e), e).wstring();
        for (wchar_t& c : w) c = static_cast<wchar_t>(std::towlower(c));
        while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
        return w;
    };
    return norm(a) == norm(b);
}

bool inside(const std::filesystem::path& child, const std::filesystem::path& dir) {
    for (std::filesystem::path p = std::filesystem::absolute(child).parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path())
        if (same_path(p, dir)) return true;
    return false;
}

// The report must never replace the input, the output or a world being placed into, and an existing
// file is only replaced when asked. Checked before converting, so nothing is lost and no work is wasted.
Status check_report_path(const std::filesystem::path& report, const convert::ConvertOptions& o) {
    const std::string shown = platform::path_to_utf8(report).value_or("the report path");
    if (same_path(report, o.input) || inside(report, o.input))
        return make_error(ErrorCode::InvalidArgument, "--report " + shown + " would overwrite the input; choose another file");
    if (same_path(report, o.output) || inside(report, o.output))
        return make_error(ErrorCode::InvalidArgument, "--report " + shown + " would overwrite the output; choose another file");
    if (o.into_world && same_path(report, *o.into_world))
        return make_error(ErrorCode::InvalidArgument, "--report " + shown + " would overwrite the world given to --into-world");
    std::error_code ec;
    if (std::filesystem::is_directory(report, ec)) return make_error(ErrorCode::InvalidArgument, "--report " + shown + " is a folder");
    if (std::filesystem::exists(report, ec) && !o.overwrite)
        return make_error(ErrorCode::AlreadyExists, "--report " + shown + " already exists; pass --overwrite to replace it");
    return {};
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
    o.target_version = a.target_version;
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

    std::optional<std::filesystem::path> report_path;
    if (!a.report_path.empty()) {
        auto rp = platform::path_from_utf8(a.report_path);
        if (!rp) {
            log::error("invalid --report path (not UTF-8)");
            return kExitError;
        }
        if (auto st = check_report_path(*rp, o); !st) {
            log::error("{}", st.error().message());
            return kExitError;
        }
        report_path = *rp;
    }

    auto summary = convert::convert(o);
    if (!summary) {
        log::error("{}", summary.error().describe());
        return summary.error().code() == ErrorCode::NotImplemented ? kExitNotImplemented : kExitError;
    }
    print_report(*summary);

    if (report_path) {
        const std::string json = convert::summary_to_json(*summary, o);
        const auto bytes = std::span<const u8>(reinterpret_cast<const u8*>(json.data()), json.size());
        if (!platform::write_file_atomically(*report_path, bytes, [](const std::filesystem::path&) -> Status { return {}; })) {
            log::error("could not write the report to {}", a.report_path);
            return kExitError;
        }
        std::puts(std::format("Report written to {}", a.report_path).c_str());
    }
    std::puts(std::format("Done: {} ({} -> {})", a.output, convert::describe(summary->source_version), summary->target_version).c_str());
    return kExitOk;
}

int run_versions() {
    const auto java = translate::JavaTargets::embedded();
    const auto bedrock = translate::BedrockTargets::embedded();
    if (!java || !bedrock) {
        log::error("{}", (!java ? java.error() : bedrock.error()).describe());
        return kExitError;
    }
    std::string line = "Java (structures, worlds):";
    for (const auto& t : (*java)->targets()) line += " " + t.name;
    std::puts(line.c_str());
    line = "Bedrock (.mcstructure, .mcworld):";
    for (const auto& t : (*bedrock)->targets()) line += " " + t.name;
    std::puts(line.c_str());
    std::puts("The first of each list is the default. Older versions get the blocks they have; the report lists the rest.");
    return kExitOk;
}

} // namespace bridger::cli

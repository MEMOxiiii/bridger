// bridger.exe — command-line interface.
//
// Subcommands: formats, info, convert (see commands/).

#include "commands/commands.hpp"

#include <bridger/core/log.hpp>
#include <bridger/core/result.hpp>
#include <bridger/formats/format.hpp>

#include <CLI/CLI.hpp>

#include <cstdio>
#include <format>
#include <string>

namespace {

using bridger::cli::kExitError;
using bridger::cli::kExitOk;

void print_formats() {
    std::puts(std::format("{:<12} {:<8} {:<13} {:<12} {}", "NAME", "EDITION", "EXTENSION", "READ", "WRITE").c_str());
    for (const auto& f : bridger::formats::all_formats()) {
        std::puts(std::format("{:<12} {:<8} {:<13} {:<12} {}", f.name, bridger::formats::to_string(f.edition),
                              f.extension.empty() ? "(folder)" : f.extension,
                              bridger::formats::to_string(f.read), bridger::formats::to_string(f.write))
                      .c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"Bridger — Minecraft Java/Bedrock format converter", "bridger"};
    argv = app.ensure_utf8(argv); // UTF-8 arguments on Windows regardless of code page
    app.set_version_flag("--version", std::string("bridger ") + BRIDGER_VERSION_STRING);
    app.require_subcommand(1);

    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "Verbose logging");

    auto* formats_cmd = app.add_subcommand("formats", "List known formats and their support status");

    std::string info_input;
    auto* info_cmd = app.add_subcommand("info", "Show information about a structure or world file");
    info_cmd->add_option("input", info_input, "Input file")->required();

    bridger::cli::ConvertArgs conv;
    auto* convert_cmd = app.add_subcommand("convert", "Convert between Java and Bedrock formats (see `bridger formats`)");
    convert_cmd->add_option("input", conv.input, "Input file")->required();
    convert_cmd->add_option("output", conv.output, "Output file, or a Java world folder (no extension)")->required();
    convert_cmd->add_option("--format", conv.format, "Output format name (overrides the extension)");
    convert_cmd->add_option("--report", conv.report_path, "Write the conversion report as JSON");
    convert_cmd->add_option("--fallback", conv.fallback, "Block for unmapped states: air|marker")->capture_default_str();
    convert_cmd->add_option("--air", conv.air, "Structure air: keep|replace existing blocks")->capture_default_str();
    convert_cmd->add_option("--place-at", conv.place_at, "World position x,y,z of the structure's corner");
    convert_cmd->add_option("--into-world", conv.into_world, "Place into a copy of an existing .mcworld (needs --place-at)");
    convert_cmd->add_option("--name", conv.level_name, "World name (default: input file name)");
    convert_cmd->add_flag("--clip", conv.clip, "Cut parts outside the build height (reported)");
    convert_cmd->add_flag("--shift-to-fit", conv.shift_to_fit, "Move vertically to fit the build height");
    convert_cmd->add_flag("--clear-entities", conv.clear_entities, "Remove entities in the target area (existing worlds)");
    convert_cmd->add_flag("--overwrite", conv.overwrite, "Replace the output file if it exists");
    convert_cmd->add_option("--threads", conv.threads, "Worker threads for whole worlds (default: all cores)")->check(CLI::Range(0, 1024));

    bridger::cli::RenderArgs rend;
    auto* render_cmd = app.add_subcommand("render", "Render a structure file to a PNG with your Minecraft textures");
    render_cmd->add_option("input", rend.input, "Structure file or world")->required();
    render_cmd->add_option("output", rend.output, "PNG file")->required();
    render_cmd->add_option("--size", rend.size, "Image size WxH")->capture_default_str();
    render_cmd->add_option("--yaw", rend.yaw, "Camera yaw in degrees")->capture_default_str();
    render_cmd->add_option("--pitch", rend.pitch, "Camera pitch in degrees")->capture_default_str();
    render_cmd->add_option("--zoom", rend.zoom, "Camera zoom (> 1 is closer)")->capture_default_str();
    render_cmd->add_flag("--map", rend.map, "Top-down map view");
    render_cmd->add_option("--radius", rend.radius, "Worlds: chunks to load around the spawn")->capture_default_str();
    render_cmd->add_option("--distant", rend.distant, "Worlds: show distant terrain (simplified) out to this many chunks");
    render_cmd->add_option("--lod-cache", rend.lod_cache, "Worlds: folder that caches distant terrain between runs");
    render_cmd->add_option("--slice", rend.slice, "Hide blocks at and above this y");
    render_cmd->add_option("--pack", rend.packs, "Resource pack (zip or folder); repeatable, last wins");
    render_cmd->add_option("--client-jar", rend.client_jar, "Minecraft client jar (default: from the launcher)");

    CLI11_PARSE(app, argc, argv);

    if (verbose) bridger::log::set_level(bridger::log::Level::Debug);

    if (formats_cmd->parsed()) {
        print_formats();
        return kExitOk;
    }
    if (info_cmd->parsed()) return bridger::cli::run_info(info_input);
    if (convert_cmd->parsed()) return bridger::cli::run_convert(conv);
    if (render_cmd->parsed()) return bridger::cli::run_render(rend);
    return kExitError;
}

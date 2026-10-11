// bridger.exe — command-line interface.
//
// Phase 0: only `--version` and `formats` work. `info` (Phase 2) and `convert`
// (Phase 4) are registered so their interface is visible, but they report
// "not implemented" instead of pretending to work.

#include <bridger/core/log.hpp>
#include <bridger/core/result.hpp>
#include <bridger/formats/format.hpp>

#include <CLI/CLI.hpp>

#include <cstdio>
#include <format>
#include <string>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitNotImplemented = 3;

int not_implemented(std::string_view command, std::string_view phase) {
    bridger::log::error("'{}' is not implemented yet (planned for {}).", command, phase);
    return kExitNotImplemented;
}

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

    std::string convert_input;
    std::string convert_output;
    auto* convert_cmd = app.add_subcommand("convert", "Convert between formats");
    convert_cmd->add_option("input", convert_input, "Input file")->required();
    convert_cmd->add_option("output", convert_output, "Output file")->required();

    CLI11_PARSE(app, argc, argv);

    if (verbose) bridger::log::set_level(bridger::log::Level::Debug);

    if (formats_cmd->parsed()) {
        print_formats();
        return kExitOk;
    }
    if (info_cmd->parsed()) return not_implemented("info", "Phase 2");
    if (convert_cmd->parsed()) return not_implemented("convert", "Phase 4");
    return kExitError;
}

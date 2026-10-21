#pragma once

// CLI subcommands. Each returns a process exit code.

#include <string>
#include <vector>

namespace bridger::cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;
inline constexpr int kExitNotImplemented = 3;

int run_info(const std::string& input_utf8);

struct ConvertArgs {
    std::string input;
    std::string output;
    std::string format;      // --format (overrides the output extension)
    std::string report_path; // --report
    std::string fallback = "air";
    std::string air = "keep";
    std::string place_at;
    std::string into_world;
    std::string level_name;
    std::string target_version; // --target-version
    std::string entities = "decorations"; // --entities decorations|all|none
    bool no_player = false;               // --no-player
    bool clip = false;
    bool shift_to_fit = false;
    bool clear_entities = false;
    bool overwrite = false;
    std::size_t threads = 0;
};

int run_convert(const ConvertArgs& args);
int run_versions();

struct RenderArgs {
    std::string input;
    std::string output;               // .png
    std::string size = "1600x900";
    float yaw = -35.0f;
    float pitch = -28.0f;
    float zoom = 1.0f;                // > 1 moves the camera closer
    int radius = 8;                   // worlds: chunks around the spawn
    int distant = 0;                  // worlds: distant terrain (LOD) out to this many chunks
    std::string lod_cache;            // worlds: folder for the distant terrain cache
    bool map = false;                 // top-down orthographic
    std::string slice;                // hide blocks at and above this y
    std::vector<std::string> packs;   // extra resource packs (highest priority last)
    std::string client_jar;           // default: the launcher's jar for the pinned version
};

int run_render(const RenderArgs& args);

} // namespace bridger::cli

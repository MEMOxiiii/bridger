#pragma once

// Data-safety policy for writing a structure into a world (prompt section 4.3,
// docs/safety.md). Defaults here ARE the documented defaults; changing any of
// them changes Bridger's safety guarantees and needs the maintainer's approval.

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>

#include <optional>
#include <string_view>

namespace bridger::convert {

// --air keep|replace
enum class AirMode : u8 {
    Keep,    // structure air leaves existing world blocks untouched (default)
    Replace, // structure air clears what was there
};

// default (fail) | --clip | --shift-to-fit
enum class HeightPolicy : u8 {
    Fail,       // structure outside build height -> clear error showing the overflow (default)
    Clip,       // cut it, report dropped blocks
    ShiftToFit, // move it vertically
};

struct PlacementPolicy {
    AirMode air = AirMode::Keep;
    HeightPolicy height = HeightPolicy::Fail;
    bool clear_entities = false;          // --clear-entities (removals are reported)
    std::optional<BlockPos> place_at;     // --place-at x,y,z; nullopt = new-world default placement
};

[[nodiscard]] Result<AirMode> parse_air_mode(std::string_view text);
[[nodiscard]] Result<BlockPos> parse_block_pos(std::string_view text); // "x,y,z"

[[nodiscard]] std::string_view to_string(AirMode mode) noexcept;
[[nodiscard]] std::string_view to_string(HeightPolicy policy) noexcept;

} // namespace bridger::convert

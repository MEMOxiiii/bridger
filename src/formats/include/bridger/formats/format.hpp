#pragma once

// Format identities and their support status. This table is the single source
// of truth for what Bridger can read and write; the README support matrix and
// `bridger --formats` must agree with it.

#include <bridger/core/types.hpp>

#include <optional>
#include <span>
#include <string_view>

namespace bridger::formats {

using bridger::Edition;

enum class FormatId : u8 {
    LegacySchematic, // .schematic (MCEdit, numeric ids)
    SpongeSchem,     // .schem (Sponge v1/v2/v3)
    Litematic,       // .litematic
    StructureNbt,    // .nbt (Java structure block)
    JavaWorld,       // Anvil world folder (level.dat + region/*.mca)
    McStructure,     // .mcstructure
    McWorld,         // .mcworld (zipped LevelDB world)
};

enum class Support : u8 {
    Planned,    // not started
    InProgress, // code exists but has not passed its acceptance criteria
    Supported,  // golden tests + fuzz target + benchmark + in-game check passed
};

struct FormatInfo {
    FormatId id;
    Edition edition;
    std::string_view name;      // short CLI name, e.g. "schem"
    std::string_view extension; // with dot; empty for folder formats
    std::string_view description;
    Support read;
    Support write;
};

[[nodiscard]] std::span<const FormatInfo> all_formats() noexcept;
[[nodiscard]] const FormatInfo& info(FormatId id) noexcept;

// Case-insensitive lookup by file extension (".schem") or CLI name ("schem").
[[nodiscard]] std::optional<FormatId> format_from_extension(std::string_view extension) noexcept;
[[nodiscard]] std::optional<FormatId> format_from_name(std::string_view name) noexcept;

[[nodiscard]] std::string_view to_string(Edition edition) noexcept;
[[nodiscard]] std::string_view to_string(Support support) noexcept;

} // namespace bridger::formats

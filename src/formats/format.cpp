#include <bridger/formats/format.hpp>

#include <algorithm>
#include <array>
#include <cassert>

namespace bridger::formats {
namespace {

using enum Support;

// Keep in sync with the README "Format support" matrix.
constexpr std::array kFormats{
    FormatInfo{FormatId::LegacySchematic, Edition::Java, "schematic", ".schematic",
               "MCEdit legacy schematic (numeric ids)", Planned, Planned},
    FormatInfo{FormatId::SpongeSchem, Edition::Java, "schem", ".schem", "Sponge schematic v1-v3", InProgress,
               Planned},
    FormatInfo{FormatId::Litematic, Edition::Java, "litematic", ".litematic", "Litematica schematic", Planned,
               Planned},
    FormatInfo{FormatId::StructureNbt, Edition::Java, "nbt", ".nbt", "Java structure block file", Planned,
               Planned},
    FormatInfo{FormatId::JavaWorld, Edition::Java, "java-world", "", "Java world (Anvil)", Planned, Planned},
    FormatInfo{FormatId::McStructure, Edition::Bedrock, "mcstructure", ".mcstructure",
               "Bedrock structure file", Planned, Planned},
    FormatInfo{FormatId::McWorld, Edition::Bedrock, "mcworld", ".mcworld", "Bedrock world (zipped LevelDB)",
               Planned, Planned},
};

constexpr char ascii_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

} // namespace

std::span<const FormatInfo> all_formats() noexcept { return kFormats; }

const FormatInfo& info(FormatId id) noexcept {
    const auto index = static_cast<usize>(id);
    assert(index < kFormats.size() && kFormats[index].id == id);
    return kFormats[index];
}

std::optional<FormatId> format_from_extension(std::string_view extension) noexcept {
    if (extension.empty()) return std::nullopt;
    if (extension.front() != '.') return format_from_name(extension);
    for (const auto& f : kFormats)
        if (!f.extension.empty() && iequals(f.extension, extension)) return f.id;
    return std::nullopt;
}

std::optional<FormatId> format_from_name(std::string_view name) noexcept {
    for (const auto& f : kFormats)
        if (iequals(f.name, name)) return f.id;
    return std::nullopt;
}

std::string_view to_string(Edition edition) noexcept {
    return edition == Edition::Java ? "Java" : "Bedrock";
}

std::string_view to_string(Support support) noexcept {
    switch (support) {
    case Planned: return "planned";
    case InProgress: return "in progress";
    case Supported: return "supported";
    }
    return "planned";
}

// Table order must match the enum so info() can index directly.
static_assert([] {
    for (usize i = 0; i < kFormats.size(); ++i)
        if (static_cast<usize>(kFormats[i].id) != i) return false;
    return true;
}());

} // namespace bridger::formats

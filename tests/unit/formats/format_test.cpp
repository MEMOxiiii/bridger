#include <bridger/formats/format.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace bridger::formats;

TEST_CASE("Formats are found by extension and name, case-insensitively", "[formats]") {
    CHECK(format_from_extension(".schem") == FormatId::SpongeSchem);
    CHECK(format_from_extension(".MCWORLD") == FormatId::McWorld);
    CHECK(format_from_extension("litematic") == FormatId::Litematic);
    CHECK(format_from_name("java-world") == FormatId::JavaWorld);
    CHECK_FALSE(format_from_extension(".txt"));
    CHECK_FALSE(format_from_extension(""));
}

TEST_CASE("Nothing claims support before it has passed acceptance", "[formats]") {
    // Update this test as formats ship. Supported requires golden tests, a fuzz target,
    // a benchmark and (for writers) the in-game checklist.
    for (const auto& f : all_formats()) {
        CAPTURE(f.name);
        const bool java_reader = f.id == FormatId::SpongeSchem || f.id == FormatId::StructureNbt ||
                                 f.id == FormatId::Litematic || f.id == FormatId::LegacySchematic ||
                                 f.id == FormatId::JavaWorld;
        CHECK(f.read == (java_reader ? Support::InProgress : Support::Planned));
        CHECK(f.write == (f.id == FormatId::McWorld || f.id == FormatId::McStructure ? Support::InProgress : Support::Planned));
        CHECK(&info(f.id) == &f);
    }
}

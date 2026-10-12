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
        // Phase 6: every format is read and written except legacy .schematic (read only).
        CHECK(f.read == Support::InProgress);
        CHECK(f.write == (f.id == FormatId::LegacySchematic ? Support::Planned : Support::InProgress));
        CHECK(&info(f.id) == &f);
    }
}

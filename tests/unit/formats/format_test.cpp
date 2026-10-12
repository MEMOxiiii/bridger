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
        // Release 0.3.0: every format passed golden tests, fuzzing, a benchmark and the in-game
        // checklist. Legacy .schematic is read only.
        CHECK(f.read == Support::Supported);
        CHECK(f.write == (f.id == FormatId::LegacySchematic ? Support::Planned : Support::Supported));
        CHECK(&info(f.id) == &f);
    }
}

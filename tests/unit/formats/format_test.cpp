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
    // Phase 0: no reader or writer exists yet. Update this test as formats ship.
    for (const auto& f : all_formats()) {
        CHECK(f.read == Support::Planned);
        CHECK(f.write == Support::Planned);
        CHECK(&info(f.id) == &f);
    }
}

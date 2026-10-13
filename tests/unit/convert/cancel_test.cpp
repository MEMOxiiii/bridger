// Cancelling a conversion never leaves output behind, even when the cancel arrives during the last
// step (after the output was written and validated, just before it is moved into place). Also: the
// report's "exact" flag accounts for warnings.

#include "../test_util.hpp"

#include <bridger/convert/pipeline.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>

using namespace bridger;
using namespace bridger::convert;

namespace {

// Nothing but the files that were there before (no output, no temp file, no temp folder).
bool only_entries(const std::filesystem::path& dir, usize expected) {
    usize n = 0;
    for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(dir)) ++n;
    return n == expected;
}

} // namespace

TEST_CASE("A cancel during the last step leaves no output, for every kind of output", "[convert][safety]") {
    const std::string out = GENERATE("castle.mcworld", "castle.mcstructure", "castle.schem", "castle.litematic", "castle.nbt");
    CAPTURE(out);
    test::TempDir dir;
    std::atomic<bool> cancel{false};
    ConvertOptions o;
    o.input = test::data_path("ingame/containers.schem");
    o.output = dir.path / out;
    o.cancel = &cancel;
    bool saw_saving = false;
    o.progress = [&](float, std::string_view step) {
        if (step == "Saving") {
            saw_saving = true;
            cancel = true; // the output is written and about to be validated and moved
        }
    };
    const auto r = bridger::convert::convert(o);
    CHECK(saw_saving);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Cancelled);
    CHECK_FALSE(std::filesystem::exists(o.output));
    CHECK(only_entries(dir.path, 0));
}

TEST_CASE("A cancel while packing a converted Java world leaves no .mcworld", "[convert][safety]") {
    test::TempDir dir;
    std::atomic<bool> cancel{false};
    ConvertOptions o;
    o.input = test::data_path("javaworld/basic");
    o.output = dir.path / "world.mcworld";
    o.cancel = &cancel;
    o.threads = 2;
    o.progress = [&](float, std::string_view step) {
        if (step.starts_with("Packing")) cancel = true; // after this, only the zip, its check and the move remain
    };
    const auto r = bridger::convert::convert(o);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Cancelled);
    CHECK(only_entries(dir.path, 0));
}

TEST_CASE("A cancel before the Java world folder is renamed leaves no folder", "[convert][safety]") {
    test::TempDir dir;
    // A Bedrock world to convert back to Java.
    ConvertOptions first;
    first.input = test::data_path("javaworld/basic");
    first.output = dir.path / "world.mcworld";
    REQUIRE(bridger::convert::convert(first));

    std::atomic<bool> cancel{false};
    ConvertOptions o;
    o.input = first.output;
    o.output = dir.path / "Java World";
    o.cancel = &cancel;
    o.progress = [&](float, std::string_view) { cancel = true; }; // as early as possible
    const auto r = bridger::convert::convert(o);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Cancelled);
    CHECK(only_entries(dir.path, 1)); // just the input world
}

TEST_CASE("The JSON report's exact flag is false when warnings record lost data", "[convert]") {
    ConvertOptions o;
    o.input = "in.schem";
    o.output = "out.mcworld";
    ConvertSummary s;
    CHECK(summary_to_json(s, o).find("\"exact\": true") != std::string::npos);
    s.warnings.push_back("12 partially generated chunks were skipped");
    const std::string j = summary_to_json(s, o);
    CHECK(j.find("\"exact\": false") != std::string::npos);
    CHECK(j.find("\"blocks_exact\": true") != std::string::npos);
}

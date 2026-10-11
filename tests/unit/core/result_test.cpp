#include <bridger/core/result.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace bridger;

namespace {

Result<int> parse_positive(int v) {
    if (v <= 0) return make_error(ErrorCode::InvalidArgument, "must be positive");
    return v;
}

Result<int> doubled(int v) {
    BRIDGER_TRY(const int p, parse_positive(v));
    return p * 2;
}

Status check(int v) {
    BRIDGER_TRY_STATUS(parse_positive(v));
    return {};
}

} // namespace

TEST_CASE("Result carries values and errors", "[core]") {
    REQUIRE(doubled(21).value() == 42);

    const auto bad = doubled(-1);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code() == ErrorCode::InvalidArgument);

    CHECK(check(1).has_value());
    CHECK_FALSE(check(0).has_value());
}

TEST_CASE("Error context is added once and described", "[core]") {
    Error e(ErrorCode::Malformed, "bad tag");
    e.with_file("a.nbt").with_offset(17).with_chunk({-3, 4});
    e.with_file("ignored.nbt"); // existing context is kept while propagating upward

    CHECK(e.context().file == "a.nbt");
    CHECK(e.describe() == "malformed data: bad tag (file 'a.nbt', offset 17, chunk -3,4)");
}

#include <bridger/convert/placement_policy.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace bridger;
using namespace bridger::convert;

TEST_CASE("Placement defaults match docs/safety.md", "[convert]") {
    const PlacementPolicy p;
    CHECK(p.air == AirMode::Keep);
    CHECK(p.height == HeightPolicy::Fail);
    CHECK_FALSE(p.clear_entities);
    CHECK_FALSE(p.place_at.has_value());
}

TEST_CASE("Parsing the air option", "[convert]") {
    CHECK(parse_air_mode("keep") == AirMode::Keep);
    CHECK(parse_air_mode("replace") == AirMode::Replace);
    CHECK_FALSE(parse_air_mode("Keep"));
    CHECK_FALSE(parse_air_mode(""));
}

TEST_CASE("Parsing the place-at option", "[convert]") {
    CHECK(parse_block_pos("1,2,3") == BlockPos{1, 2, 3});
    CHECK(parse_block_pos("-100,-64,2000000") == BlockPos{-100, -64, 2000000});
    CHECK_FALSE(parse_block_pos("1,2"));
    CHECK_FALSE(parse_block_pos("1,2,3,4"));
    CHECK_FALSE(parse_block_pos("1,,3"));
    CHECK_FALSE(parse_block_pos("1,2,x"));
    CHECK_FALSE(parse_block_pos("1, 2,3"));
    CHECK_FALSE(parse_block_pos("99999999999,0,0"));
}

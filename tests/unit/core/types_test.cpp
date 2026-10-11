#include <bridger/core/types.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace bridger;

TEST_CASE("Coordinate helpers floor toward negative infinity", "[core]") {
    STATIC_REQUIRE(floor_div16(0) == 0);
    STATIC_REQUIRE(floor_div16(15) == 0);
    STATIC_REQUIRE(floor_div16(16) == 1);
    STATIC_REQUIRE(floor_div16(-1) == -1);
    STATIC_REQUIRE(floor_div16(-16) == -1);
    STATIC_REQUIRE(floor_div16(-17) == -2);
    STATIC_REQUIRE(floor_mod16(-1) == 15);
    STATIC_REQUIRE(floor_mod16(17) == 1);

    CHECK(chunk_of({-1, 64, 33}) == ChunkPos{-1, 2});
    CHECK(section_of({-1, -64, 33}) == SectionPos{-1, -4, 2});
}

TEST_CASE("ChunkPos packing is unique for sign combinations", "[core]") {
    CHECK(ChunkPos{1, -1}.packed() != ChunkPos{-1, 1}.packed());
    CHECK(ChunkPos{0, 0}.packed() == 0);
}

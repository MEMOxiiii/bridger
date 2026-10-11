#include <bridger/platform/paths.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace bridger;

TEST_CASE("UTF-8 paths round-trip through the OS path type", "[platform]") {
    // Arabic + emoji + Latin: must survive regardless of the system ANSI code page.
    const std::string name = "\xD8\xAC\xD8\xB3\xD8\xB1 bridge \xF0\x9F\x8C\x89.mcworld";
    const auto path = platform::path_from_utf8(name);
    REQUIRE(path);
    const auto back = platform::path_to_utf8(*path);
    REQUIRE(back);
    CHECK(*back == name);
}

TEST_CASE("Invalid UTF-8 is rejected", "[platform]") {
    const auto path = platform::path_from_utf8("bad\xFF.schem");
    REQUIRE_FALSE(path);
    CHECK(path.error().code() == ErrorCode::InvalidArgument);
}

#ifdef _WIN32
TEST_CASE("Long paths get the \\\\?\\ prefix", "[platform]") {
    const auto local = platform::to_long_path(std::filesystem::path(LR"(C:\a\b\..\c.schem)"));
    REQUIRE(local);
    CHECK(local->native() == LR"(\\?\C:\a\c.schem)");

    const auto unc = platform::to_long_path(std::filesystem::path(LR"(\\server\share\x.schem)"));
    REQUIRE(unc);
    CHECK(unc->native() == LR"(\\?\UNC\server\share\x.schem)");

    // Already prefixed: unchanged.
    const auto same = platform::to_long_path(*local);
    REQUIRE(same);
    CHECK(*same == *local);
}
#endif

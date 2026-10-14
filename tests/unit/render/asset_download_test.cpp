// The client jar download's checks, without the network: which URLs are allowed, and reading
// Mojang's manifest and version files (shapes taken from the real files for 26.3).

#include <bridger/platform/net.hpp>
#include <bridger/render/asset_download.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace bridger;
using namespace bridger::render;

TEST_CASE("Only https downloads from Mojang's hosts are allowed", "[render][safety]") {
    CHECK(is_mojang_download_url("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"));
    CHECK(is_mojang_download_url("https://piston-data.mojang.com/v1/objects/e877b6a07acd633fb3bb475002175cec036e7b87/client.jar"));
    CHECK_FALSE(is_mojang_download_url("http://piston-data.mojang.com/v1/objects/x/client.jar"));      // not https
    CHECK_FALSE(is_mojang_download_url("https://piston-data.mojang.com.evil.example/client.jar"));     // lookalike host
    CHECK_FALSE(is_mojang_download_url("https://evil.example/piston-data.mojang.com/client.jar"));     // host in the path
    CHECK_FALSE(is_mojang_download_url("https://piston-data.mojang.com:8443/client.jar"));             // other port
    CHECK_FALSE(is_mojang_download_url("https://user@piston-data.mojang.com/client.jar"));             // credentials
    CHECK_FALSE(is_mojang_download_url("https://example.com/"));
}

TEST_CASE("The version manifest gives the version file's URL and SHA-1", "[render]") {
    const std::string manifest = R"({"latest":{"release":"26.3"},"versions":[
        {"id":"26.4-snapshot-4","type":"snapshot","url":"https://piston-meta.mojang.com/v1/packages/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/x.json","sha1":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
        {"id":"26.3","type":"release","url":"https://piston-meta.mojang.com/v1/packages/11f1aefe473eefba0af62e3286c33b692807b2eb/26.3.json","sha1":"11f1aefe473eefba0af62e3286c33b692807b2eb"}]})";
    const auto f = find_version_in_manifest(manifest, "26.3");
    REQUIRE(f);
    CHECK(f->sha1 == "11f1aefe473eefba0af62e3286c33b692807b2eb");
    CHECK(f->url.ends_with("/26.3.json"));

    CHECK(find_version_in_manifest(manifest, "1.0").error().code() == ErrorCode::FileNotFound);
    CHECK_FALSE(find_version_in_manifest("not json", "26.3"));
    // An entry pointing anywhere else is refused.
    CHECK_FALSE(find_version_in_manifest(R"({"versions":[{"id":"26.3","url":"https://evil.example/26.3.json","sha1":"11f1aefe473eefba0af62e3286c33b692807b2eb"}]})", "26.3"));
    CHECK_FALSE(find_version_in_manifest(R"({"versions":[{"id":"26.3","url":"https://piston-meta.mojang.com/x.json","sha1":"nothex"}]})", "26.3"));
}

TEST_CASE("The version file gives the client jar's URL, SHA-1 and size", "[render]") {
    const std::string version = R"({"id":"26.3","downloads":{
        "client":{"sha1":"e877b6a07acd633fb3bb475002175cec036e7b87","size":41483720,"url":"https://piston-data.mojang.com/v1/objects/e877b6a07acd633fb3bb475002175cec036e7b87/client.jar"},
        "server":{"sha1":"33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c","size":1,"url":"https://piston-data.mojang.com/v1/objects/33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c/server.jar"}}})";
    const auto f = client_download_of(version);
    REQUIRE(f);
    CHECK(f->size == 41483720);
    CHECK(f->sha1 == "e877b6a07acd633fb3bb475002175cec036e7b87");
    CHECK_FALSE(client_download_of(R"({"downloads":{}})"));
}

TEST_CASE("SHA-1 matches the standard test vectors", "[platform]") {
    const std::string abc = "abc";
    const auto h = platform::sha1_hex(std::span(reinterpret_cast<const u8*>(abc.data()), abc.size()));
    REQUIRE(h);
    CHECK(*h == "a9993e364706816aba3e25717850c26c9cd0d89d");
    const auto empty = platform::sha1_hex({});
    REQUIRE(empty);
    CHECK(*empty == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

// Real download from Mojang (hidden: needs the network; run with  bridger_unit_tests "[.network]").
TEST_CASE("The client jar downloads, verifies and opens", "[.network]") {
    float last = -1.0f;
    const auto path = download_client_jar("26.3", [&](float f, std::string_view) {
        CHECK(f >= last);
        last = f;
        return true;
    });
    REQUIRE(path);
    CHECK(std::filesystem::file_size(*path) == 41483720);
    CHECK(last == 1.0f);
}

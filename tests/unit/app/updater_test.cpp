// The desktop app's self-update (src/app/updater.cpp): release parsing, checksums, package checks and the file swap
// with rollback. No network access.

#include "../test_util.hpp"

#include <bridger/io/zip_archive.hpp>
#include <bridger/platform/net.hpp>

#include <updater.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>

using namespace bridger;
using namespace bridger::app::update;
namespace fs = std::filesystem;

namespace {

std::string release_json(std::string_view tag, std::string_view assets, bool prerelease = false) {
    return std::string(R"({"tag_name":")") + std::string(tag) + R"(","html_url":"https://github.com/MEMOxiiii/bridger/releases/tag/)" +
           std::string(tag) + R"(","draft":false,"prerelease":)" + (prerelease ? "true" : "false") + R"(,"body":"Notes","assets":[)" +
           std::string(assets) + "]}";
}

void write_text(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("Update versions parse and compare", "[app][update]") {
    const auto v = Version::parse("v1.10.2");
    REQUIRE(v);
    CHECK(v->str() == "1.10.2");
    CHECK(*Version::parse("1.0.0") < *Version::parse("1.0.1"));
    CHECK(*Version::parse("1.9.9") < *Version::parse("1.10.0"));
    CHECK_FALSE(Version::parse("1.2"));
    CHECK_FALSE(Version::parse("1.2.3-beta"));
    CHECK_FALSE(Version::parse("x1.2.3"));
}

TEST_CASE("The latest release JSON gives the zip, its digest or checksum file", "[app][update]") {
    const std::string digest(64, 'a');
    const std::string asset = "bridger-1.1.0" + std::string(bridger::app::update::kAssetSuffix); // this system's file
    const auto r = parse_release(release_json(
        "v1.1.0", R"({"name":")" + asset + R"(","size":123,"digest":"sha256:)" + digest +
                      R"(","browser_download_url":"https://github.com/MEMOxiiii/bridger/releases/download/v1.1.0/)" + asset + R"("},
                     {"name":")" + asset + R"(.sha256","browser_download_url":"https://github.com/MEMOxiiii/bridger/releases/download/v1.1.0/)" + asset + R"(.sha256"})"));
    REQUIRE(r);
    CHECK(r->version == *Version::parse("1.1.0"));
    CHECK(r->zip_size == 123);
    CHECK(r->sha256 == digest);
    CHECK(r->sha256_url.ends_with(asset + ".sha256"));
    CHECK(r->notes == "Notes");

    // Assets from anywhere else are ignored; without our zip the release is refused.
    CHECK_FALSE(parse_release(release_json(
        "v1.1.0", R"({"name":")" + asset + R"(","browser_download_url":"https://evil.example/)" + asset + R"("})")));
    CHECK_FALSE(parse_release(release_json("v1.1.0", "", /*prerelease=*/true)));
    CHECK_FALSE(parse_release("not json"));

    CHECK(parse_sha256_file(digest + "  bridger-1.1.0-windows-x64.zip\n") == digest);
    CHECK(parse_sha256_file(std::string(64, 'B')) == std::string(64, 'b'));
    CHECK_FALSE(parse_sha256_file("1234"));
    CHECK_FALSE(parse_sha256_file(std::string(64, 'g')));
}

TEST_CASE("Update packages are checked before anything is written", "[app][update]") {
    test::TempDir dir;
    const auto zip_of = [](std::vector<io::ZipEntry> entries) { return io::write_zip(entries); };
    // A good package: one folder with bridger-gui.exe.
    const auto good = zip_of({{"bridger-1.1.0-windows-x64/bridger-gui.exe", {1, 2, 3}},
                              {"bridger-1.1.0-windows-x64/licenses/a.txt", {'a'}}});
    REQUIRE(good);
    const auto root = extract_package(*good, dir.path / "s1");
    REQUIRE(root);
    CHECK(fs::is_regular_file(*root / "bridger-gui.exe"));
    CHECK(fs::is_regular_file(*root / "licenses" / "a.txt"));
    // Unsafe names, files outside the folder, a missing program: refused, nothing extracted.
    for (const auto& bad : {zip_of({{"bridger/../evil.exe", {1}}, {"bridger/bridger-gui.exe", {1}}}),
                            zip_of({{"C:/evil.exe", {1}}}),
                            zip_of({{"evil.exe", {1}}}),
                            zip_of({{"a/bridger-gui.exe", {1}}, {"b/x", {1}}}),
                            zip_of({{"bridger/readme.txt", {1}}})}) {
        if (!bad) continue; // the zip writer itself refuses some unsafe names
        const fs::path s = dir.path / "bad";
        CHECK_FALSE(extract_package(*bad, s));
        CHECK_FALSE(fs::exists(dir.path / "evil.exe"));
        fs::remove_all(s);
    }
}

TEST_CASE("Installing swaps the program's files and cleanup removes only the old ones", "[app][update]") {
    test::TempDir dir;
    const fs::path app = dir.path / "app";
    write_text(app / "bridger-gui.exe", "old gui");
    write_text(app / "bridger.exe", "old cli");
    write_text(app / "notes.old", "the user's own file");
    const fs::path pkg = dir.path / "pkg";
    write_text(pkg / "bridger-gui.exe", "new gui");
    write_text(pkg / "bridger.exe", "new cli");
    write_text(pkg / "licenses" / "new.txt", "license");

    REQUIRE(install(pkg, app));
    CHECK(read_text(app / "bridger-gui.exe") == "new gui");
    CHECK(read_text(app / "bridger.exe") == "new cli");
    CHECK(read_text(app / "licenses" / "new.txt") == "license");
    CHECK(read_text(app / "bridger-gui.exe.old") == "old gui");
    cleanup(app);
    CHECK_FALSE(fs::exists(app / "bridger-gui.exe.old"));
    CHECK_FALSE(fs::exists(app / "bridger.exe.old"));
    CHECK(fs::exists(app / "notes.old")); // not made by the update: kept
}

TEST_CASE("A failed install puts every file back", "[app][update]") {
    test::TempDir dir;
    const fs::path app = dir.path / "app";
    write_text(app / "a.exe", "old a");
    write_text(app / "b.exe", "old b");
    const fs::path pkg = dir.path / "pkg";
    write_text(pkg / "a.exe", "new a");
    write_text(pkg / "b.exe", "new b");
    // b.exe cannot be moved aside while it is open without delete sharing (Windows).
    std::ifstream lock(app / "b.exe");
    const auto r = install(pkg, app);
    lock.close();
#ifdef _WIN32
    REQUIRE_FALSE(r);
    CHECK(read_text(app / "a.exe") == "old a");
    CHECK(read_text(app / "b.exe") == "old b");
    CHECK_FALSE(fs::exists(app / "a.exe.old"));
#else
    CHECK(r);
#endif
}

// Hidden (needs the internet; run with: bridger_unit_tests "[.network]"): the real latest release is found, downloaded,
// checked against GitHub's SHA-256 and unpacked.
TEST_CASE("The latest real release downloads, verifies and unpacks", "[.network][app][update]") {
    const auto release = fetch_latest();
    REQUIRE(release);
    CHECK_FALSE(release->sha256.empty());
    if (!kSelfInstall) {
        // Linux and macOS only offer the release page; what they share with Windows is the download and the check.
        const auto bytes = platform::https_get(release->zip_url, 512ull << 20);
        REQUIRE(bytes);
        CHECK(platform::sha256_hex(*bytes).value() == release->sha256);
        return;
    }
    test::TempDir dir;
    const auto root = download_and_stage(*release, dir.path / "staging", {});
    REQUIRE(root);
    CHECK(fs::is_regular_file(*root / "bridger-gui.exe"));
    CHECK(fs::is_regular_file(*root / "bridger.exe"));
}

// Phase 0 smoke test: leveldb-mcpe builds, links, and round-trips data with the
// raw-zlib compressor that Bedrock uses (compressor id 4, per the fork's own
// mcpe_sample_setup.cpp). The real LevelDB store wrapper arrives in Phase 0.5/4.

#include <leveldb/db.h>
#include <leveldb/decompress_allocator.h>
#include <leveldb/zlib_compressor.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("bridger-leveldb-test-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

} // namespace

TEST_CASE("leveldb-mcpe round-trips binary keys with raw zlib compression", "[formats][leveldb]") {
    TempDir dir;

    auto raw_zlib = std::make_unique<leveldb::ZlibCompressorRaw>(-1);
    auto zlib = std::make_unique<leveldb::ZlibCompressor>();
    leveldb::Options options;
    options.create_if_missing = true;
    options.compressors[0] = raw_zlib.get();
    options.compressors[1] = zlib.get();

    // Bedrock-style binary key (contains zero bytes) and a compressible value.
    const std::string key("\x01\x00\x00\x00\xFF\xFF\xFF\xFF\x2F\x00", 10);
    const std::string value(4096, 'b');

    {
        leveldb::DB* raw_db = nullptr;
        const leveldb::Status open = leveldb::DB::Open(options, dir.path.string(), &raw_db);
        REQUIRE(open.ok());
        std::unique_ptr<leveldb::DB> db(raw_db);
        REQUIRE(db->Put(leveldb::WriteOptions(), key, value).ok());
        db->CompactRange(nullptr, nullptr); // force a compressed table file to be written
    }

    // Reopen from disk and read back.
    leveldb::DB* raw_db = nullptr;
    REQUIRE(leveldb::DB::Open(options, dir.path.string(), &raw_db).ok());
    std::unique_ptr<leveldb::DB> db(raw_db);

    leveldb::DecompressAllocator allocator;
    leveldb::ReadOptions read_options;
    read_options.decompress_allocator = &allocator;
    std::string got;
    REQUIRE(db->Get(read_options, key, &got).ok());
    CHECK(got == value);
}

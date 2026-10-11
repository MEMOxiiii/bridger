#include <bridger/io/compression.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <numeric>

using namespace bridger;
using namespace bridger::io;

namespace {

std::vector<u8> sample(usize n) {
    std::vector<u8> v(n);
    for (usize i = 0; i < n; ++i) v[i] = static_cast<u8>((i * 31) ^ (i >> 7));
    return v;
}

} // namespace

TEST_CASE("Compression round-trips in every format", "[io][compression]") {
    const Compression format = GENERATE(Compression::Gzip, Compression::Zlib, Compression::RawDeflate);
    const usize size = GENERATE(usize{0}, usize{1}, usize{1000}, usize{300'000});
    CAPTURE(to_string(format), size);
    const auto input = sample(size);
    const auto packed = compress(input, format);
    REQUIRE(packed);
    const auto unpacked = decompress(*packed, format);
    REQUIRE(unpacked);
    CHECK(*unpacked == input);
}

TEST_CASE("Gzip and zlib are detected; raw deflate is not", "[io][compression]") {
    const auto input = sample(100);
    CHECK(detect_compression(*compress(input, Compression::Gzip)) == Compression::Gzip);
    CHECK(detect_compression(*compress(input, Compression::Zlib)) == Compression::Zlib);
    CHECK(detect_compression(std::vector<u8>{0x0A, 0x00, 0x00}) == Compression::None); // NBT compound
    CHECK(detect_compression({}) == Compression::None);
}

TEST_CASE("Decompression output is capped (zip bomb guard)", "[io][compression]") {
    const std::vector<u8> zeros(8 * 1024 * 1024, 0); // compresses to a few KB
    const auto bomb = compress(zeros, Compression::Gzip, 9);
    REQUIRE(bomb);
    REQUIRE(bomb->size() < 64 * 1024);

    DecompressLimits limits;
    limits.max_output_bytes = 1024 * 1024;
    const auto r = decompress(*bomb, Compression::Gzip, limits);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::LimitExceeded);

    limits.max_output_bytes = zeros.size(); // exactly enough
    CHECK(decompress(*bomb, Compression::Gzip, limits));
}

TEST_CASE("Corrupt compressed data is an error, not a crash", "[io][compression]") {
    auto packed = *compress(sample(5000), Compression::Gzip);
    packed[packed.size() / 2] ^= 0xFF;
    const auto r = decompress(packed, Compression::Gzip);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Malformed);

    // Truncations
    const auto good = *compress(sample(5000), Compression::Zlib);
    for (usize len : {usize{0}, usize{1}, usize{2}, good.size() / 2, good.size() - 1})
        CHECK_FALSE(decompress(std::span<const u8>(good.data(), len), Compression::Zlib));
}

TEST_CASE("Concatenated gzip members are joined", "[io][compression]") {
    const auto a = sample(1000);
    const auto b = sample(77);
    auto joined = *compress(a, Compression::Gzip);
    const auto second = *compress(b, Compression::Gzip);
    joined.insert(joined.end(), second.begin(), second.end());
    const auto out = decompress(joined, Compression::Gzip);
    REQUIRE(out);
    auto expected = a;
    expected.insert(expected.end(), b.begin(), b.end());
    CHECK(*out == expected);

    joined.push_back(0x42); // trailing garbage
    CHECK_FALSE(decompress(joined, Compression::Gzip));
}

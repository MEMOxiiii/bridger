#include <bridger/io/varint.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <limits>
#include <vector>

using namespace bridger;

TEST_CASE("Varint encode/decode round-trip", "[io]") {
    const u32 value = GENERATE(0u, 1u, 127u, 128u, 255u, 300u, 16383u, 16384u, 0x0FFFFFFFu,
                               std::numeric_limits<u32>::max());
    std::vector<u8> buf;
    const usize written = io::encode_varint32(value, buf);
    CHECK(written == buf.size());
    CHECK(written == io::varint_size(value));

    const auto decoded = io::decode_varint32(buf);
    REQUIRE(decoded);
    CHECK(decoded->value == value);
    CHECK(decoded->length == written);
}

TEST_CASE("Varint known encodings", "[io]") {
    std::vector<u8> buf;
    io::encode_varint32(300, buf);
    CHECK(buf == std::vector<u8>{0xAC, 0x02});

    buf.clear();
    io::encode_varint64(std::numeric_limits<u64>::max(), buf);
    CHECK(buf.size() == io::kMaxVarint64Bytes);
    const auto d = io::decode_varint64(buf);
    REQUIRE(d);
    CHECK(d->value == std::numeric_limits<u64>::max());
}

TEST_CASE("Varint rejects truncated and overflowing input", "[io]") {
    const std::vector<u8> truncated{0x80, 0x80};
    const auto t = io::decode_varint32(truncated);
    REQUIRE_FALSE(t);
    CHECK(t.error().code() == ErrorCode::UnexpectedEof);

    const auto empty = io::decode_varint32({});
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code() == ErrorCode::UnexpectedEof);

    // 5th byte may only carry 4 bits for u32.
    const std::vector<u8> overflow{0xFF, 0xFF, 0xFF, 0xFF, 0x1F};
    const auto o = io::decode_varint32(overflow);
    REQUIRE_FALSE(o);
    CHECK(o.error().code() == ErrorCode::Malformed);

    const std::vector<u8> too_long{0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
    REQUIRE_FALSE(io::decode_varint32(too_long));
}

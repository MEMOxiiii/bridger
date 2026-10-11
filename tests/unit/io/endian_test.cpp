#include <bridger/io/endian.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace bridger;

TEST_CASE("Big- and little-endian loads", "[io]") {
    const std::array<u8, 8> bytes{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    CHECK(io::load_be<u16>(bytes.data()) == 0x0102);
    CHECK(io::load_le<u16>(bytes.data()) == 0x0201);
    CHECK(io::load_be<u32>(bytes.data()) == 0x01020304u);
    CHECK(io::load_le<u32>(bytes.data()) == 0x04030201u);
    CHECK(io::load_be<u64>(bytes.data()) == 0x0102030405060708ull);
    CHECK(io::load_le<i64>(bytes.data()) == 0x0807060504030201ll);
    // Unaligned access is fine.
    CHECK(io::load_be<u32>(bytes.data() + 1) == 0x02030405u);
}

TEST_CASE("Stores round-trip, including floats and negatives", "[io]") {
    std::array<u8, 8> buf{};
    io::store_be<f32>(buf.data(), 1.5f);
    CHECK(buf[0] == 0x3F); // IEEE-754 1.5f = 0x3FC00000
    CHECK(buf[1] == 0xC0);
    CHECK(io::load_be<f32>(buf.data()) == 1.5f);

    io::store_le<i32>(buf.data(), -2);
    CHECK(buf[0] == 0xFE);
    CHECK(buf[3] == 0xFF);
    CHECK(io::load_le<i32>(buf.data()) == -2);

    io::store_be<f64>(buf.data(), -0.25);
    CHECK(io::load_be<f64>(buf.data()) == -0.25);
}

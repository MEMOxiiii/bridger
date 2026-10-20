#include <bridger/platform/hash.hpp>
#include <bridger/platform/net.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace bridger;

namespace {

std::span<const u8> bytes(std::string_view s) { return {reinterpret_cast<const u8*>(s.data()), s.size()}; }

struct Vector {
    std::string message;
    std::string_view sha1, sha256;
};

// FIPS 180 example messages ("abc", the empty string, the 448-bit two-block message, a million 'a'), with the
// digests published by NIST (Cryptographic Standards and Guidelines, example values).
std::vector<Vector> nist_vectors() {
    return {
        {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {std::string(1000000, 'a'), "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
    };
}

} // namespace

TEST_CASE("Portable SHA-1 and SHA-256 match the NIST examples", "[platform]") {
    for (const Vector& v : nist_vectors()) {
        CAPTURE(v.message.size());
        CHECK(platform::to_hex(platform::sha1_digest(bytes(v.message))) == v.sha1);
        CHECK(platform::to_hex(platform::sha256_digest(bytes(v.message))) == v.sha256);
    }
    // Lengths around the padding boundary (55, 56, 63, 64 bytes): one or two final blocks, against the system
    // implementation (BCrypt on Windows; the same code elsewhere, where the NIST cases above are the check).
    for (const usize n : {55u, 56u, 63u, 64u, 65u, 119u, 120u}) {
        const std::string m(n, 'x');
        CAPTURE(n);
        CHECK(platform::to_hex(platform::sha1_digest(bytes(m))) == platform::sha1_hex(bytes(m)).value());
        CHECK(platform::to_hex(platform::sha256_digest(bytes(m))) == platform::sha256_hex(bytes(m)).value());
    }
}

TEST_CASE("Downloads refuse anything but https", "[platform]") {
    const auto r = platform::https_get("http://example.com/", 1024);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::InvalidArgument);
}

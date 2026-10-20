// SHA-1 and SHA-256 as specified in FIPS 180-4 (sections 5.1.1 padding, 6.1.2 SHA-1, 6.2.2 SHA-256).

#include <bridger/platform/hash.hpp>

#include <bit>

namespace bridger::platform {
namespace {

u32 load_be32(const u8* p) noexcept {
    return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

void store_be32(u8* p, u32 v) noexcept {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}

// Feeds `data` and the padding (0x80, zeros, the 64-bit big-endian bit length) to `block` in 64-byte blocks.
template <typename Block>
void for_each_block(std::span<const u8> data, Block&& block) noexcept {
    usize i = 0;
    for (; i + 64 <= data.size(); i += 64) block(data.data() + i);
    std::array<u8, 128> tail{};
    const usize rest = data.size() - i;
    for (usize k = 0; k < rest; ++k) tail[k] = data[i + k];
    tail[rest] = 0x80;
    const usize len = rest + 1 + 8 <= 64 ? 64 : 128;
    const u64 bits = static_cast<u64>(data.size()) * 8;
    for (usize k = 0; k < 8; ++k) tail[len - 1 - k] = static_cast<u8>(bits >> (8 * k));
    block(tail.data());
    if (len == 128) block(tail.data() + 64);
}

constexpr std::array<u32, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

} // namespace

std::array<u8, 20> sha1_digest(std::span<const u8> data) noexcept {
    std::array<u32, 5> h = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    for_each_block(data, [&h](const u8* p) {
        std::array<u32, 80> w{};
        for (usize t = 0; t < 16; ++t) w[t] = load_be32(p + 4 * t);
        for (usize t = 16; t < 80; ++t) w[t] = std::rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (usize t = 0; t < 80; ++t) {
            u32 f = b ^ c ^ d; // rounds 20-39 and 60-79
            if (t < 20) f = (b & c) | (~b & d);
            else if (t >= 40 && t < 60) f = (b & c) | (b & d) | (c & d);
            static constexpr std::array<u32, 4> kK = {0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6};
            const u32 temp = std::rotl(a, 5) + f + e + kK[t / 20] + w[t];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = temp;
        }
        const std::array<u32, 5> add = {a, b, c, d, e};
        for (usize i = 0; i < 5; ++i) h[i] += add[i];
    });
    std::array<u8, 20> out{};
    for (usize i = 0; i < 5; ++i) store_be32(out.data() + 4 * i, h[i]);
    return out;
}

std::array<u8, 32> sha256_digest(std::span<const u8> data) noexcept {
    std::array<u32, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for_each_block(data, [&h](const u8* p) {
        std::array<u32, 64> w{};
        for (usize t = 0; t < 16; ++t) w[t] = load_be32(p + 4 * t);
        for (usize t = 16; t < 64; ++t) {
            const u32 s0 = std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const u32 s1 = std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (usize t = 0; t < 64; ++t) {
            const u32 t1 = hh + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[t] + w[t];
            const u32 t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        const std::array<u32, 8> add = {a, b, c, d, e, f, g, hh};
        for (usize i = 0; i < 8; ++i) h[i] += add[i];
    });
    std::array<u8, 32> out{};
    for (usize i = 0; i < 8; ++i) store_be32(out.data() + 4 * i, h[i]);
    return out;
}

std::string to_hex(std::span<const u8> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (const u8 b : bytes) {
        s.push_back(kDigits[b >> 4]);
        s.push_back(kDigits[b & 15]);
    }
    return s;
}

} // namespace bridger::platform

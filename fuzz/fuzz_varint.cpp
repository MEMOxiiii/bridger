// Fuzz target: varint decoding must never read out of bounds or crash, and any
// value it decodes must re-encode to the same number of bytes or fewer.

#include <bridger/io/varint.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::span<const bridger::u8> rest(data, size);
    while (!rest.empty()) {
        const auto d = bridger::io::decode_varint64(rest);
        if (!d) break;
        if (d->length == 0 || d->length > rest.size()) std::abort();
        std::vector<bridger::u8> re;
        if (bridger::io::encode_varint64(d->value, re) > d->length) std::abort();
        rest = rest.subspan(d->length);
    }
    return 0;
}

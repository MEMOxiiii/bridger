// Fuzz target: decompression of untrusted data must fail cleanly (no crash, no
// unbounded allocation) and anything it decodes must re-compress losslessly.

#include <bridger/io/compression.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace bridger;
using namespace bridger::io;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    const Compression formats[] = {Compression::Gzip, Compression::Zlib, Compression::RawDeflate};
    const Compression format = formats[data[0] % 3];
    DecompressLimits limits;
    limits.max_output_bytes = 4 * 1024 * 1024;

    const auto out = decompress(std::span<const u8>(data + 1, size - 1), format, limits);
    if (!out) return 0;
    const auto again = compress(*out, format, 1);
    if (!again) std::abort();
    const auto back = decompress(*again, format, limits);
    if (!back || *back != *out) std::abort();
    return 0;
}

#include <bridger/io/compression.hpp>

#include <libdeflate.h>

#include <algorithm>
#include <memory>

namespace bridger::io {
namespace {

struct DecompressorDeleter {
    void operator()(libdeflate_decompressor* d) const noexcept { libdeflate_free_decompressor(d); }
};
struct CompressorDeleter {
    void operator()(libdeflate_compressor* c) const noexcept { libdeflate_free_compressor(c); }
};

// One decompressor per thread, reused across calls (allocation-free hot path).
libdeflate_decompressor* thread_decompressor() {
    thread_local std::unique_ptr<libdeflate_decompressor, DecompressorDeleter> d{libdeflate_alloc_decompressor()};
    return d.get();
}

// Decompresses one member/stream into out[out_pos...], growing out up to the limit.
// Returns bytes of input consumed.
Result<usize> decompress_one(libdeflate_decompressor* d, std::span<const u8> in, Compression format,
                             std::vector<u8>& out, usize out_pos, usize max_total) {
    usize capacity = std::min(max_total - out_pos, std::max<usize>(in.size() * 4, 64 * 1024));
    for (;;) {
        out.resize(out_pos + capacity);
        usize in_used = 0;
        usize out_used = 0;
        libdeflate_result r = LIBDEFLATE_BAD_DATA;
        switch (format) {
        case Compression::Gzip:
            r = libdeflate_gzip_decompress_ex(d, in.data(), in.size(), out.data() + out_pos, capacity, &in_used,
                                              &out_used);
            break;
        case Compression::Zlib:
            r = libdeflate_zlib_decompress_ex(d, in.data(), in.size(), out.data() + out_pos, capacity, &in_used,
                                              &out_used);
            break;
        case Compression::RawDeflate:
            r = libdeflate_deflate_decompress_ex(d, in.data(), in.size(), out.data() + out_pos, capacity, &in_used,
                                                 &out_used);
            break;
        case Compression::None: break;
        }
        if (r == LIBDEFLATE_SUCCESS) {
            out.resize(out_pos + out_used);
            return in_used;
        }
        if (r != LIBDEFLATE_INSUFFICIENT_SPACE)
            return make_error(ErrorCode::Malformed, std::string("corrupt ") + std::string(to_string(format)) + " data");
        const usize room = max_total - out_pos;
        if (capacity >= room)
            return make_error(ErrorCode::LimitExceeded,
                              "decompressed size exceeds the limit of " + std::to_string(max_total) + " bytes");
        capacity = std::min(room, capacity * 2);
    }
}

} // namespace

std::string_view to_string(Compression c) noexcept {
    switch (c) {
    case Compression::None: return "uncompressed";
    case Compression::Gzip: return "gzip";
    case Compression::Zlib: return "zlib";
    case Compression::RawDeflate: return "raw deflate";
    }
    return "unknown";
}

Compression detect_compression(std::span<const u8> data) noexcept {
    if (data.size() >= 2 && data[0] == 0x1F && data[1] == 0x8B) return Compression::Gzip;
    // zlib: CM = 8 (deflate), CINFO <= 7, and (CMF*256 + FLG) divisible by 31 (RFC 1950 §2.2).
    if (data.size() >= 2 && (data[0] & 0x0F) == 8 && (data[0] >> 4) <= 7 &&
        ((static_cast<unsigned>(data[0]) << 8) | data[1]) % 31 == 0)
        return Compression::Zlib;
    return Compression::None;
}

Result<std::vector<u8>> decompress(std::span<const u8> data, Compression format, const DecompressLimits& limits) {
    std::vector<u8> out;
    if (format == Compression::None) {
        if (data.size() > limits.max_output_bytes)
            return make_error(ErrorCode::LimitExceeded, "input exceeds the size limit");
        out.assign(data.begin(), data.end());
        return out;
    }
    libdeflate_decompressor* d = thread_decompressor();
    if (d == nullptr) return make_error(ErrorCode::IoError, "out of memory allocating a decompressor");

    usize in_pos = 0;
    do {
        BRIDGER_TRY(const usize used,
                    decompress_one(d, data.subspan(in_pos), format, out, out.size(), limits.max_output_bytes));
        in_pos += used;
        // Only gzip defines concatenated members; anything after a zlib/raw stream is ignored by design.
    } while (format == Compression::Gzip && in_pos < data.size() && data.size() - in_pos >= 2 &&
             data[in_pos] == 0x1F && data[in_pos + 1] == 0x8B);

    if (format == Compression::Gzip && in_pos != data.size())
        return make_error(ErrorCode::Malformed, "trailing garbage after gzip data", ErrorContext::at_offset(in_pos));
    return out;
}

Result<std::vector<u8>> compress(std::span<const u8> data, Compression format, int level) {
    std::vector<u8> out;
    if (format == Compression::None) {
        out.assign(data.begin(), data.end());
        return out;
    }
    if (level < 0 || level > 12) return make_error(ErrorCode::InvalidArgument, "compression level must be 0..12");
    std::unique_ptr<libdeflate_compressor, CompressorDeleter> c{libdeflate_alloc_compressor(level)};
    if (!c) return make_error(ErrorCode::IoError, "out of memory allocating a compressor");

    usize bound = 0;
    switch (format) {
    case Compression::Gzip: bound = libdeflate_gzip_compress_bound(c.get(), data.size()); break;
    case Compression::Zlib: bound = libdeflate_zlib_compress_bound(c.get(), data.size()); break;
    case Compression::RawDeflate: bound = libdeflate_deflate_compress_bound(c.get(), data.size()); break;
    case Compression::None: break;
    }
    out.resize(bound);
    usize n = 0;
    switch (format) {
    case Compression::Gzip: n = libdeflate_gzip_compress(c.get(), data.data(), data.size(), out.data(), bound); break;
    case Compression::Zlib: n = libdeflate_zlib_compress(c.get(), data.data(), data.size(), out.data(), bound); break;
    case Compression::RawDeflate:
        n = libdeflate_deflate_compress(c.get(), data.data(), data.size(), out.data(), bound);
        break;
    case Compression::None: break;
    }
    if (n == 0) return make_error(ErrorCode::IoError, "compression failed");
    out.resize(n);
    return out;
}

} // namespace bridger::io

#include <bridger/io/zip_archive.hpp>
#include <bridger/platform/paths.hpp>

#include <minizip-ng/mz.h>
#include <minizip-ng/mz_strm.h>
#include <minizip-ng/mz_strm_mem.h>
#include <minizip-ng/mz_zip.h>
#include <minizip-ng/mz_zip_rw.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace bridger::io {
namespace {

// RAII for minizip-ng handles.
struct MemStream {
    void* h = mz_stream_mem_create();
    ~MemStream() { mz_stream_mem_delete(&h); }
};
struct ZipWriter {
    void* h = mz_zip_writer_create();
    ~ZipWriter() { mz_zip_writer_delete(&h); }
};
struct ReaderHandle {
    void* h = mz_zip_reader_create();
    ~ReaderHandle() { mz_zip_reader_delete(&h); }
};

bool safe_entry_name(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\' || name.find(':') != std::string_view::npos)
        return false;
    usize start = 0;
    while (start <= name.size()) {
        usize end = name.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = name.size();
        if (name.substr(start, end - start) == "..") return false;
        start = end + 1;
    }
    return true;
}

} // namespace

Result<std::vector<u8>> write_zip(std::span<const ZipEntry> entries) {
    MemStream mem;
    ZipWriter zip;
    if (!mem.h || !zip.h) return make_error(ErrorCode::IoError, "zip: out of memory");
    mz_stream_mem_set_grow_size(mem.h, 1 << 20);
    if (mz_stream_open(mem.h, nullptr, MZ_OPEN_MODE_CREATE | MZ_OPEN_MODE_READWRITE) != MZ_OK ||
        mz_zip_writer_open(zip.h, mem.h, 0) != MZ_OK)
        return make_error(ErrorCode::IoError, "zip: cannot open writer");
    mz_zip_writer_set_compress_method(zip.h, MZ_COMPRESS_METHOD_DEFLATE);
    mz_zip_writer_set_compress_level(zip.h, 6);

    for (const ZipEntry& e : entries) {
        if (!safe_entry_name(e.name)) return make_error(ErrorCode::InvalidArgument, "zip: unsafe entry name " + e.name);
        if (e.data.size() > static_cast<usize>(std::numeric_limits<i32>::max()))
            return make_error(ErrorCode::LimitExceeded, "zip: entry too large: " + e.name);
        mz_zip_file info{};
        info.filename = e.name.c_str();
        info.modified_date = 315532800; // 1980-01-01, the zip epoch: reproducible output
        info.flag = MZ_ZIP_FLAG_UTF8;
        info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        info.uncompressed_size = static_cast<i64>(e.data.size());
        static const u8 kEmpty = 0; // minizip-ng rejects a null buffer even for empty entries
        const void* data = e.data.empty() ? &kEmpty : e.data.data();
        if (mz_zip_writer_add_buffer(zip.h, data, static_cast<i32>(e.data.size()), &info) != MZ_OK)
            return make_error(ErrorCode::IoError, "zip: cannot add " + e.name);
    }
    if (mz_zip_writer_close(zip.h) != MZ_OK) return make_error(ErrorCode::IoError, "zip: cannot finish archive");

    const void* buf = nullptr;
    i32 len = 0;
    mz_stream_mem_get_buffer(mem.h, &buf);
    mz_stream_mem_get_buffer_length(mem.h, &len);
    if (buf == nullptr || len < 0) return make_error(ErrorCode::IoError, "zip: no output");
    const auto* p = static_cast<const u8*>(buf);
    return std::vector<u8>(p, p + len);
}

Status write_zip_file_from_directory(const std::filesystem::path& dir, const std::filesystem::path& zip_path) {
    namespace fs = std::filesystem;
    std::vector<std::pair<std::string, fs::path>> files;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        BRIDGER_TRY(std::string rel, platform::path_to_utf8(fs::relative(e.path(), dir)));
        for (char& c : rel)
            if (c == '\\') c = '/';
        files.emplace_back(std::move(rel), e.path());
    }
    std::sort(files.begin(), files.end());

    BRIDGER_TRY(const std::string zip_utf8, platform::path_to_utf8(zip_path));
    ZipWriter zip;
    if (!zip.h) return make_error(ErrorCode::IoError, "zip: out of memory");
    if (mz_zip_writer_open_file(zip.h, zip_utf8.c_str(), 0, 0) != MZ_OK)
        return make_error(ErrorCode::IoError, "zip: cannot create " + zip_utf8);
    mz_zip_writer_set_compress_method(zip.h, MZ_COMPRESS_METHOD_DEFLATE);
    mz_zip_writer_set_compress_level(zip.h, 6);
    for (const auto& [name, path] : files) {
        std::ifstream in(path, std::ios::binary);
        const std::vector<u8> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (!in && !in.eof()) return make_error(ErrorCode::IoError, "zip: cannot read " + name);
        if (data.size() > static_cast<usize>(std::numeric_limits<i32>::max()))
            return make_error(ErrorCode::LimitExceeded, "zip: entry too large: " + name);
        mz_zip_file info{};
        info.filename = name.c_str();
        info.modified_date = 315532800;
        info.flag = MZ_ZIP_FLAG_UTF8;
        info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        info.uncompressed_size = static_cast<i64>(data.size());
        static const u8 kEmpty = 0;
        if (mz_zip_writer_add_buffer(zip.h, data.empty() ? &kEmpty : data.data(), static_cast<i32>(data.size()), &info) != MZ_OK)
            return make_error(ErrorCode::IoError, "zip: cannot add " + name);
    }
    if (mz_zip_writer_close(zip.h) != MZ_OK) return make_error(ErrorCode::IoError, "zip: cannot finish archive");
    return {};
}

Result<std::vector<ZipListing>> list_zip_file(const std::filesystem::path& zip_path, const ZipLimits& limits) {
    BRIDGER_TRY(const std::string zip_utf8, platform::path_to_utf8(zip_path));
    ReaderHandle zip;
    if (!zip.h) return make_error(ErrorCode::IoError, "zip: out of memory");
    if (mz_zip_reader_open_file(zip.h, zip_utf8.c_str()) != MZ_OK) return make_error(ErrorCode::Malformed, "zip: not a zip archive");
    std::vector<ZipListing> out;
    for (i32 r = mz_zip_reader_goto_first_entry(zip.h); r == MZ_OK; r = mz_zip_reader_goto_next_entry(zip.h)) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(zip.h, &info) != MZ_OK || info == nullptr || info->filename == nullptr)
            return make_error(ErrorCode::Malformed, "zip: unreadable entry");
        if (out.size() >= limits.max_entries) return make_error(ErrorCode::LimitExceeded, "zip: too many entries");
        out.push_back({info->filename, static_cast<u64>(std::max<i64>(0, info->uncompressed_size))});
    }
    mz_zip_reader_close(zip.h);
    return out;
}

Result<std::vector<ZipEntry>> read_zip(std::span<const u8> archive, const ZipLimits& limits) {
    if (archive.size() > static_cast<usize>(std::numeric_limits<i32>::max()))
        return make_error(ErrorCode::LimitExceeded, "zip: archive too large");
    ReaderHandle zip;
    if (!zip.h) return make_error(ErrorCode::IoError, "zip: out of memory");
    if (mz_zip_reader_open_buffer(zip.h, const_cast<u8*>(archive.data()), static_cast<i32>(archive.size()), 0) != MZ_OK)
        return make_error(ErrorCode::Malformed, "zip: not a zip archive");

    std::vector<ZipEntry> out;
    u64 total = 0;
    for (i32 r = mz_zip_reader_goto_first_entry(zip.h); r == MZ_OK; r = mz_zip_reader_goto_next_entry(zip.h)) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(zip.h, &info) != MZ_OK || info == nullptr || info->filename == nullptr)
            return make_error(ErrorCode::Malformed, "zip: unreadable entry");
        std::string name(info->filename);
        if (!name.empty() && name.back() == '/') continue; // directory entry
        if (!safe_entry_name(name)) return make_error(ErrorCode::Malformed, "zip: unsafe entry name " + name);
        if (out.size() >= limits.max_entries) return make_error(ErrorCode::LimitExceeded, "zip: too many entries");
        // The declared size is untrusted: it bounds the buffer, and extraction must produce exactly that much.
        if (info->uncompressed_size < 0 || static_cast<u64>(info->uncompressed_size) > limits.max_total_bytes - total)
            return make_error(ErrorCode::LimitExceeded, "zip: uncompressed size exceeds the limit");
        const i32 len = mz_zip_reader_entry_save_buffer_length(zip.h);
        if (len < 0 || len != info->uncompressed_size) return make_error(ErrorCode::Malformed, "zip: bad entry size " + name);
        ZipEntry e{std::move(name), std::vector<u8>(static_cast<usize>(len))};
        if (len > 0 && mz_zip_reader_entry_save_buffer(zip.h, e.data.data(), len) != MZ_OK)
            return make_error(ErrorCode::Malformed, "zip: corrupt entry " + e.name);
        total += static_cast<u64>(len);
        out.push_back(std::move(e));
    }
    mz_zip_reader_close(zip.h);
    return out;
}

ZipReader::ZipReader() = default;
ZipReader::ZipReader(ZipReader&& o) noexcept : handle_(o.handle_), limits_(o.limits_), index_(std::move(o.index_)) { o.handle_ = nullptr; }
ZipReader& ZipReader::operator=(ZipReader&& o) noexcept {
    if (this != &o) {
        if (handle_ != nullptr) {
            mz_zip_reader_close(handle_);
            mz_zip_reader_delete(&handle_);
        }
        handle_ = o.handle_;
        limits_ = o.limits_;
        index_ = std::move(o.index_);
        o.handle_ = nullptr;
    }
    return *this;
}
ZipReader::~ZipReader() {
    if (handle_ != nullptr) {
        mz_zip_reader_close(handle_);
        mz_zip_reader_delete(&handle_);
    }
}

Result<ZipReader> ZipReader::open(const std::filesystem::path& zip_path, const ZipLimits& limits) {
    BRIDGER_TRY(const std::string utf8, platform::path_to_utf8(zip_path));
    ZipReader r;
    r.limits_ = limits;
    r.handle_ = mz_zip_reader_create();
    if (r.handle_ == nullptr) return make_error(ErrorCode::IoError, "zip: out of memory");
    if (mz_zip_reader_open_file(r.handle_, utf8.c_str()) != MZ_OK) return make_error(ErrorCode::Malformed, "zip: cannot open " + utf8);
    void* zip = nullptr;
    if (mz_zip_reader_get_zip_handle(r.handle_, &zip) != MZ_OK || zip == nullptr) return make_error(ErrorCode::Malformed, "zip: cannot open " + utf8);
    for (i32 e = mz_zip_reader_goto_first_entry(r.handle_); e == MZ_OK; e = mz_zip_reader_goto_next_entry(r.handle_)) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(r.handle_, &info) != MZ_OK || info == nullptr || info->filename == nullptr)
            return make_error(ErrorCode::Malformed, "zip: unreadable entry in " + utf8);
        std::string name(info->filename);
        if (name.empty() || name.back() == '/') continue;
        r.index_.emplace(std::move(name), mz_zip_get_entry(zip)); // the first of duplicate names wins, as before
        if (r.index_.size() > limits.max_entries) return make_error(ErrorCode::LimitExceeded, "zip: too many entries in " + utf8);
    }
    return r;
}

Result<std::optional<std::vector<u8>>> ZipReader::read(std::string_view name) {
    if (!safe_entry_name(name)) return make_error(ErrorCode::InvalidArgument, "zip: unsafe entry name");
    const std::string n(name);
    const auto it = index_.find(n);
    if (it == index_.end()) return std::optional<std::vector<u8>>{};
    void* zip = nullptr;
    if (mz_zip_reader_get_zip_handle(handle_, &zip) != MZ_OK || zip == nullptr || mz_zip_goto_entry(zip, it->second) != MZ_OK)
        return make_error(ErrorCode::Malformed, "zip: unreadable entry " + n);
    mz_zip_file* info = nullptr;
    if (mz_zip_entry_get_info(zip, &info) != MZ_OK || info == nullptr) return make_error(ErrorCode::Malformed, "zip: unreadable entry " + n);
    if (info->uncompressed_size < 0 || static_cast<u64>(info->uncompressed_size) > limits_.max_total_bytes ||
        info->uncompressed_size > std::numeric_limits<i32>::max())
        return make_error(ErrorCode::LimitExceeded, "zip: entry too large " + n);
    std::vector<u8> out(static_cast<usize>(info->uncompressed_size));
    if (mz_zip_entry_read_open(zip, 0, nullptr) != MZ_OK) return make_error(ErrorCode::Malformed, "zip: corrupt entry " + n);
    usize done = 0;
    while (done < out.size()) {
        const i32 got = mz_zip_entry_read(zip, out.data() + done, static_cast<i32>(out.size() - done));
        if (got <= 0) break;
        done += static_cast<usize>(got);
    }
    // Closing checks the CRC once the whole entry was read.
    if (mz_zip_entry_close(zip) != MZ_OK || done != out.size()) return make_error(ErrorCode::Malformed, "zip: corrupt entry " + n);
    return std::optional<std::vector<u8>>{std::move(out)};
}

Result<std::vector<std::string>> ZipReader::names() {
    std::vector<std::string> out;
    out.reserve(index_.size());
    for (const auto& [name, pos] : index_) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace bridger::io

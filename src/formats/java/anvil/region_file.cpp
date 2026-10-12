#include <bridger/formats/java/anvil/region_file.hpp>
#include <bridger/io/endian.hpp>
#include <bridger/platform/paths.hpp>

#include <regex>
#include <string>

namespace bridger::formats::java {

namespace {
constexpr usize kSector = 4096;
}

Result<RegionFile> RegionFile::open(const std::filesystem::path& path) {
    BRIDGER_TRY(const std::string name, platform::path_to_utf8(path.filename()));
    static const std::regex kName(R"(r\.(-?\d+)\.(-?\d+)\.mca)");
    std::smatch m;
    if (!std::regex_match(name, m, kName)) return make_error(ErrorCode::InvalidArgument, "not a region file name: " + name);
    RegionFile r;
    r.rx_ = std::stoi(m[1].str());
    r.rz_ = std::stoi(m[2].str());
    BRIDGER_TRY(r.file_, platform::MappedFile::open(path));
    r.path_ = path;
    // An empty file is a region with no chunks; anything else needs both header tables.
    if (r.file_.size() != 0 && r.file_.size() < 2 * kSector)
        return make_error(ErrorCode::Malformed, "region file shorter than its header: " + name);
    return r;
}

bool RegionFile::has_chunk(u32 lx, u32 lz) const noexcept {
    if (file_.size() < 2 * kSector || lx > 31 || lz > 31) return false;
    const u8* entry = file_.data().data() + 4 * (lx + lz * 32);
    return (entry[0] | entry[1] | entry[2]) != 0 && entry[3] != 0;
}

Result<std::optional<std::vector<u8>>> RegionFile::read_chunk(u32 lx, u32 lz, const io::DecompressLimits& limits) const {
    if (!has_chunk(lx, lz)) return std::optional<std::vector<u8>>{};
    const auto data = file_.data();
    const u8* entry = data.data() + 4 * (lx + lz * 32);
    const usize offset = ((usize{entry[0]} << 16) | (usize{entry[1]} << 8) | entry[2]) * kSector;
    const usize sectors = entry[3];
    auto bad = [&](const std::string& why) {
        return make_error(ErrorCode::Malformed, "region " + std::to_string(rx_) + "," + std::to_string(rz_) + " chunk " +
                                                    std::to_string(lx) + "," + std::to_string(lz) + ": " + why);
    };
    if (offset < 2 * kSector || offset + 5 > data.size()) return bad("location outside the file");
    const i32 length = io::load_be<i32>(data.data() + offset);
    if (length < 1) return bad("invalid length");
    const u8 kind = data[offset + 4];
    std::span<const u8> payload;
    std::vector<u8> external;
    if (kind & 0x80) {
        // Payload in c.<x>.<z>.mcc next to the region file (absolute chunk coordinates).
        const i32 cx = rx_ * 32 + static_cast<i32>(lx), cz = rz_ * 32 + static_cast<i32>(lz);
        BRIDGER_TRY(const std::filesystem::path ext,
                    platform::path_from_utf8("c." + std::to_string(cx) + "." + std::to_string(cz) + ".mcc"));
        BRIDGER_TRY(const platform::MappedFile f, platform::MappedFile::open(path_.parent_path() / ext));
        external.assign(f.data().begin(), f.data().end());
        payload = external;
    } else {
        if (static_cast<usize>(length) - 1 > sectors * kSector || offset + 4 + static_cast<usize>(length) > data.size())
            return bad("payload longer than its sectors");
        payload = data.subspan(offset + 5, static_cast<usize>(length) - 1);
    }
    io::Compression c;
    switch (kind & 0x7F) {
    case 1: c = io::Compression::Gzip; break;
    case 2: c = io::Compression::Zlib; break;
    case 3: c = io::Compression::None; break;
    case 4: return make_error(ErrorCode::UnsupportedFormat, "chunk uses LZ4 compression (not supported)");
    case 127: return make_error(ErrorCode::UnsupportedFormat, "chunk uses a custom server compression");
    default: return bad("unknown compression type " + std::to_string(kind & 0x7F));
    }
    BRIDGER_TRY(std::vector<u8> out, io::decompress(payload, c, limits));
    return std::optional<std::vector<u8>>{std::move(out)};
}

} // namespace bridger::formats::java

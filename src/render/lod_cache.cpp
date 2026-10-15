// Distant terrain cache; file layout in lod_cache.hpp.

#include <bridger/io/endian.hpp>
#include <bridger/platform/atomic_replace.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/render/lod_cache.hpp>

#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bridger::render {
namespace fs = std::filesystem;

namespace {

constexpr std::array<u8, 4> kMagic{'B', 'L', 'O', 'D'};
constexpr u32 kVersion = 1;
constexpr usize kHeader = 4 + 4 + 4 + 4 + 128;
constexpr usize kEntry = 256 * 2 + 256 * 4;

u64 fnv1a(std::span<const u8> d) {
    u64 h = 0xcbf29ce484222325ull;
    for (const u8 b : d) h = (h ^ b) * 0x100000001b3ull;
    return h;
}

struct Region {
    std::vector<std::optional<ColumnSummary>> slots = std::vector<std::optional<ColumnSummary>>(1024); // 1.5 MB: heap, never the stack
};

u64 region_key(i32 rx, i32 rz) { return (static_cast<u64>(static_cast<u32>(rx)) << 32) | static_cast<u32>(rz); }

std::optional<Region> decode(std::span<const u8> d, i32 rx, i32 rz) {
    if (d.size() < kHeader + 8 || std::memcmp(d.data(), kMagic.data(), 4) != 0) return std::nullopt;
    if (io::load_le<u32>(d.data() + 4) != kVersion || io::load_le<i32>(d.data() + 8) != rx || io::load_le<i32>(d.data() + 12) != rz)
        return std::nullopt;
    if (io::load_le<u64>(d.data() + d.size() - 8) != fnv1a(d.first(d.size() - 8))) return std::nullopt;
    Region r;
    usize p = kHeader;
    for (usize slot = 0; slot < 1024; ++slot) {
        if (!((d[16 + slot / 8] >> (slot % 8)) & 1)) continue;
        if (p + kEntry > d.size() - 8) return std::nullopt;
        ColumnSummary s;
        for (usize i = 0; i < 256; ++i) s.top[i] = io::load_le<i16>(d.data() + p + i * 2);
        for (usize i = 0; i < 256; ++i) s.color[i] = io::load_le<u32>(d.data() + p + 512 + i * 4);
        r.slots[slot] = s;
        p += kEntry;
    }
    if (p != d.size() - 8) return std::nullopt;
    return r;
}

std::vector<u8> encode(const Region& r, i32 rx, i32 rz) {
    std::vector<u8> out(kHeader);
    std::memcpy(out.data(), kMagic.data(), 4);
    io::store_le<u32>(out.data() + 4, kVersion);
    io::store_le<i32>(out.data() + 8, rx);
    io::store_le<i32>(out.data() + 12, rz);
    for (usize slot = 0; slot < 1024; ++slot) {
        const auto& s = r.slots[slot];
        if (!s) continue;
        out[16 + slot / 8] = static_cast<u8>(out[16 + slot / 8] | (1u << (slot % 8)));
        const usize p = out.size();
        out.resize(p + kEntry);
        for (usize i = 0; i < 256; ++i) io::store_le<i16>(out.data() + p + i * 2, s->top[i]);
        for (usize i = 0; i < 256; ++i) io::store_le<u32>(out.data() + p + 512 + i * 4, s->color[i]);
    }
    const u64 h = fnv1a(out);
    out.resize(out.size() + 8);
    io::store_le<u64>(out.data() + out.size() - 8, h);
    return out;
}

} // namespace

struct LodCache::Impl {
    fs::path dir;
    std::mutex lock;
    std::unordered_map<u64, Region> regions; // loaded (or created) regions
    std::unordered_set<u64> dirty;

    fs::path file(i32 rx, i32 rz) const { return dir / std::format("r.{}.{}.blod", rx, rz); }

    Region& region(i32 rx, i32 rz) {
        const u64 k = region_key(rx, rz);
        if (const auto it = regions.find(k); it != regions.end()) return it->second;
        Region r;
        std::error_code ec;
        if (fs::is_regular_file(file(rx, rz), ec))
            if (auto f = platform::MappedFile::open(file(rx, rz)))
                if (auto d = decode(f->data(), rx, rz)) r = std::move(*d);
        return regions.emplace(k, std::move(r)).first->second;
    }
};

LodCache::LodCache(fs::path dir) : impl_(std::make_unique<Impl>()) { impl_->dir = std::move(dir); }
LodCache::~LodCache() = default;

const fs::path& LodCache::dir() const noexcept { return impl_->dir; }

std::optional<ColumnSummary> LodCache::get(ChunkPos pos) {
    std::lock_guard g(impl_->lock);
    return impl_->region(pos.x >> 5, pos.z >> 5).slots[static_cast<usize>((pos.x & 31) + (pos.z & 31) * 32)];
}

void LodCache::put(ChunkPos pos, const ColumnSummary& summary) {
    std::lock_guard g(impl_->lock);
    auto& slot = impl_->region(pos.x >> 5, pos.z >> 5).slots[static_cast<usize>((pos.x & 31) + (pos.z & 31) * 32)];
    if (slot && *slot == summary) return;
    slot = summary;
    impl_->dirty.insert(region_key(pos.x >> 5, pos.z >> 5));
}

Status LodCache::flush() {
    std::lock_guard g(impl_->lock);
    if (impl_->dirty.empty()) return {};
    std::error_code ec;
    fs::create_directories(impl_->dir, ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot create the distant terrain cache: " + ec.message());
    for (const u64 k : impl_->dirty) {
        const i32 rx = static_cast<i32>(static_cast<u32>(k >> 32)), rz = static_cast<i32>(static_cast<u32>(k));
        const std::vector<u8> bytes = encode(impl_->regions.at(k), rx, rz);
        BRIDGER_TRY_STATUS(platform::write_file_atomically(impl_->file(rx, rz), bytes, [rx, rz](const fs::path& p) -> Status {
            BRIDGER_TRY(const platform::MappedFile f, platform::MappedFile::open(p));
            if (!decode(f.data(), rx, rz)) return make_error(ErrorCode::ValidationFailed, "distant terrain cache file did not re-read");
            return {};
        }));
    }
    impl_->dirty.clear();
    return {};
}

} // namespace bridger::render

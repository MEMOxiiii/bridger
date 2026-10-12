#include <bridger/formats/bedrock/mcworld/leveldb_store.hpp>
#include <bridger/platform/paths.hpp>

#include <leveldb/db.h>
#include <leveldb/decompress_allocator.h>
#include <leveldb/env.h>
#include <leveldb/write_batch.h>
#include <leveldb/zlib_compressor.h>

namespace bridger::formats::bedrock {
namespace {

class NullLogger final : public leveldb::Logger {
public:
    void Logv(const char*, va_list) override {}
};

tl::unexpected<Error> leveldb_error(const leveldb::Status& s, std::string_view what) {
    return make_error(ErrorCode::IoError, std::string("LevelDB ") + std::string(what) + ": " + s.ToString());
}

} // namespace

struct LevelDbStore::Impl {
    // Same compressor setup as Mojang's mcpe_sample_setup.cpp in the leveldb-mcpe repository.
    leveldb::ZlibCompressorRaw raw{-1};
    leveldb::ZlibCompressor zlib;
    NullLogger logger;
    leveldb::DecompressAllocator allocator;
    std::unique_ptr<leveldb::DB> db;
};

LevelDbStore::LevelDbStore() : impl_(std::make_unique<Impl>()) {}
LevelDbStore::LevelDbStore(LevelDbStore&&) noexcept = default;
LevelDbStore& LevelDbStore::operator=(LevelDbStore&&) noexcept = default;
LevelDbStore::~LevelDbStore() = default;

Result<LevelDbStore> LevelDbStore::open(const std::filesystem::path& dir, bool create_new) {
    LevelDbStore store;
    leveldb::Options options;
    options.create_if_missing = create_new;
    options.error_if_exists = create_new;
    options.compressors[0] = &store.impl_->raw;
    options.compressors[1] = &store.impl_->zlib;
    options.info_log = &store.impl_->logger;
    options.write_buffer_size = 4 * 1024 * 1024;
    // leveldb-mcpe's Windows Env takes narrow paths; pass UTF-8 (it converts internally).
    BRIDGER_TRY(const std::string path, platform::path_to_utf8(dir));
    leveldb::DB* raw = nullptr;
    const leveldb::Status s = leveldb::DB::Open(options, path, &raw);
    if (!s.ok()) return leveldb_error(s, "open");
    store.impl_->db.reset(raw);
    return store;
}

Status LevelDbStore::put(std::string_view key, std::span<const u8> value) {
    const leveldb::Status s = impl_->db->Put(leveldb::WriteOptions(), leveldb::Slice(key.data(), key.size()),
                                             leveldb::Slice(reinterpret_cast<const char*>(value.data()), value.size()));
    if (!s.ok()) return leveldb_error(s, "put");
    return {};
}

Result<std::optional<std::vector<u8>>> LevelDbStore::get(std::string_view key) const {
    leveldb::ReadOptions ro;
    ro.decompress_allocator = &impl_->allocator;
    std::string value;
    const leveldb::Status s = impl_->db->Get(ro, leveldb::Slice(key.data(), key.size()), &value);
    if (s.IsNotFound()) return std::optional<std::vector<u8>>{};
    if (!s.ok()) return leveldb_error(s, "get");
    return std::optional<std::vector<u8>>{std::vector<u8>(value.begin(), value.end())};
}

Status LevelDbStore::for_each(const std::function<void(std::string_view, std::span<const u8>)>& fn) const {
    leveldb::ReadOptions ro;
    ro.decompress_allocator = &impl_->allocator;
    std::unique_ptr<leveldb::Iterator> it(impl_->db->NewIterator(ro));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const leveldb::Slice k = it->key();
        const leveldb::Slice v = it->value();
        fn(std::string_view(k.data(), k.size()), std::span<const u8>(reinterpret_cast<const u8*>(v.data()), v.size()));
    }
    if (!it->status().ok()) return leveldb_error(it->status(), "iterate");
    return {};
}

Status LevelDbStore::compact() {
    impl_->db->CompactRange(nullptr, nullptr);
    return {};
}

} // namespace bridger::formats::bedrock

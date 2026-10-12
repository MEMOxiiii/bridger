#include <bridger/render/assets.hpp>

#include <bridger/io/zip_archive.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>

#include <mutex>

namespace bridger::render {
namespace fs = std::filesystem;

namespace {

struct Pack {
    fs::path path;
    bool folder = false;
    std::optional<io::ZipReader> zip;
    std::unique_ptr<std::mutex> lock = std::make_unique<std::mutex>(); // ZipReader is single-threaded
};

} // namespace

struct AssetSource::Impl {
    std::vector<Pack> packs; // lowest priority first
};

AssetSource::AssetSource() : impl_(std::make_unique<Impl>()) {}
AssetSource::AssetSource(AssetSource&&) noexcept = default;
AssetSource& AssetSource::operator=(AssetSource&&) noexcept = default;
AssetSource::~AssetSource() = default;

usize AssetSource::pack_count() const noexcept { return impl_->packs.size(); }

Result<AssetSource> AssetSource::open(const std::vector<fs::path>& packs) {
    AssetSource s;
    for (const fs::path& p : packs) {
        std::error_code ec;
        Pack pack;
        pack.path = p;
        if (fs::is_directory(p, ec)) {
            pack.folder = true;
        } else {
            BRIDGER_TRY(io::ZipReader zip, io::ZipReader::open(p));
            pack.zip.emplace(std::move(zip));
        }
        s.impl_->packs.push_back(std::move(pack));
    }
    return s;
}

std::optional<std::vector<u8>> AssetSource::read(std::string_view path) const {
    for (auto it = impl_->packs.rbegin(); it != impl_->packs.rend(); ++it) {
        Pack& p = *it;
        if (p.folder) {
            auto rel = platform::path_from_utf8(path);
            if (!rel) continue;
            std::error_code ec;
            const fs::path full = p.path / *rel;
            if (!fs::is_regular_file(full, ec)) continue;
            auto f = platform::MappedFile::open(full);
            if (!f) continue;
            return std::vector<u8>(f->data().begin(), f->data().end());
        }
        std::lock_guard g(*p.lock);
        auto r = p.zip->read(path);
        if (r && *r) return std::move(**r);
    }
    return std::nullopt;
}

std::optional<fs::path> AssetSource::find_client_jar(std::string_view version) {
    const auto app_data = platform::roaming_app_data_dir();
    if (!app_data) return std::nullopt;
    auto v = platform::path_from_utf8(version);
    if (!v) return std::nullopt;
    fs::path jar = *app_data / ".minecraft" / "versions" / *v / *v;
    jar += ".jar";
    std::error_code ec;
    if (!fs::is_regular_file(jar, ec)) return std::nullopt;
    return jar;
}

} // namespace bridger::render

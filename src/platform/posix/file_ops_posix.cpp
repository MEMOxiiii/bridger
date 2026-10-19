// POSIX (Linux, macOS) implementation of FileOps and MappedFile. Durability: fsync on the file, and on the parent
// directory after a rename (POSIX makes a rename durable only once its directory is synced). docs/safety.md.

#include <bridger/platform/atomic_replace.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>

#include <cerrno>
#include <cstdio> // renameat2 (Linux), renamex_np (macOS)
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace bridger::platform {
namespace {

ErrorCode map_code(int code) {
    switch (code) {
    case ENOENT:
    case ENOTDIR: return ErrorCode::FileNotFound;
    case EEXIST: return ErrorCode::AlreadyExists;
    case EACCES:
    case EPERM:
    case EBUSY: return ErrorCode::AccessDenied;
    default: return ErrorCode::IoError;
    }
}

tl::unexpected<Error> last_error(std::string_view what, const std::filesystem::path& path) {
    const int code = errno;
    return make_error(map_code(code), std::string(what) + ": " + std::strerror(code) + " (errno " + std::to_string(code) + ")",
                      ErrorContext{path.string(), std::nullopt, std::nullopt});
}

int fd_of(FileOps::Handle h) { return static_cast<int>(reinterpret_cast<std::intptr_t>(h)) - 1; }
FileOps::Handle handle_of(int fd) { return reinterpret_cast<FileOps::Handle>(static_cast<std::intptr_t>(fd) + 1); } // never null

// fsync of the directory that holds `path`, so that a rename into it survives a crash.
Status sync_parent(const std::filesystem::path& path) {
    std::filesystem::path dir = path.parent_path();
    if (dir.empty()) dir = ".";
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return last_error("cannot open folder to sync", dir);
    const int rc = ::fsync(fd);
    ::close(fd);
    if (rc != 0) return last_error("folder sync failed", dir);
    return {};
}

class PosixFileOps final : public FileOps {
public:
    Result<Handle> create_new(const std::filesystem::path& path) override {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) return last_error("cannot create file", path);
        return handle_of(fd);
    }

    Status write(Handle handle, std::span<const u8> bytes) override {
        while (!bytes.empty()) {
            const ssize_t n = ::write(fd_of(handle), bytes.data(), bytes.size());
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return last_error("write failed", {});
            bytes = bytes.subspan(static_cast<usize>(n));
        }
        return {};
    }

    Status flush(Handle handle) override {
        if (::fsync(fd_of(handle)) != 0) return last_error("flush failed", {});
        return {};
    }

    Status flush_path(const std::filesystem::path& path) override {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return last_error("cannot open file to flush", path);
        const int rc = ::fsync(fd);
        ::close(fd);
        if (rc != 0) return last_error("flush failed", path);
        return {};
    }

    Status close(Handle handle) override {
        if (::close(fd_of(handle)) != 0) return last_error("close failed", {});
        return {};
    }

    // Rename without replacing, files and folders alike: renameat2(RENAME_NOREPLACE) on Linux, renamex_np(RENAME_EXCL)
    // on macOS; both fail atomically when the target exists. File systems without it: link() + unlink() for files
    // (also atomic), and an existence check right before rename() for folders.
    Status move_new(const std::filesystem::path& from, const std::filesystem::path& to) override {
#if defined(__APPLE__)
        if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) return sync_parent(to);
#elif defined(__linux__)
        if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) return sync_parent(to);
#endif
#if defined(__APPLE__) || defined(__linux__)
        if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP) return last_error("rename failed", to);
#endif
        struct stat st {};
        if (::stat(from.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            if (::lstat(to.c_str(), &st) == 0) {
                errno = EEXIST;
                return last_error("rename failed", to);
            }
            if (::rename(from.c_str(), to.c_str()) != 0) return last_error("rename failed", to);
            return sync_parent(to);
        }
        if (::link(from.c_str(), to.c_str()) != 0) return last_error("rename failed", to);
        if (::unlink(from.c_str()) != 0) return last_error("removing the temporary name failed", from);
        return sync_parent(to);
    }

    Status move_replace(const std::filesystem::path& from, const std::filesystem::path& to) override {
        if (::rename(from.c_str(), to.c_str()) != 0) return last_error("rename failed", to);
        return sync_parent(to);
    }

    // ReplaceFileW semantics: the old destination stays reachable at `backup` (a hard link), then the replacement
    // takes the destination's name in one atomic rename.
    ReplaceOutcome replace(const std::filesystem::path& destination, const std::filesystem::path& replacement,
                           const std::filesystem::path& backup) override {
        ::unlink(backup.c_str()); // a stale backup from an earlier run
        if (::link(destination.c_str(), backup.c_str()) != 0) return ReplaceOutcome::RemoveReplacedFailed; // nothing changed
        if (::rename(replacement.c_str(), destination.c_str()) != 0) return ReplaceOutcome::MoveReplacementFailed; // unchanged
        return sync_parent(destination) ? ReplaceOutcome::Ok : ReplaceOutcome::OtherError;
    }

    Status remove(const std::filesystem::path& path) override {
        if (::unlink(path.c_str()) != 0) return last_error("delete failed", path);
        return {};
    }

    bool exists(const std::filesystem::path& path) override {
        struct stat st {};
        return ::stat(path.c_str(), &st) == 0;
    }
};

} // namespace

FileOps& os_file_ops() noexcept {
    static PosixFileOps ops;
    return ops;
}

// ---------------------------------------------------------------------------
// MappedFile: a read-only private mapping; file_ holds the descriptor + 1, mapping_ is unused.
// ---------------------------------------------------------------------------

MappedFile::~MappedFile() { reset(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)), file_(std::exchange(other.file_, nullptr)),
      mapping_(std::exchange(other.mapping_, nullptr)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        reset();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        file_ = std::exchange(other.file_, nullptr);
        mapping_ = std::exchange(other.mapping_, nullptr);
    }
    return *this;
}

void MappedFile::reset() noexcept {
    if (data_ != nullptr && size_ > 0) ::munmap(const_cast<u8*>(data_), size_);
    if (file_ != nullptr) ::close(fd_of(file_));
    data_ = nullptr;
    size_ = 0;
    file_ = nullptr;
    mapping_ = nullptr;
}

Result<MappedFile> MappedFile::open(const std::filesystem::path& path) {
    MappedFile mf;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return last_error("cannot open file", path);
    mf.file_ = handle_of(fd);
    struct stat st {};
    if (::fstat(fd, &st) != 0) return last_error("cannot get file size", path);
    if (!S_ISREG(st.st_mode)) return make_error(ErrorCode::InvalidArgument, "not a regular file", ErrorContext{path.string(), std::nullopt, std::nullopt});
    if (st.st_size == 0) return mf; // empty file: nothing to map
    const auto size = static_cast<u64>(st.st_size);
    if (size > static_cast<u64>(SIZE_MAX)) return make_error(ErrorCode::LimitExceeded, "file too large to map");
    void* view = ::mmap(nullptr, static_cast<usize>(size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) return last_error("cannot map file", path);
    mf.data_ = static_cast<const u8*>(view);
    mf.size_ = static_cast<usize>(size);
    return mf;
}

} // namespace bridger::platform

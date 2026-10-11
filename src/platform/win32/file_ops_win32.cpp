// Windows implementation of FileOps and MappedFile.

#include <bridger/platform/atomic_replace.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>

#include <Windows.h>

#include <algorithm>
#include <string>
#include <utility>

namespace bridger::platform {
namespace {

std::string win32_message(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
        0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = len > 0 ? std::wstring(buffer, len) : std::wstring();
    if (buffer != nullptr) ::LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    auto utf8 = path_to_utf8(std::filesystem::path(text));
    return "Win32 error " + std::to_string(code) + (utf8 && !utf8->empty() ? ": " + *utf8 : std::string());
}

ErrorCode map_code(DWORD code) {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return ErrorCode::FileNotFound;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return ErrorCode::AlreadyExists;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION: return ErrorCode::AccessDenied;
    default: return ErrorCode::IoError;
    }
}

tl::unexpected<Error> last_error(std::string_view what, const std::filesystem::path& path) {
    const DWORD code = ::GetLastError();
    auto shown = path_to_utf8(path);
    return make_error(map_code(code), std::string(what) + ": " + win32_message(code),
                      ErrorContext{shown ? *shown : std::string(), std::nullopt, std::nullopt});
}

Result<std::wstring> native(const std::filesystem::path& path) {
    BRIDGER_TRY(std::filesystem::path long_path, to_long_path(path));
    return long_path.native();
}

class Win32FileOps final : public FileOps {
public:
    Result<Handle> create_new(const std::filesystem::path& path) override {
        BRIDGER_TRY(const std::wstring p, native(path));
        HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return last_error("cannot create file", path);
        return static_cast<Handle>(h);
    }

    Status write(Handle handle, std::span<const u8> bytes) override {
        while (!bytes.empty()) {
            const DWORD chunk = static_cast<DWORD>(std::min<usize>(bytes.size(), 1u << 30));
            DWORD written = 0;
            if (!::WriteFile(static_cast<HANDLE>(handle), bytes.data(), chunk, &written, nullptr) || written == 0)
                return last_error("write failed", {});
            bytes = bytes.subspan(written);
        }
        return {};
    }

    Status flush(Handle handle) override {
        if (!::FlushFileBuffers(static_cast<HANDLE>(handle))) return last_error("flush failed", {});
        return {};
    }

    Status close(Handle handle) override {
        if (!::CloseHandle(static_cast<HANDLE>(handle))) return last_error("close failed", {});
        return {};
    }

    Status move_new(const std::filesystem::path& from, const std::filesystem::path& to) override {
        BRIDGER_TRY(const std::wstring f, native(from));
        BRIDGER_TRY(const std::wstring t, native(to));
        if (!::MoveFileExW(f.c_str(), t.c_str(), MOVEFILE_WRITE_THROUGH)) return last_error("rename failed", to);
        return {};
    }

    Status move_replace(const std::filesystem::path& from, const std::filesystem::path& to) override {
        BRIDGER_TRY(const std::wstring f, native(from));
        BRIDGER_TRY(const std::wstring t, native(to));
        if (!::MoveFileExW(f.c_str(), t.c_str(), MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING))
            return last_error("rename failed", to);
        return {};
    }

    ReplaceOutcome replace(const std::filesystem::path& destination, const std::filesystem::path& replacement,
                           const std::filesystem::path& backup) override {
        auto d = native(destination);
        auto r = native(replacement);
        auto b = native(backup);
        if (!d || !r || !b) return ReplaceOutcome::OtherError;
        if (::ReplaceFileW(d->c_str(), r->c_str(), b->c_str(), 0, nullptr, nullptr)) return ReplaceOutcome::Ok;
        switch (::GetLastError()) {
        case ERROR_UNABLE_TO_REMOVE_REPLACED: return ReplaceOutcome::RemoveReplacedFailed;
        case ERROR_UNABLE_TO_MOVE_REPLACEMENT: return ReplaceOutcome::MoveReplacementFailed;
        case ERROR_UNABLE_TO_MOVE_REPLACEMENT_2: return ReplaceOutcome::MoveReplacement2Failed;
        default: return ReplaceOutcome::OtherError;
        }
    }

    Status remove(const std::filesystem::path& path) override {
        BRIDGER_TRY(const std::wstring p, native(path));
        if (!::DeleteFileW(p.c_str())) return last_error("delete failed", path);
        return {};
    }

    bool exists(const std::filesystem::path& path) override {
        auto p = native(path);
        return p && ::GetFileAttributesW(p->c_str()) != INVALID_FILE_ATTRIBUTES;
    }
};

} // namespace

FileOps& os_file_ops() noexcept {
    static Win32FileOps ops;
    return ops;
}

// ---------------------------------------------------------------------------
// MappedFile
// ---------------------------------------------------------------------------

MappedFile::~MappedFile() { reset(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
      file_(std::exchange(other.file_, nullptr)), mapping_(std::exchange(other.mapping_, nullptr)) {}

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
    if (data_ != nullptr) ::UnmapViewOfFile(data_);
    if (mapping_ != nullptr) ::CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_ != nullptr) ::CloseHandle(static_cast<HANDLE>(file_));
    data_ = nullptr;
    size_ = 0;
    mapping_ = nullptr;
    file_ = nullptr;
}

Result<MappedFile> MappedFile::open(const std::filesystem::path& path) {
    BRIDGER_TRY(const std::wstring p, native(path));
    MappedFile mf;
    HANDLE file = ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return last_error("cannot open file", path);
    mf.file_ = file;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size)) return last_error("cannot get file size", path);
    if (size.QuadPart == 0) return mf; // empty file: nothing to map
    if (static_cast<u64>(size.QuadPart) > static_cast<u64>(SIZE_MAX))
        return make_error(ErrorCode::LimitExceeded, "file too large to map");

    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) return last_error("cannot map file", path);
    mf.mapping_ = mapping;

    const void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) return last_error("cannot map file view", path);
    mf.data_ = static_cast<const u8*>(view);
    mf.size_ = static_cast<usize>(size.QuadPart);
    return mf;
}

} // namespace bridger::platform

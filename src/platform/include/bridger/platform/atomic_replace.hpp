#pragma once

// Safe output writing (docs/safety.md §2):
//   temp file in the destination's directory -> write -> flush -> validate ->
//   MoveFileExW (new file) or ReplaceFileW with a backup (existing file) ->
//   validate destination -> remove backup.
// The temp file and backup are never deleted until the destination is confirmed
// valid. Every failure message states exactly which file is where.

#include <bridger/core/result.hpp>
#include <bridger/core/types.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <span>

namespace bridger::platform {

// Outcome of a ReplaceFileW-style operation, in OS-neutral terms.
enum class ReplaceOutcome : u8 {
    Ok,
    RemoveReplacedFailed,   // ERROR_UNABLE_TO_REMOVE_REPLACED: destination unchanged
    MoveReplacementFailed,  // ERROR_UNABLE_TO_MOVE_REPLACEMENT: destination unchanged
    MoveReplacement2Failed, // ERROR_UNABLE_TO_MOVE_REPLACEMENT_2: original is at the backup name
    OtherError,             // anything else: inspect the file system
};

// File-system primitives used by AtomicFileWriter. The default implementation
// calls the OS; tests substitute one that injects failures at chosen steps.
class FileOps {
public:
    using Handle = void*;
    virtual ~FileOps() = default;

    [[nodiscard]] virtual Result<Handle> create_new(const std::filesystem::path& path) = 0; // fails if it exists
    [[nodiscard]] virtual Status write(Handle handle, std::span<const u8> bytes) = 0;
    [[nodiscard]] virtual Status flush(Handle handle) = 0; // durable: FlushFileBuffers
    [[nodiscard]] virtual Status close(Handle handle) = 0;
    // Rename without replacing; durable (MOVEFILE_WRITE_THROUGH).
    [[nodiscard]] virtual Status move_new(const std::filesystem::path& from, const std::filesystem::path& to) = 0;
    // Rename replacing an existing target; durable.
    [[nodiscard]] virtual Status move_replace(const std::filesystem::path& from,
                                              const std::filesystem::path& to) = 0;
    // ReplaceFileW(destination, replacement, backup).
    [[nodiscard]] virtual ReplaceOutcome replace(const std::filesystem::path& destination,
                                                 const std::filesystem::path& replacement,
                                                 const std::filesystem::path& backup) = 0;
    [[nodiscard]] virtual Status remove(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual bool exists(const std::filesystem::path& path) = 0;
};

// The real OS implementation (a process-wide stateless instance).
[[nodiscard]] FileOps& os_file_ops() noexcept;

// Validates a written file (e.g. re-reads it with Bridger's own reader).
using FileValidator = std::function<Status(const std::filesystem::path&)>;

class AtomicFileWriter {
public:
    // Creates "<dest>.<random>.bridger-tmp" next to `destination`.
    [[nodiscard]] static Result<AtomicFileWriter> create(const std::filesystem::path& destination,
                                                         FileOps& ops = os_file_ops());

    AtomicFileWriter(AtomicFileWriter&& other) noexcept;
    AtomicFileWriter& operator=(AtomicFileWriter&&) = delete;
    AtomicFileWriter(const AtomicFileWriter&) = delete;
    AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;
    // Abandoning an uncommitted writer closes and deletes its temp file
    // (nothing has touched the destination yet).
    ~AtomicFileWriter();

    [[nodiscard]] Status write(std::span<const u8> bytes);

    // Flush, validate, move into place, validate the destination, drop the backup.
    // On failure the returned error describes where every file is.
    [[nodiscard]] Status commit(const FileValidator& validate);

    [[nodiscard]] const std::filesystem::path& destination() const noexcept { return destination_; }
    [[nodiscard]] const std::filesystem::path& temp_path() const noexcept { return temp_; }

private:
    AtomicFileWriter(std::filesystem::path destination, std::filesystem::path temp, FileOps::Handle handle,
                     FileOps& ops);

    Status replace_existing(const FileValidator& validate);

    std::filesystem::path destination_;
    std::filesystem::path temp_;
    FileOps::Handle handle_ = nullptr;
    FileOps* ops_ = nullptr;
    bool committed_ = false; // commit() ran (successfully or not): never auto-delete afterwards
};

// Convenience: write a whole buffer safely.
[[nodiscard]] Status write_file_atomically(const std::filesystem::path& destination, std::span<const u8> bytes,
                                           const FileValidator& validate, FileOps& ops = os_file_ops());

} // namespace bridger::platform

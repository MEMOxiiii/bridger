// OS-independent part of the atomic writer: the commit protocol and recovery.
// The OS calls live behind FileOps (win32/file_ops_win32.cpp).

#include <bridger/platform/atomic_replace.hpp>
#include <bridger/platform/paths.hpp>

#include <string>
#include <utility>

namespace bridger::platform {
namespace {

std::string show(const std::filesystem::path& p) {
    auto utf8 = path_to_utf8(p);
    return utf8 ? *utf8 : std::string("<unprintable path>");
}

// "Files now: destination '<d>' (present), temp '<t>' (present), backup '<b>' (absent)."
std::string where_are_files(FileOps& ops, const std::filesystem::path& dest, const std::filesystem::path& temp,
                            const std::filesystem::path* backup) {
    auto state = [&ops](const std::filesystem::path& p) { return ops.exists(p) ? "present" : "absent"; };
    std::string s = "Files now: destination '" + show(dest) + "' (" + state(dest) + "), temp '" + show(temp) +
                    "' (" + state(temp) + ")";
    if (backup != nullptr) s += ", backup '" + show(*backup) + "' (" + state(*backup) + ")";
    return s + ".";
}

} // namespace

AtomicFileWriter::AtomicFileWriter(std::filesystem::path destination, std::filesystem::path temp,
                                   FileOps::Handle handle, FileOps& ops)
    : destination_(std::move(destination)), temp_(std::move(temp)), handle_(handle), ops_(&ops) {}

AtomicFileWriter::AtomicFileWriter(AtomicFileWriter&& other) noexcept
    : destination_(std::move(other.destination_)), temp_(std::move(other.temp_)),
      handle_(std::exchange(other.handle_, nullptr)), ops_(other.ops_), committed_(other.committed_) {
    other.committed_ = true; // moved-from object must not delete anything
}

AtomicFileWriter::~AtomicFileWriter() {
    if (committed_ || ops_ == nullptr) return;
    if (handle_ != nullptr) (void)ops_->close(handle_);
    (void)ops_->remove(temp_);
}

Result<AtomicFileWriter> AtomicFileWriter::create(const std::filesystem::path& destination, FileOps& ops) {
    if (!destination.has_filename())
        return make_error(ErrorCode::InvalidArgument, "output path has no file name: " + show(destination));
    // A few attempts in case a random name collides with an existing file.
    for (int attempt = 0; attempt < 8; ++attempt) {
        BRIDGER_TRY(std::filesystem::path temp, sibling_temp_path(destination, "bridger-tmp"));
        auto handle = ops.create_new(temp);
        if (handle) return AtomicFileWriter(destination, std::move(temp), *handle, ops);
        if (handle.error().code() != ErrorCode::AlreadyExists) return tl::unexpected(std::move(handle).error());
    }
    return make_error(ErrorCode::IoError, "could not create a unique temp file next to " + show(destination));
}

AtomicFileWriter AtomicFileWriter::adopt(std::filesystem::path temp, std::filesystem::path destination, FileOps& ops) {
    return AtomicFileWriter(std::move(destination), std::move(temp), nullptr, ops);
}

Result<std::filesystem::path> AtomicFileWriter::temp_path_for(const std::filesystem::path& destination) {
    return sibling_temp_path(destination, "bridger-tmp");
}

Status AtomicFileWriter::write(std::span<const u8> bytes) {
    if (handle_ == nullptr) return make_error(ErrorCode::InvalidArgument, "write after commit");
    return ops_->write(handle_, bytes);
}

Status AtomicFileWriter::commit(const FileValidator& validate) {
    if (committed_) return make_error(ErrorCode::InvalidArgument, "commit called twice");
    committed_ = true;

    // 1. Make the temp file durable and close it (adopted files are flushed by path).
    if (handle_ == nullptr) {
        if (auto s = ops_->flush_path(temp_); !s)
            return make_error(ErrorCode::IoError, "flushing the output failed: " + s.error().message() +
                                                      ". The destination was not touched. " +
                                                      where_are_files(*ops_, destination_, temp_, nullptr));
    } else if (auto s = ops_->flush(handle_); !s) {
        (void)ops_->close(std::exchange(handle_, nullptr));
        return make_error(ErrorCode::IoError, "flushing the output failed: " + s.error().message() +
                                                  ". The destination was not touched. " +
                                                  where_are_files(*ops_, destination_, temp_, nullptr));
    }
    if (handle_ == nullptr) {
        // adopted: nothing to close
    } else if (auto s = ops_->close(std::exchange(handle_, nullptr)); !s) {
        return make_error(ErrorCode::IoError, "closing the output failed: " + s.error().message() +
                                                  ". The destination was not touched. " +
                                                  where_are_files(*ops_, destination_, temp_, nullptr));
    }

    // 2. Validate before anything touches the destination. An invalid temp file is
    //    not user data, so it is removed.
    if (auto s = validate(temp_); !s) {
        (void)ops_->remove(temp_);
        // A cancellation checked by the validator (the last moment before the move) is not a failure.
        if (s.error().code() == ErrorCode::Cancelled)
            return make_error(ErrorCode::Cancelled, s.error().message() + "; the output was discarded and the destination was not touched");
        return make_error(ErrorCode::ValidationFailed,
                          "the written output failed validation (" + s.error().describe() +
                              "); it was discarded and the destination was not touched");
    }

    // 3a. New file: rename without replacing. If something appeared at the destination
    //     meanwhile, the rename fails and nothing is lost.
    if (!ops_->exists(destination_)) {
        if (auto s = ops_->move_new(temp_, destination_); !s) {
            return make_error(ErrorCode::IoError, "moving the output into place failed: " + s.error().message() +
                                                      ". Your new output is kept at the temp path. " +
                                                      where_are_files(*ops_, destination_, temp_, nullptr));
        }
        return {};
    }

    // 3b. Existing file (the user chose to overwrite).
    return replace_existing(validate);
}

Status AtomicFileWriter::replace_existing(const FileValidator& validate) {
    BRIDGER_TRY(const std::filesystem::path backup, sibling_temp_path(destination_, "bridger-bak"));
    const ReplaceOutcome outcome = ops_->replace(destination_, temp_, backup);

    switch (outcome) {
    case ReplaceOutcome::Ok: {
        // 4. Validate what is now at the destination; only then drop the backup.
        if (auto s = validate(destination_); !s) {
            if (ops_->move_replace(backup, destination_)) {
                return make_error(ErrorCode::ValidationFailed,
                                  "the replaced destination failed validation (" + s.error().describe() +
                                      "); the previous file was restored. " +
                                      where_are_files(*ops_, destination_, temp_, &backup));
            }
            return make_error(ErrorCode::ValidationFailed,
                              "the replaced destination failed validation (" + s.error().describe() +
                                  ") and restoring the previous file from the backup failed. " +
                                  where_are_files(*ops_, destination_, temp_, &backup));
        }
        // Failing to delete the backup is not a failure of the conversion; it is reported in the log by callers.
        (void)ops_->remove(backup);
        return {};
    }
    case ReplaceOutcome::RemoveReplacedFailed:
    case ReplaceOutcome::MoveReplacementFailed:
        return make_error(ErrorCode::IoError,
                          "replacing the existing file failed; the destination is unchanged and your new output "
                          "is kept at the temp path. " +
                              where_are_files(*ops_, destination_, temp_, &backup));
    case ReplaceOutcome::MoveReplacement2Failed:
    case ReplaceOutcome::OtherError:
        break;
    }

    // The original may now be under the backup name with the destination name free.
    if (!ops_->exists(destination_) && ops_->exists(backup)) {
        if (ops_->move_new(backup, destination_)) {
            return make_error(ErrorCode::IoError,
                              "replacing the existing file failed; the original was restored and your new output "
                              "is kept at the temp path. " +
                                  where_are_files(*ops_, destination_, temp_, &backup));
        }
        return make_error(ErrorCode::IoError,
                          "replacing the existing file failed and the original could not be restored "
                          "automatically: it is at the backup path. " +
                              where_are_files(*ops_, destination_, temp_, &backup));
    }
    return make_error(ErrorCode::IoError,
                      "replacing the existing file failed. " + where_are_files(*ops_, destination_, temp_, &backup));
}

Status write_file_atomically(const std::filesystem::path& destination, std::span<const u8> bytes,
                             const FileValidator& validate, FileOps& ops) {
    BRIDGER_TRY(AtomicFileWriter writer, AtomicFileWriter::create(destination, ops));
    BRIDGER_TRY_STATUS(writer.write(bytes));
    return writer.commit(validate);
}

} // namespace bridger::platform

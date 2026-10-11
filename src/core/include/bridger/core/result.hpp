#pragma once

// Error handling: bridger::Result<T> = tl::expected<T, bridger::Error>.
// No exceptions cross module boundaries (docs/adr/0003-error-handling.md).

#include <bridger/core/types.hpp>

#include <tl/expected.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace bridger {

enum class ErrorCode : u16 {
    Unknown = 0,
    InvalidArgument,
    NotImplemented,
    // I/O and platform
    FileNotFound,
    AlreadyExists,
    AccessDenied,
    IoError,
    // Data
    UnexpectedEof,
    Malformed,          // structurally invalid input
    LimitExceeded,      // size / depth / decompression limits (untrusted input guards)
    UnsupportedFormat,
    UnsupportedVersion,
    // Conversion
    Unmapped,
    OutOfBounds,        // e.g. structure exceeds world build height
    Cancelled,
    ValidationFailed,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

// Where an error happened. All fields are optional; fill in what is known.
struct ErrorContext {
    std::string file;
    std::optional<u64> offset;
    std::optional<ChunkPos> chunk;

    // Use these instead of partial designated initializers ({.offset = x}), which
    // clang warns about (-Wmissing-designated-field-initializers).
    [[nodiscard]] static ErrorContext at_offset(u64 offset) {
        ErrorContext ctx;
        ctx.offset = offset;
        return ctx;
    }
    [[nodiscard]] static ErrorContext at_chunk(ChunkPos chunk) {
        ErrorContext ctx;
        ctx.chunk = chunk;
        return ctx;
    }
};

class Error {
public:
    Error(ErrorCode code, std::string message, ErrorContext context = {})
        : code_(code), message_(std::move(message)), context_(std::move(context)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const ErrorContext& context() const noexcept { return context_; }

    // Adds context while an error propagates upward (fields already set are kept).
    Error& with_file(std::string file) & {
        if (context_.file.empty()) context_.file = std::move(file);
        return *this;
    }
    Error&& with_file(std::string file) && { return std::move(with_file(std::move(file))); }

    Error& with_offset(u64 offset) & {
        if (!context_.offset) context_.offset = offset;
        return *this;
    }
    Error&& with_offset(u64 offset) && { return std::move(with_offset(offset)); }

    Error& with_chunk(ChunkPos chunk) & {
        if (!context_.chunk) context_.chunk = chunk;
        return *this;
    }
    Error&& with_chunk(ChunkPos chunk) && { return std::move(with_chunk(chunk)); }

    // Human-readable: "<code>: <message> (file '<f>', offset <o>, chunk <x>,<z>)".
    [[nodiscard]] std::string describe() const;

private:
    ErrorCode code_;
    std::string message_;
    ErrorContext context_;
};

template <typename T>
using Result = tl::expected<T, Error>;

using Status = Result<void>;

[[nodiscard]] inline tl::unexpected<Error> make_error(ErrorCode code, std::string message,
                                                      ErrorContext context = {}) {
    return tl::unexpected<Error>(Error(code, std::move(message), std::move(context)));
}

// Propagates an error from an expression returning Result<...>.
// Usage: BRIDGER_TRY(auto value, parse(x));  or  BRIDGER_TRY_STATUS(write(y));
#define BRIDGER_CONCAT_INNER(a, b) a##b
#define BRIDGER_CONCAT(a, b) BRIDGER_CONCAT_INNER(a, b)

#define BRIDGER_TRY_IMPL(tmp, decl, expr)                                                          \
    auto tmp = (expr);                                                                              \
    if (!tmp) return ::tl::unexpected<::bridger::Error>(std::move(tmp).error());                   \
    decl = std::move(*tmp)

#define BRIDGER_TRY(decl, expr) BRIDGER_TRY_IMPL(BRIDGER_CONCAT(bridger_try_, __LINE__), decl, expr)

#define BRIDGER_TRY_STATUS(expr)                                                                   \
    do {                                                                                            \
        if (auto bridger_status_ = (expr); !bridger_status_)                                        \
            return ::tl::unexpected<::bridger::Error>(std::move(bridger_status_).error());         \
    } while (false)

} // namespace bridger

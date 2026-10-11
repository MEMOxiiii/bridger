#include <bridger/core/result.hpp>

#include <format>

namespace bridger {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown: return "unknown error";
    case ErrorCode::InvalidArgument: return "invalid argument";
    case ErrorCode::NotImplemented: return "not implemented";
    case ErrorCode::FileNotFound: return "file not found";
    case ErrorCode::AlreadyExists: return "already exists";
    case ErrorCode::AccessDenied: return "access denied";
    case ErrorCode::IoError: return "I/O error";
    case ErrorCode::UnexpectedEof: return "unexpected end of data";
    case ErrorCode::Malformed: return "malformed data";
    case ErrorCode::LimitExceeded: return "limit exceeded";
    case ErrorCode::UnsupportedFormat: return "unsupported format";
    case ErrorCode::UnsupportedVersion: return "unsupported version";
    case ErrorCode::Unmapped: return "unmapped";
    case ErrorCode::OutOfBounds: return "out of bounds";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::ValidationFailed: return "validation failed";
    }
    return "unknown error";
}

std::string Error::describe() const {
    std::string out = std::format("{}: {}", to_string(code_), message_);
    std::string ctx;
    auto append = [&ctx](std::string part) {
        if (!ctx.empty()) ctx += ", ";
        ctx += std::move(part);
    };
    if (!context_.file.empty()) append(std::format("file '{}'", context_.file));
    if (context_.offset) append(std::format("offset {}", *context_.offset));
    if (context_.chunk) append(std::format("chunk {},{}", context_.chunk->x, context_.chunk->z));
    if (!ctx.empty()) out += std::format(" ({})", ctx);
    return out;
}

} // namespace bridger

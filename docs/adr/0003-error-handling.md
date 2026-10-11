# 0003. Error handling: `Result<T>`, no exceptions across modules

- Status: accepted
- Date: 2026-10-11

## Context

Input is untrusted and malformed data is normal, not exceptional. Errors must carry enough context (file,
offset, chunk) to be reported precisely. Hot paths must stay predictable.

## Options

1. C++ exceptions.
2. `std::expected` (C++23). Not available under `/std:c++20`.
3. `tl::expected` (a C++11/14/17 implementation of the same API).

## Decision

- `bridger::Result<T> = tl::expected<T, bridger::Error>` and `bridger::Status = Result<void>`.
- `bridger::Error` holds an `ErrorCode`, a message, and an `ErrorContext` (file, byte offset, chunk). Context is
  added while the error propagates (`with_file`, `with_offset`, `with_chunk`). A field that is already set is
  never overwritten, so the innermost, most precise context wins.
- `BRIDGER_TRY(decl, expr)` and `BRIDGER_TRY_STATUS(expr)` propagate errors.
- **No exceptions cross module boundaries.** Code that calls throwing third-party APIs (e.g. `std::filesystem`
  without `error_code`, or allocation failure) converts them at the boundary. `std::bad_alloc` is the accepted
  exception: it terminates the conversion.

## Consequences

- Switching to `std::expected` later (C++23) is mechanical, because the API is the same.
- `tl::expected` appears in `core/result.hpp`, a public header. That is accepted, because the type is part of
  every module's API.

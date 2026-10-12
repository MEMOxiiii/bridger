#pragma once

// Thin wrappers over oneTBB, so TBB headers stay out of the rest of the code base.

#include <bridger/core/types.hpp>

#include <functional>

namespace bridger {

// Number of worker threads for `requested` (0 = all hardware threads).
[[nodiscard]] usize resolve_thread_count(usize requested) noexcept;

// Calls fn(i) for every i in [0, count) on up to `threads` threads (0 = all). Returns
// after all calls finished. With threads == 1 the calls run in order on this thread.
// fn must not throw.
void parallel_for(usize count, usize threads, const std::function<void(usize)>& fn);

} // namespace bridger

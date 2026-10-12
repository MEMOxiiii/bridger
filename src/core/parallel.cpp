#include <bridger/core/parallel.hpp>

#include <tbb/blocked_range.h>
#include <tbb/info.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

namespace bridger {

usize resolve_thread_count(usize requested) noexcept {
    if (requested != 0) return requested;
    const int n = tbb::info::default_concurrency();
    return n > 0 ? static_cast<usize>(n) : 1;
}

void parallel_for(usize count, usize threads, const std::function<void(usize)>& fn) {
    threads = resolve_thread_count(threads);
    if (threads == 1 || count <= 1) {
        for (usize i = 0; i < count; ++i) fn(i);
        return;
    }
    tbb::task_arena arena(static_cast<int>(threads));
    arena.execute([&] {
        tbb::parallel_for(tbb::blocked_range<usize>(0, count, 1), [&](const tbb::blocked_range<usize>& r) {
            for (usize i = r.begin(); i != r.end(); ++i) fn(i);
        });
    });
}

} // namespace bridger

/**
 * @file parallelfor.h
 * @brief A tiny index-parallel loop for the engine's CPU-heavy bakes (pure std, no Qt/Vulkan).
 *
 * Used by the IBL environment bakes (environment.cpp) and the figure importer's per-corrective
 * subdivision (figureimporter.cpp) — embarrassingly parallel loops whose single-threaded cost is
 * user-visible (a multi-second startup/import stall). Indices are claimed atomically, so items of
 * uneven cost still balance across cores.
 */

#ifndef PARALLELFOR_H
#define PARALLELFOR_H

#include <atomic>
#include <thread>
#include <vector>

namespace pose {

/// Runs fn(i) for every i in [0, count) across the hardware's threads. Falls back to a plain
/// inline loop when count is small or only one core is available. fn must be safe to call
/// concurrently for distinct indices (each index's work must touch disjoint output).
template <typename Fn>
void parallelFor(int count, Fn&& fn) {
    if (count <= 0) {
        return;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    const int workers = std::min(count, static_cast<int>(hw > 1 ? hw : 1));
    if (workers <= 1 || count == 1) {
        for (int i = 0; i < count; ++i) {
            fn(i);
        }
        return;
    }
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(workers));
    for (int w = 0; w < workers; ++w) {
        pool.emplace_back([&next, count, &fn]() {
            for (int i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
                fn(i);
            }
        });
    }
    for (std::thread& t : pool) {
        t.join();
    }
}

} // namespace pose

#endif // PARALLELFOR_H

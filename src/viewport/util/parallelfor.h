// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file parallelfor.h
 * @brief A tiny index-parallel loop for the engine's CPU-heavy loops (pure std, no Qt/Vulkan).
 *
 * Used wherever an embarrassingly parallel loop's single-threaded cost is user-visible: the IBL
 * environment bake, the figure importer's corrective scan and subdivision, the resolver's
 * document prefetch, the ambient-occlusion bake, the texture decode and gutter fill, the splat
 * sort, the overlay compositor, displacement, a render capture's posed-mesh bake and the
 * read-backs' unpacking. Indices are claimed atomically, so items of uneven cost still balance
 * across cores. Lives in viewport/util because it belongs to no layer — rendering/, scene/,
 * import/ and the Qt-facing services all use it.
 *
 * Threads are started per call, not pooled: every caller runs once per user action, or a few
 * times a second at most (the splat sort runs inline under a million splats), so a persistent
 * pool would buy nothing measurable.
 */

#ifndef PARALLELFOR_H
#define PARALLELFOR_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace pose {

/// Runs fn(i) for every i in [0, count) across the hardware's threads, the calling thread among
/// them; returns when all have finished. Falls back to a plain inline loop when count is 1 or
/// only one core is available. fn must be safe to call concurrently for distinct indices (each
/// index's work must touch disjoint output).
///
/// An exception thrown by fn is rethrown here, on the calling thread, once every worker has
/// stopped (the first one wins; indices not yet claimed are then left undone) — a failed
/// allocation in a decode or a bake ends in the caller's error handling, not in std::terminate.
template <typename Fn>
void parallelFor(int count, Fn&& fn) {
    if (count <= 0) {
        return;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    const int workers = std::min(count, static_cast<int>(hw > 1 ? hw : 1));
    if (workers <= 1) {
        for (int i = 0; i < count; ++i) {
            fn(i);
        }
        return;
    }
    std::atomic<int>   next{0};
    std::atomic<bool>  failed{false};
    std::exception_ptr error; // the first exception a worker met; guarded by errorMutex
    std::mutex         errorMutex;
    const auto work = [&]() {
        try {
            for (int i = next.fetch_add(1); i < count && !failed.load(std::memory_order_relaxed);
                 i = next.fetch_add(1)) {
                fn(i);
            }
        } catch (...) {
            const std::lock_guard<std::mutex> lock(errorMutex);
            if (!error) {
                error = std::current_exception();
            }
            failed.store(true, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(workers) - 1);
    try {
        for (int w = 1; w < workers; ++w) {
            pool.emplace_back(work);
        }
    } catch (...) {
        // A thread could not be started: the ones that were, and this one, do all the work.
    }
    work(); // the calling thread is the last worker
    for (std::thread& t : pool) {
        t.join();
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

} // namespace pose

#endif // PARALLELFOR_H

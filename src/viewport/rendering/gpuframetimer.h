// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file gpuframetimer.h
 * @brief GPU time of a frame, and of the Gaussian splat draws within it, from timestamp queries.
 *
 * A frame stamps the top of its command buffer, either side of each scene pass's splat draw, and
 * the bottom; the stamps are read back where the frame's fence is waited for — the previous frame
 * of that slot — so nothing ever waits on a query. They are written with the availability bit, so
 * a stamp a frame never recorded (the desktop's second scene pass; a frame recorded before timing
 * began) is skipped, not misread.
 *
 * On whenever the queue has timestamps (a handful of stamps and one non-blocking readback a
 * frame): VR Mode's dynamic splat scale reads the result, and POSESTUDIO_GPU_PERF /
 * POSESTUDIO_VR_PERF print it. On a queue without timestamps every call is a no-op and no
 * reading is ever valid.
 *
 * A SLOT is one frame that may be in flight on its own: the renderer uses one per desktop frame
 * in flight and one for the VR frame. Not thread-safe; Qt-free.
 */

#ifndef GPUFRAMETIMER_H
#define GPUFRAMETIMER_H

#include "vulkanhandles.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace pose {

class VulkanContext;

/// The GPU time of the last COMPLETED frame of a kind — the whole command buffer, and the
/// Gaussian splat draw(s) within it (both eyes' in a VR frame) — in milliseconds. `valid` once
/// such a frame has completed.
struct GpuTimings {
    double frameMs = 0.0;
    double splatMs = 0.0;
    bool   valid = false;
};

/**
 * @class GpuFrameTimer
 * @brief The timestamp query pool and the bookkeeping of which stamp is which.
 */
class GpuFrameTimer {
public:
    /// @param slotCount How many frames may be stamped independently (see the file comment).
    GpuFrameTimer(VulkanContext& context, uint32_t slotCount);

    /// Starts @p slot's frame: resets the slot's queries and writes its first stamp. Call right
    /// after vkBeginCommandBuffer, OUTSIDE any render pass (a query pool reset is not allowed in
    /// one). The marks and end() that follow belong to this slot.
    void begin(VkCommandBuffer cmd, uint32_t slot);
    /// Brackets one scene pass's splat draw. Up to kSplatPasses pairs a frame are kept (the
    /// desktop's one; an eye each in VR); a further pair stamps nothing.
    void beginSplats(VkCommandBuffer cmd);
    void endSplats(VkCommandBuffer cmd);
    /// The frame's last stamp: call before vkEndCommandBuffer.
    void end(VkCommandBuffer cmd);

    /// Reads @p slot's last frame into @p into. Call only once that frame's fence has been
    /// waited for. Leaves @p into as it was while the frame's own two stamps are not both in
    /// (no frame stamped yet): the last good reading stands. Throws VulkanError on a device error.
    void read(uint32_t slot, GpuTimings& into);

private:
    /// Scene passes whose splat draw is timed per frame.
    static constexpr uint32_t kSplatPasses = 2;
    /// Per slot: the frame's first stamp, a begin/end pair per splat pass, the frame's last stamp.
    static constexpr uint32_t kQueries = 2 + 2 * kSplatPasses;

    VulkanContext&    m_context;
    bool              m_enabled = false;        ///< false on a queue without timestamps
    float             m_periodNs = 0.0f;        ///< nanoseconds per timestamp tick
    UniqueQueryPool   m_pool;
    std::vector<bool> m_written;                ///< per slot: a frame has stamped it
    uint32_t          m_slot = 0;               ///< the slot the frame being recorded stamps
    uint32_t          m_pass = 0;               ///< its splat passes so far
};

} // namespace pose

#endif // GPUFRAMETIMER_H

/**
 * @file vulkancommands.h
 * @brief One-shot command submission for load-time GPU work (buffer/image uploads, layout
 *        transitions, mipmap blits).
 *
 * Two flavours, both Qt-free:
 *  - submitImmediate(): a single, self-contained transfer — spins up a throwaway transient pool,
 *    records via the callback, submits, and blocks on a fence. Fine for a lone upload.
 *  - ImmediateBatch: accumulate MANY transfers into one command buffer and submit them with a
 *    single fence wait. Importing a model with many material groups is dominated by per-upload
 *    submit+stall cost, so batching is the difference between one GPU round-trip and dozens.
 */

#ifndef VULKANCOMMANDS_H
#define VULKANCOMMANDS_H

#include "vulkanbuffer.h" // ImmediateBatch retains staging VulkanBuffers until the GPU completes

#include <vulkan/vulkan.h>

#include <functional>
#include <vector>

namespace pose {

class VulkanContext;

/// Records @p recorder into a one-time-submit command buffer on the graphics queue and blocks
/// until the GPU finishes. Throws VulkanError on failure. (Thin wrapper over ImmediateBatch; for
/// a single transfer. Prefer ImmediateBatch directly when uploading many resources at once.)
void submitImmediate(VulkanContext& context, const std::function<void(VkCommandBuffer)>& recorder);

/**
 * @class ImmediateBatch
 * @brief Collects load-time GPU transfers (buffer copies, image uploads, mip blits, layout
 *        transitions) into ONE command buffer and submits them with a SINGLE fence wait.
 *
 * The per-resource alternative — a submitImmediate() per buffer/texture — pays a full
 * graphics-queue submit + CPU fence stall (plus a throwaway pool/fence) for every upload, which
 * dominates import time once a model has many meshes. Record everything via commandBuffer(),
 * call submitAndWait() once, and that collapses to a single round-trip.
 *
 * Staging buffers passed to retain() are kept alive until submitAndWait() completes — the GPU is
 * still reading them until then. Not thread-safe; use on the import (GUI) thread. Qt-free.
 */
class ImmediateBatch {
public:
    /// Creates a transient command pool and begins a one-time-submit primary command buffer.
    explicit ImmediateBatch(VulkanContext& context);
    /// submitAndWait()s if the caller hasn't, then frees the pool. Swallows errors (destructor).
    ~ImmediateBatch();

    ImmediateBatch(const ImmediateBatch&) = delete;
    ImmediateBatch& operator=(const ImmediateBatch&) = delete;

    /// The command buffer to record transfer/transition commands into (valid until submitAndWait).
    VkCommandBuffer commandBuffer() const { return m_cmd; }

    /// Keep a staging buffer alive until submitAndWait() completes (the GPU reads it during submit).
    void retain(VulkanBuffer&& staging);

    /// Ends recording, submits once, blocks on a single fence, then frees the staging buffers and
    /// the pool. Idempotent. Throws VulkanError on failure.
    void submitAndWait();

private:
    VulkanContext*            m_context   = nullptr;
    VkCommandPool             m_pool      = VK_NULL_HANDLE;
    VkCommandBuffer           m_cmd       = VK_NULL_HANDLE;
    std::vector<VulkanBuffer> m_retained; // staging buffers held until the submit completes
    bool                      m_submitted = false;
};

} // namespace pose

#endif // VULKANCOMMANDS_H

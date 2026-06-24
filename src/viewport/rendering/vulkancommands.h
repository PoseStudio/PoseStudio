/**
 * @file vulkancommands.h
 * @brief One-shot command submission for load-time GPU work (buffer/image uploads, layout
 *        transitions, mipmap blits).
 *
 * These run rarely (on import), so each call spins up a throwaway transient command pool,
 * records via the callback, submits to the graphics queue, and blocks on a fence. Qt-free.
 */

#ifndef VULKANCOMMANDS_H
#define VULKANCOMMANDS_H

#include <vulkan/vulkan.h>

#include <functional>

namespace pose {

class VulkanContext;

/// Records @p recorder into a one-time-submit command buffer on the graphics queue and blocks
/// until the GPU finishes. Throws VulkanError on failure.
void submitImmediate(VulkanContext& context, const std::function<void(VkCommandBuffer)>& recorder);

} // namespace pose

#endif // VULKANCOMMANDS_H

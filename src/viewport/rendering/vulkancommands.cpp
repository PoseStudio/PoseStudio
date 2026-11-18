/**
 * @file vulkancommands.cpp
 * @brief Implementation of ImmediateBatch and submitImmediate. See vulkancommands.h.
 */

#include "vulkancommands.h"

#include "vulkancommon.h"
#include "vulkancontext.h"

#include <utility>

namespace pose {

ImmediateBatch::ImmediateBatch(VulkanContext& context) : m_context(&context) {
    VkDevice device = context.device();

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = context.graphicsFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
    m_pool.reset(device, pool); // owned from here: a throw below frees it

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &m_cmd));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(m_cmd, &begin));
}

// A batch that was never submitted is DISCARDED: the pool goes (and the command buffer with it),
// the staging buffers go, and nothing was ever queued. It must not be submitted here — the
// destructor also runs when an exception unwinds past the batch, and by then the objects its
// recorded commands name may already be destroyed (a Mesh whose vertex-buffer copy is recorded
// and whose next allocation then fails; a recorder that threw inside an open render pass).
// Submitting that names dead handles on exactly the path that should end in "import failed".
ImmediateBatch::~ImmediateBatch() = default;

void ImmediateBatch::retain(VulkanBuffer&& staging) {
    m_retained.push_back(std::move(staging));
}

void ImmediateBatch::submitAndWait() {
    if (m_submitted) {
        return;
    }
    m_submitted = true;

    VkDevice device = m_context->device();
    VK_CHECK(vkEndCommandBuffer(m_cmd));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_cmd;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence rawFence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &rawFence));
    const UniqueFence fence(device, rawFence);

    try {
        VK_CHECK(vkQueueSubmit(m_context->graphicsQueue(), 1, &submit, rawFence));
        VK_CHECK(vkWaitForFences(device, 1, &rawFence, VK_TRUE, UINT64_MAX));
    } catch (...) {
        // The submit may have reached the queue, so drain the whole device before rethrowing:
        // the destructor is about to free m_retained, and a failed wait says nothing about
        // whether the GPU is done reading the staging buffers. (Ignore the wait-idle result; on
        // device-lost there's nothing better.)
        vkDeviceWaitIdle(device);
        throw;
    }

    m_pool.reset(); // also frees the command buffer
    m_cmd = VK_NULL_HANDLE;
    m_retained.clear(); // the GPU is done reading the staging buffers; release them now
}

void submitImmediate(VulkanContext& context, const std::function<void(VkCommandBuffer)>& recorder) {
    ImmediateBatch batch(context);
    recorder(batch.commandBuffer());
    batch.submitAndWait();
}

} // namespace pose

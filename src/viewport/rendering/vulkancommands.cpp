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
    VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &m_pool));

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

ImmediateBatch::~ImmediateBatch() {
    if (!m_submitted) {
        try {
            submitAndWait();
        } catch (...) {
            // Never let an exception escape a destructor. submitAndWait() flips m_submitted first,
            // so this won't loop; the pool is freed below as a fallback.
        }
    }
    if (m_pool != VK_NULL_HANDLE && m_context != nullptr) {
        vkDestroyCommandPool(m_context->device(), m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
}

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
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &fence));

    VK_CHECK(vkQueueSubmit(m_context->graphicsQueue(), 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, m_pool, nullptr); // also frees the command buffer
    m_pool = VK_NULL_HANDLE;
    m_cmd = VK_NULL_HANDLE;
    m_retained.clear(); // the GPU is done reading the staging buffers; release them now
}

void submitImmediate(VulkanContext& context, const std::function<void(VkCommandBuffer)>& recorder) {
    ImmediateBatch batch(context);
    recorder(batch.commandBuffer());
    batch.submitAndWait();
}

} // namespace pose

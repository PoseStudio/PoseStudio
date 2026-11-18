/**
 * @file gpuframetimer.cpp
 * @brief Implementation of GpuFrameTimer. See gpuframetimer.h.
 */

#include "gpuframetimer.h"

#include "vulkancommon.h"
#include "vulkancontext.h"

#include <array>

namespace pose {

GpuFrameTimer::GpuFrameTimer(VulkanContext& context, uint32_t slotCount)
    : m_context(context), m_written(slotCount, false) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_context.physicalDevice(), &props);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_context.physicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_context.physicalDevice(), &familyCount, families.data());
    const uint32_t family = m_context.graphicsFamily();
    if (family >= familyCount || families[family].timestampValidBits == 0 || props.limits.timestampPeriod <= 0.0f) {
        return; // a queue without timestamps: the readouts print no GPU numbers
    }
    m_periodNs = props.limits.timestampPeriod;
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kQueries * slotCount;
    VkQueryPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateQueryPool(m_context.device(), &info, nullptr, &pool));
    m_pool = UniqueQueryPool(m_context.device(), pool);
    m_enabled = true;
}

void GpuFrameTimer::begin(VkCommandBuffer cmd, uint32_t slot) {
    m_slot = slot;
    m_pass = 0;
    if (!m_enabled) {
        return;
    }
    const uint32_t first = slot * kQueries;
    vkCmdResetQueryPool(cmd, m_pool.get(), first, kQueries);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_pool.get(), first);
    m_written[slot] = true;
}

void GpuFrameTimer::beginSplats(VkCommandBuffer cmd) {
    if (!m_enabled || m_pass >= kSplatPasses) {
        return; // a third scene pass stamps nothing
    }
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool.get(),
                        m_slot * kQueries + 1 + 2 * m_pass);
}

void GpuFrameTimer::endSplats(VkCommandBuffer cmd) {
    if (m_enabled && m_pass < kSplatPasses) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool.get(),
                            m_slot * kQueries + 2 + 2 * m_pass);
    }
    ++m_pass;
}

void GpuFrameTimer::end(VkCommandBuffer cmd) {
    if (!m_enabled) {
        return;
    }
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool.get(),
                        m_slot * kQueries + kQueries - 1);
}

void GpuFrameTimer::read(uint32_t slot, GpuTimings& into) {
    if (!m_enabled || !m_written[slot]) {
        return;
    }
    // Each query's value and availability, as a pair of 64-bit words.
    std::array<uint64_t, kQueries * 2> results{};
    const VkResult r = vkGetQueryPoolResults(m_context.device(), m_pool.get(), slot * kQueries, kQueries,
                                             sizeof(results), results.data(), sizeof(uint64_t) * 2,
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY) {
        VK_CHECK(r);
    }
    auto available = [&](uint32_t q) { return results[q * 2 + 1] != 0; };
    auto value = [&](uint32_t q) { return results[q * 2]; };
    if (!available(0) || !available(kQueries - 1)) {
        return; // the frame's own stamps are not both in (a frame still running): keep the last reading
    }
    const double toMs = static_cast<double>(m_periodNs) * 1.0e-6;
    into.frameMs = static_cast<double>(value(kQueries - 1) - value(0)) * toMs;
    into.splatMs = 0.0;
    for (uint32_t pass = 0; pass < kSplatPasses; ++pass) {
        const uint32_t b = 1 + 2 * pass;
        if (available(b) && available(b + 1)) {
            into.splatMs += static_cast<double>(value(b + 1) - value(b)) * toMs;
        }
    }
    into.valid = true;
}

} // namespace pose

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file descriptorutil.h
 * @brief Header-only helpers that collapse Vulkan's descriptor boilerplate.
 *
 * Creating a set layout, sizing a pool, allocating sets and writing sampler/buffer descriptors
 * each take a dozen lines of struct filling that were repeated for every set the scene binds
 * (camera, material, pose, IBL, post). These helpers express each step as one call while
 * keeping the exact Vulkan semantics — nothing is cached or deferred, and every VkResult still
 * goes through VK_CHECK. Layouts and pools come back as RAII owners (vulkanhandles.h).
 */

#ifndef DESCRIPTORUTIL_H
#define DESCRIPTORUTIL_H

#include "vulkancommon.h"
#include "vulkanhandles.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace pose {

/// One layout binding: `count` descriptors of `type` at `binding`, visible to `stages`.
inline VkDescriptorSetLayoutBinding descriptorBinding(uint32_t binding, VkDescriptorType type,
                                                      VkShaderStageFlags stages,
                                                      uint32_t count = 1) {
    VkDescriptorSetLayoutBinding b{};
    b.binding = binding;
    b.descriptorType = type;
    b.descriptorCount = count;
    b.stageFlags = stages;
    return b;
}

/// `count` consecutive bindings (0..count-1) of the same type and stages — the common shape of a
/// "several samplers" or "several storage buffers" set.
inline std::vector<VkDescriptorSetLayoutBinding> uniformBindings(uint32_t count, VkDescriptorType type,
                                                                 VkShaderStageFlags stages) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(count);
    for (uint32_t i = 0; i < count; ++i) bindings.push_back(descriptorBinding(i, type, stages));
    return bindings;
}

/// Creates a descriptor set layout from a binding list.
inline UniqueDescriptorSetLayout createDescriptorSetLayout(
    VkDevice device, const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout));
    return UniqueDescriptorSetLayout(device, layout);
}

/// A pool sized for exactly `maxSets` sets drawing from the given per-type totals.
inline UniqueDescriptorPool createDescriptorPool(VkDevice device,
                                                 std::initializer_list<VkDescriptorPoolSize> sizes,
                                                 uint32_t maxSets) {
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    info.pPoolSizes = sizes.begin();
    info.maxSets = maxSets;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &info, nullptr, &pool));
    return UniqueDescriptorPool(device, pool);
}

/// Convenience for the pool-size initializer list.
inline VkDescriptorPoolSize poolSize(VkDescriptorType type, uint32_t count) {
    VkDescriptorPoolSize s{};
    s.type = type;
    s.descriptorCount = count;
    return s;
}

/// Allocates `count` sets of one layout from `pool` into `out` (which must hold `count` slots).
inline void allocateDescriptorSets(VkDevice device, VkDescriptorPool pool,
                                   VkDescriptorSetLayout layout, uint32_t count,
                                   VkDescriptorSet* out) {
    const std::vector<VkDescriptorSetLayout> layouts(count, layout);
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = pool;
    info.descriptorSetCount = count;
    info.pSetLayouts = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(device, &info, out));
}

/// Allocates a single set.
inline VkDescriptorSet allocateDescriptorSet(VkDevice device, VkDescriptorPool pool,
                                             VkDescriptorSetLayout layout) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    allocateDescriptorSets(device, pool, layout, 1, &set);
    return set;
}

/// A sampled-image descriptor in the shader-read layout (the layout every sampled target and
/// texture in this engine is in when it is bound).
inline VkDescriptorImageInfo sampledImageInfo(VkImageView view, VkSampler sampler,
                                              VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    VkDescriptorImageInfo info{};
    info.imageLayout = layout;
    info.imageView = view;
    info.sampler = sampler;
    return info;
}

/// A whole-buffer descriptor (or the first `range` bytes when given).
inline VkDescriptorBufferInfo bufferInfo(VkBuffer buffer, VkDeviceSize range = VK_WHOLE_SIZE,
                                         VkDeviceSize offset = 0) {
    VkDescriptorBufferInfo info{};
    info.buffer = buffer;
    info.offset = offset;
    info.range = range;
    return info;
}

/// Writes `count` combined-image-sampler descriptors to consecutive bindings starting at
/// `firstBinding`. `infos` must stay alive for the call only.
inline void writeCombinedImageSamplers(VkDevice device, VkDescriptorSet set, uint32_t firstBinding,
                                       const VkDescriptorImageInfo* infos, uint32_t count) {
    std::vector<VkWriteDescriptorSet> writes(count);
    for (uint32_t i = 0; i < count; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = firstBinding + i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, count, writes.data(), 0, nullptr);
}

/// Writes `count` buffer descriptors of `type` (uniform or storage) to consecutive bindings.
inline void writeBufferDescriptors(VkDevice device, VkDescriptorSet set, uint32_t firstBinding,
                                   VkDescriptorType type, const VkDescriptorBufferInfo* infos,
                                   uint32_t count) {
    std::vector<VkWriteDescriptorSet> writes(count);
    for (uint32_t i = 0; i < count; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = firstBinding + i;
        writes[i].descriptorType = type;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, count, writes.data(), 0, nullptr);
}

} // namespace pose

#endif // DESCRIPTORUTIL_H

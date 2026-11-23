// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file shadowmap.h
 * @brief The key light's shadow map: a depth-only offscreen target + the comparison sampler
 *        the lit shaders test against.
 *
 * Owns the D32 depth image, its single-sample depth-only render pass (independent of the scene
 * pass's MSAA count), the framebuffer, and borrows a comparison (PCF) sampler + a raw-depth
 * twin from the context's SamplerCache. Scene records the depth-only pass into it each frame
 * (recordShadowPass) before the main pass samples it — the mesh shader through set 3 binding 2
 * (shadowing the key light on the figure), the grid shader through the same set (the
 * ground/contact shadow). The sampler clamps to an opaque-white border, so any receiver outside
 * the fitted light frustum simply reads "unshadowed".
 *
 * At creation the image is cleared to 1.0 and moved to the read layout, so frames before the
 * first shadow pass sample a defined, fully-lit map. When Scene SKIPS the pass (no casters, or
 * the shadow dial off) the map is NOT re-cleared — it keeps whatever the last pass wrote; what
 * makes receivers read lit then is the degenerate light matrix Scene::recordShadowPass parks
 * in the camera UBO, which projects every receiver outside the map's depth range.
 *
 * Like the rest of rendering/, pure Vulkan + std — no Qt.
 */

#ifndef SHADOWMAP_H
#define SHADOWMAP_H

#include "attachmentimage.h"
#include "vulkanhandles.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace pose {

class VulkanContext;

class ShadowMap {
public:
    explicit ShadowMap(VulkanContext& context, uint32_t size = 2048);
    ~ShadowMap() = default; // members clean up in reverse declaration order

    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    VkRenderPass  renderPass() const { return m_renderPass.get(); }
    VkFramebuffer framebuffer() const { return m_framebuffer.get(); }
    uint32_t      size() const { return m_size; }

    /// Combined-image-sampler descriptor info (read layout + the comparison sampler) for the
    /// shaders' `sampler2DShadow` binding (set 3 binding 2 in the mesh path; the grid shares it).
    VkDescriptorImageInfo descriptorInfo() const;

    /// Same image through the NON-comparison sampler (set 3 binding 3): raw depth values, for the
    /// ground shadow's PCSS blocker search (a comparison sampler can only answer lit/unlit, not
    /// "how far above the floor is the caster"). Border reads 1.0 = empty/far, i.e. no blocker.
    VkDescriptorImageInfo rawDescriptorInfo() const;

private:
    VulkanContext&    m_context;
    uint32_t          m_size = 0;
    UniqueRenderPass  m_renderPass;
    AttachmentImage   m_image;
    VkSampler         m_sampler = VK_NULL_HANDLE;    // comparison (PCF); borrowed from the SamplerCache
    VkSampler         m_rawSampler = VK_NULL_HANDLE; // no compare: raw depths for the blocker search
    UniqueFramebuffer m_framebuffer; // last: references the view + pass above
};

} // namespace pose

#endif // SHADOWMAP_H

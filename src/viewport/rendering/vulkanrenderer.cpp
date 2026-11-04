/**
 * @file vulkanrenderer.cpp
 * @brief Implementation of the per-frame rendering loop. See vulkanrenderer.h.
 */

#include "vulkanrenderer.h"

#include "hdrtarget.h"
#include "outlinemask.h"
#include "postprocess.h"
#include "shaderlibrary.h"
#include "vulkancontext.h"
#include "vulkanbuffer.h"
#include "vulkanswapchain.h"

#include "grid.h"
#include "scene.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace pose {

namespace {

UniqueSemaphore makeSemaphore(VkDevice device) {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSemaphore(device, &info, nullptr, &semaphore));
    return UniqueSemaphore(device, semaphore);
}

UniqueFence makeSignalledFence(VkDevice device) {
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    info.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first wait doesn't block forever
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &info, nullptr, &fence));
    return UniqueFence(device, fence);
}

/// Sets the dynamic viewport + scissor to the whole of @p extent.
void setFullViewport(VkCommandBuffer cmd, VkExtent2D extent) {
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

// The outline's width is 2 logical pixels — Blender's default outline weight — scaled to
// physical pixels by the window's device pixel ratio (desktop) or the eye image's height (VR).
constexpr float kOutlineWidthLogicalPx = 2.0f;

} // namespace

/// VR Mode's targets (see beginVr): the eye-sized scene targets and post chain, the headset
/// composite's render pass (borrowed from the session), and the VR frame's own command buffer +
/// fence — one VR frame in flight, independent of the desktop frames' slots.
struct VulkanRenderer::VrTargets {
    VkRenderPass                 renderPass = VK_NULL_HANDLE; // the session's, over its images
    VkExtent2D                   extent{};
    bool                         encodeSrgb = false;
    float                        outlineWidthPx = kOutlineWidthLogicalPx;
    bool                         hasFrame = false;     // a stereo frame has rendered: the eye targets hold one
    bool                         lastOutlined = false; // ... with an outline mask
    std::unique_ptr<HdrTarget>   hdr;
    std::unique_ptr<OutlineMask> outline;
    std::unique_ptr<PostProcess> post;
    VkCommandBuffer              cmd = VK_NULL_HANDLE; // from the renderer's pool
    UniqueFence                  fence;
};

VulkanRenderer::VulkanRenderer(VulkanContext& context, VkExtent2D initialExtent,
                               std::string shaderDir)
    : m_context(context), m_windowExtent(initialExtent) {
    // Every compiled shader is read up front: a missing .spv fails here with one clear message
    // (the usual cause is the shader-compile build step not having run) rather than partway
    // through pipeline construction. The blobs are kept: VR Mode builds a second post chain
    // from them later (beginVr).
    m_shaders = std::make_unique<ShaderLibrary>(std::move(shaderDir));
    ShaderLibrary& shaders = *m_shaders;
    shaders.preload();

    m_swapchain = std::make_unique<VulkanSwapchain>(m_context, m_windowExtent);

    // The scene renders offscreen into the HDR target (linear RGBA16F at the context's MSAA
    // count, resolved); scene pipelines are built against ITS render pass. The swapchain pass —
    // single-sample, no depth — then only ever runs the tonemapping composite.
    m_hdrTarget = std::make_unique<HdrTarget>(m_context, m_swapchain->extent());
    // The selection-outline mask is screen-sized too; the Scene's silhouette pipeline builds
    // against its pass, so it exists before the Scene.
    m_outlineMask = std::make_unique<OutlineMask>(m_context, m_swapchain->extent());

    m_scene = std::make_unique<Scene>(m_context, m_hdrTarget->renderPass(),
                                      m_outlineMask->renderPass(),
                                      shaders.get("mesh.vert"), shaders.get("mesh.frag"),
                                      shaders.get("skeleton.vert"), shaders.get("skeleton.frag"),
                                      shaders.get("shadow.vert"), shaders.get("shadow.frag"),
                                      shaders.get("background.vert"),
                                      shaders.get("background.frag"),
                                      shaders.get("outlinemask.frag"));

    // The grid samples the scene's shadow map (ground/contact shadow) through the scene-wide
    // set 3, so its pipeline is built against that layout — Scene must exist first.
    m_grid = std::make_unique<Grid>(m_context, m_hdrTarget->renderPass(),
                                    shaders.get("grid.vert"), shaders.get("grid.frag"),
                                    m_scene->iblSetLayout());

    m_postProcess = std::make_unique<PostProcess>(
        m_context, m_swapchain->renderPass(), m_hdrTarget->resolveInfo(),
        m_hdrTarget->specResolveInfo(), m_outlineMask->descriptorInfo(), m_swapchain->extent(),
        shaders.get("fullscreen.vert"), shaders.get("bloombright.frag"),
        shaders.get("bloomblur.frag"), shaders.get("composite.frag"),
        shaders.get("sssblur.frag"));

    createCommandPool();
    createCommandBuffers();
    createSyncObjects();

    const VkExtent2D extent = m_swapchain->extent();
    m_camera.setViewportSize(static_cast<float>(extent.width), static_cast<float>(extent.height));
}

VulkanRenderer::~VulkanRenderer() {
    // Nothing in flight may reference these objects when we destroy them. The sync objects and
    // the command pool are RAII members, released after this body; the surface-dependent objects
    // are released here in dependency order (consumers before the targets/passes they sample or
    // were built against).
    vkDeviceWaitIdle(m_context.device());

    if (m_vr) {
        m_postProcess->clearMirrorSource(); // it points at the eye targets about to go
        m_vr.reset();
    }
    m_postProcess.reset();
    m_grid.reset();
    m_scene.reset();
    m_outlineMask.reset();
    m_hdrTarget.reset();
    m_swapchain.reset();
}

void VulkanRenderer::beginVr(VkRenderPass eyeRenderPass, VkExtent2D eyeExtent, bool encodeSrgb) {
    endVr();
    // Nothing may be in flight: the VR frames write the per-frame UBO slots the desktop frames
    // were using, and from here on the desktop frame only composites (the mirror).
    vkDeviceWaitIdle(m_context.device());

    auto vr = std::make_unique<VrTargets>();
    vr->renderPass = eyeRenderPass;
    vr->extent = eyeExtent;
    vr->encodeSrgb = encodeSrgb;
    // A headset image is ~2000 px tall; the 2-logical-pixel outline is scaled against a 1080p
    // frame so it stays a visible hairline in the headset and in the mirror.
    vr->outlineWidthPx = kOutlineWidthLogicalPx * std::max(1.0f, static_cast<float>(eyeExtent.height) / 1080.0f);
    // The same pass structure as the desktop targets (same formats, same MSAA count), so every
    // scene pipeline — built against the desktop targets' passes — is compatible with these.
    vr->hdr = std::make_unique<HdrTarget>(m_context, eyeExtent);
    vr->outline = std::make_unique<OutlineMask>(m_context, eyeExtent);
    ShaderLibrary& shaders = *m_shaders;
    vr->post = std::make_unique<PostProcess>(
        m_context, eyeRenderPass, vr->hdr->resolveInfo(), vr->hdr->specResolveInfo(),
        vr->outline->descriptorInfo(), eyeExtent, shaders.get("fullscreen.vert"),
        shaders.get("bloombright.frag"), shaders.get("bloomblur.frag"), shaders.get("composite.frag"),
        shaders.get("sssblur.frag"));

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_commandPool.get();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(m_context.device(), &ai, &vr->cmd));
    vr->fence = makeSignalledFence(m_context.device());

    // The desktop composite mirrors the eye targets from now on.
    m_postProcess->setMirrorSource(vr->hdr->resolveInfo(), vr->post->bloomInfo(), vr->outline->descriptorInfo());
    m_vr = std::move(vr);
}

void VulkanRenderer::endVr() {
    if (!m_vr) {
        return;
    }
    vkDeviceWaitIdle(m_context.device());
    m_postProcess->clearMirrorSource();
    if (m_vr->cmd != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(m_context.device(), m_commandPool.get(), 1, &m_vr->cmd);
    }
    m_vr.reset();
}

void VulkanRenderer::drawVrFrame(const VrEyeView views[2], const VkFramebuffer eyeFramebuffers[2]) {
    if (!m_vr) {
        return;
    }
    static_assert(kMaxFramesInFlight >= 2, "each eye writes its own per-frame UBO slot");
    VkDevice device = m_context.device();
    const VkFence fence = m_vr->fence.get();
    // One VR frame in flight: the previous frame's command buffer and the UBO slots both eyes
    // write must be free before this one records — and so must the desktop frames' (a desktop
    // frame recorded before the first stereo frame still renders the scene through those slots;
    // afterwards it only composites the mirror and these waits are free).
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    for (const UniqueFence& desktopFence : m_inFlightFences) {
        const VkFence handle = desktopFence.get();
        VK_CHECK(vkWaitForFences(device, 1, &handle, VK_TRUE, UINT64_MAX));
    }

    VkCommandBuffer cmd = m_vr->cmd;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    // The shadow map is view-independent: once per frame, shared by both eyes.
    m_scene->recordShadowPass(cmd, 0);

    const bool pbr = m_scene->isPbr();
    const bool tonemap = pbr && m_scene->lightingSettings().tonemap;
    bool outlined = false;
    // Right eye first, LEFT eye last: the eye targets are reused by both eyes in sequence, so
    // they end the frame holding the left eye — the one the desktop mirror composites.
    for (int eye = 1; eye >= 0; --eye) {
        Camera eyeCamera;
        eyeCamera.setExplicitView(views[eye].view, views[eye].projection);
        outlined = recordScenePasses(cmd, eyeCamera, static_cast<uint32_t>(eye), *m_vr->hdr, *m_vr->outline,
                                     *m_vr->post, m_vr->extent);

        VkRenderPassBeginInfo eyePass{};
        eyePass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        eyePass.renderPass = m_vr->renderPass;
        eyePass.framebuffer = eyeFramebuffers[eye];
        eyePass.renderArea.extent = m_vr->extent;
        vkCmdBeginRenderPass(cmd, &eyePass, VK_SUBPASS_CONTENTS_INLINE);
        setFullViewport(cmd, m_vr->extent);
        PostProcess::CompositeOptions options;
        options.encodeSrgb = m_vr->encodeSrgb;
        m_vr->post->recordComposite(cmd, tonemap, pbr, outlined ? m_vr->outlineWidthPx : 0.0f, options);
        vkCmdEndRenderPass(cmd);
    }
    m_vr->lastOutlined = outlined;
    m_vr->hasFrame = true;
    VK_CHECK(vkEndCommandBuffer(cmd));

    VK_CHECK(vkResetFences(device, 1, &fence));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(m_context.graphicsQueue(), 1, &submit, fence));
}

void VulkanRenderer::notifyResize(VkExtent2D newExtent) {
    // Expose events report the current size too — only a real change needs a swapchain rebuild,
    // otherwise every un-obscure of the window would trigger a device-idle + full rebuild.
    if (newExtent.width == m_windowExtent.width && newExtent.height == m_windowExtent.height) {
        return;
    }
    m_windowExtent = newExtent;
    m_framebufferResized = true;
}

void VulkanRenderer::addModel(const ModelData& data) {
    m_scene->addModel(data);
}

void VulkanRenderer::deleteModel(std::size_t index) {
    // The model's buffers and descriptor sets may be referenced by command buffers still in
    // flight; wait for the device to finish before the scene frees them.
    vkDeviceWaitIdle(m_context.device());
    m_scene->removeModel(index);
}

void VulkanRenderer::clearModels() {
    vkDeviceWaitIdle(m_context.device()); // same in-flight-frame guard as deleteModel
    m_scene->clearModels();
}

bool VulkanRenderer::frameSelected() {
    glm::vec3 mn;
    glm::vec3 mx;
    if (!m_scene || !m_scene->framingBounds(mn, mx)) {
        return false;
    }
    // The box's bounding sphere: fits from every orbit angle, so the frame never clips a corner
    // as the user orbits on from it.
    const glm::vec3 center = 0.5f * (mn + mx);
    const float radius = std::max(0.5f * glm::length(mx - mn), 0.05f);
    m_camera.frame(center, radius);
    return true;
}

void VulkanRenderer::applyBakedEnvironment(const BakedEnvironment& baked) {
    if (m_scene) {
        m_scene->applyBakedEnvironment(baked);
    }
}

void VulkanRenderer::createCommandPool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // we reset+rerecord each frame
    ci.queueFamilyIndex = m_context.graphicsFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(m_context.device(), &ci, nullptr, &pool));
    m_commandPool.reset(m_context.device(), pool);
}

void VulkanRenderer::createCommandBuffers() {
    m_commandBuffers.resize(kMaxFramesInFlight);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_commandPool.get();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());
    VK_CHECK(vkAllocateCommandBuffers(m_context.device(), &ai, m_commandBuffers.data()));
}

void VulkanRenderer::createSyncObjects() {
    VkDevice device = m_context.device();
    m_imageAvailableSemaphores.clear();
    m_inFlightFences.clear();
    m_renderFinishedSemaphores.clear();
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        m_imageAvailableSemaphores.push_back(makeSemaphore(device));
        m_inFlightFences.push_back(makeSignalledFence(device));
    }
    for (uint32_t i = 0; i < m_swapchain->imageCount(); ++i) {
        m_renderFinishedSemaphores.push_back(makeSemaphore(device));
    }
}

void VulkanRenderer::recreateSwapchain() {
    // Skip while minimised — a zero-sized swapchain is invalid; we'll rebuild once the
    // window has real extent again.
    if (m_windowExtent.width == 0 || m_windowExtent.height == 0) {
        return;
    }
    // The cached Qt extent can lag the surface: if the window was minimised between an update
    // request and this rebuild (acquire/present returned OUT_OF_DATE while m_windowExtent is
    // still the pre-minimise size), the SURFACE reports 0x0 — and creating a swapchain with a
    // zero imageExtent is a spec violation. Check the authoritative source before rebuilding.
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_context.physicalDevice(), m_context.surface(),
                                                  &caps) == VK_SUCCESS &&
        caps.currentExtent.width != std::numeric_limits<uint32_t>::max() &&
        (caps.currentExtent.width == 0 || caps.currentExtent.height == 0)) {
        return; // surface currently has no area; the next real resize/expose rebuilds
    }
    vkDeviceWaitIdle(m_context.device());

    const uint32_t oldImageCount = m_swapchain->imageCount();
    m_swapchain->recreate(m_windowExtent);

    // The per-image renderFinished semaphores must match the (possibly new) image count.
    if (m_swapchain->imageCount() != oldImageCount) {
        m_renderFinishedSemaphores.clear();
        for (uint32_t i = 0; i < m_swapchain->imageCount(); ++i) {
            m_renderFinishedSemaphores.push_back(makeSemaphore(m_context.device()));
        }
    }

    const VkExtent2D extent = m_swapchain->extent();
    // The offscreen HDR target, the outline mask, and the bloom targets track the swapchain size
    // (the device is idle here).
    m_hdrTarget->resize(extent);
    m_outlineMask->resize(extent);
    m_postProcess->resize(extent, m_hdrTarget->resolveInfo(), m_hdrTarget->specResolveInfo(),
                          m_outlineMask->descriptorInfo());
    m_camera.setViewportSize(static_cast<float>(extent.width), static_cast<float>(extent.height));
    m_framebufferResized = false;
}

bool VulkanRenderer::drawFrame() {
    if (m_windowExtent.width == 0 || m_windowExtent.height == 0) {
        return false; // minimised: nothing to draw (the next expose/resize re-arms drawing)
    }

    VkDevice device = m_context.device();
    const VkFence inFlight = m_inFlightFences[m_currentFrame].get();
    const VkSemaphore imageAvailable = m_imageAvailableSemaphores[m_currentFrame].get();
    VK_CHECK(vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX));

    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(device, m_swapchain->handle(), UINT64_MAX,
                                             imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return true; // this frame was skipped; the caller must schedule one at the new size
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        VK_CHECK(acquire);
    }

    // FRAME CAPTURE (requestCapture): this frame's composited image is copied into a host buffer.
    const VkExtent2D captureExtent = m_swapchain->extent();
    const bool capturing = m_captureRequested && m_swapchain->readable();
    VulkanBuffer captureBuffer;
    if (capturing) {
        captureBuffer = VulkanBuffer(m_context, static_cast<VkDeviceSize>(captureExtent.width) * captureExtent.height * 4,
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }

    VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    recordCommandBuffer(cmd, imageIndex, capturing ? captureBuffer.handle() : VK_NULL_HANDLE);

    // Reset the fence only once we're committing to a submit that will re-signal it: an early-out
    // above — or a throw during recording — must leave it signalled, because an unsignalled fence
    // with no pending submit deadlocks the next frame's infinite vkWaitForFences.
    VK_CHECK(vkResetFences(device, 1, &inFlight));

    const VkSemaphore renderFinished = m_renderFinishedSemaphores[imageIndex].get();
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished;
    VK_CHECK(vkQueueSubmit(m_context.graphicsQueue(), 1, &submit, inFlight));
    if (capturing) {
        // (A diagnostic path: waiting for the frame here is its whole cost.)
        m_captureRequested = false;
        VK_CHECK(vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX));
        captureBuffer.invalidate();
        const VkFormat format = m_swapchain->colorFormat();
        m_capture.width = captureExtent.width;
        m_capture.height = captureExtent.height;
        m_capture.bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
        const auto* bytes = static_cast<const unsigned char*>(captureBuffer.mappedData());
        const std::size_t count = static_cast<std::size_t>(captureExtent.width) * captureExtent.height * 4;
        if (bytes != nullptr) {
            m_capture.pixels.assign(bytes, bytes + count);
        } else {
            m_capture.pixels.clear();
        }
    }

    VkSwapchainKHR swapchain = m_swapchain->handle();
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;

    bool needsRedraw = false;
    VkResult presentResult = vkQueuePresentKHR(m_context.presentQueue(), &present);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR ||
        m_framebufferResized) {
        recreateSwapchain();
        needsRedraw = true; // the presented frame predates the rebuild — draw once at the new size
    } else if (presentResult != VK_SUCCESS) {
        VK_CHECK(presentResult);
    }

    m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;
    return needsRedraw;
}

/**
 * @brief THE FRAME GRAPH: records every pass of one frame, in order, into @p cmd.
 *
 *   1. Key-light SHADOW map (Scene::recordShadowPass — depth-only, its own pass on the ShadowMap;
 *      skipped with no casters, the light matrix then parked on an always-lit projection).
 *   2. Selection OUTLINE mask (Scene::recordOutlinePass — the selected model's silhouette into the
 *      OutlineMask; skipped with no selection).
 *   3. Offscreen HDR SCENE pass (HdrTarget, at the context's MSAA count, in LINEAR HDR):
 *      the HDRI backdrop -> opaque meshes -> transparent meshes (+ wire/hidden-line variants per
 *      shade mode) -> the floor grid -> the line overlays (skeleton, pins, ortho floor line);
 *      resolved into the sampleable colour + specular images.
 *   4. Screen-space SSS blur (H into a scratch target, V back into the HDR resolve) and then BLOOM
 *      (half-res bright-extract + separable blur) — PBR mode only.
 *   5. Swapchain pass (single-sample, no depth): the fullscreen COMPOSITE — HDR + bloom, ACES
 *      when tonemapping, the selection outline over the top — into the acquired swapchain image.
 *
 * Every pass's inter-pass synchronisation is expressed by its render pass's external
 * dependencies (renderpassbuilder.h), not by barriers here.
 */
bool VulkanRenderer::takeCapture(CapturedFrame& out) {
    if (m_capture.pixels.empty()) {
        return false;
    }
    out = std::move(m_capture);
    m_capture = CapturedFrame{};
    return true;
}

bool VulkanRenderer::recordScenePasses(VkCommandBuffer cmd, const Camera& camera, uint32_t frameSlot,
                                       HdrTarget& hdr, OutlineMask& outline, PostProcess& post,
                                       VkExtent2D extent) {
    // Selection-outline mask (its own small pass, skipped when nothing is selected): the
    // composite dilates it into the outline, so it too must precede the composite's pass.
    const bool outlined = m_scene->recordOutlinePass(cmd, camera, frameSlot, outline);

    // --- Offscreen HDR scene pass: backdrop + meshes + grid, in LINEAR HDR. --------------------
    // Five clear slots cover every attachment layout the HDR target uses (MSAA:
    // colourMS/depth/resolve/specMS/specResolve; single-sample: colour/depth/spec) — the
    // SPECULAR attachments clear to zero (no specular where nothing draws). The swapchain pass
    // below needs none: its one attachment loads DONT_CARE.
    std::array<VkClearValue, 5> clears{};
    // Viewport background #3E4042 = (62, 64, 66) as LINEAR values (sRGB->linear of each channel).
    // The HDR target is a linear format and the composite's sRGB swapchain store does the final
    // encode, so the displayed pixel comes back out as exactly #3E4042. Alpha clears to 0: the
    // HDR target's alpha is the SSS mask, and empty pixels are not skin.
    clears[0].color = {{0.04816f, 0.05125f, 0.05447f, 0.0f}};
    clears[1].depthStencil = {1.0f, 0};
    clears[2].color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // single-sample: the spec attachment
    clears[3].color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // MSAA: the spec MS attachment
    clears[4].color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderPassBeginInfo scenePass{};
    scenePass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    scenePass.renderPass = hdr.renderPass();
    scenePass.framebuffer = hdr.framebuffer();
    scenePass.renderArea.extent = extent;
    scenePass.clearValueCount = hdr.attachmentCount();
    scenePass.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &scenePass, VK_SUBPASS_CONTENTS_INLINE);
    setFullViewport(cmd, extent);

    // Imported meshes first (opaque, depth test+write), then the grid — a transparent overlay
    // that depth-tests against scene geometry and blends; the scene's set 3 + light matrix +
    // shadow dials feed its PCSS ground shadow.
    m_scene->record(cmd, camera, frameSlot);
    const LightingSettings& lighting = m_scene->lightingSettings();
    m_grid->record(cmd, camera, m_scene->iblSet(), m_scene->lightViewProj(),
                   glm::vec4(lighting.shadowIntensity, lighting.shadowSoftness,
                             lighting.shadowReach, 0.0f));
    vkCmdEndRenderPass(cmd);

    // --- SSSSS then bloom (PBR mode only — the stylized modes author display-ready values). ----
    if (m_scene->isPbr()) {
        post.recordSss(cmd);   // diffuses skin in place (HDR alpha = the SSS mask)
        post.recordBloom(cmd); // then blooms the diffused image
    }
    return outlined;
}

void VulkanRenderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, VkBuffer captureInto) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    const VkExtent2D extent = m_swapchain->extent();
    const bool pbr = m_scene->isPbr();
    const bool tonemap = pbr && m_scene->lightingSettings().tonemap;
    float outlineWidthPx = 0.0f;
    PostProcess::CompositeOptions composite;

    if (m_vr && m_vr->hasFrame) {
        // VR MODE: the desktop window is a MIRROR of the headset's left eye (drawVrFrame leaves
        // the eye targets holding it). No scene passes here — the eye frame already rendered
        // them — just the composite, cropped to the window's aspect so the near-square eye
        // image is neither stretched nor letterboxed. (Until the first stereo frame has
        // rendered, the eye targets hold nothing sampleable: the desktop renders itself.)
        composite.fromMirror = true;
        const float eyeAspect = static_cast<float>(m_vr->extent.width) / static_cast<float>(m_vr->extent.height);
        const float windowAspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        if (windowAspect > eyeAspect) {
            composite.uvScale[1] = eyeAspect / windowAspect; // wider window: crop the eye's top/bottom
            composite.uvOffset[1] = 0.5f * (1.0f - composite.uvScale[1]);
        } else {
            composite.uvScale[0] = windowAspect / eyeAspect; // taller window: crop the sides
            composite.uvOffset[0] = 0.5f * (1.0f - composite.uvScale[0]);
        }
        outlineWidthPx = m_vr->lastOutlined ? m_vr->outlineWidthPx : 0.0f;
    } else {
        // Key-light shadow pass first (its own depth-only render pass on the shadow map), so the
        // scene pass can sample the fresh map. Also fits this frame's light matrix, which
        // Scene::record() writes into the camera UBO.
        m_scene->recordShadowPass(cmd, m_currentFrame);
        const bool outlined = recordScenePasses(cmd, m_camera, m_currentFrame, *m_hdrTarget, *m_outlineMask,
                                                *m_postProcess, extent);
        outlineWidthPx = outlined ? kOutlineWidthLogicalPx * m_uiScale : 0.0f;
    }

    // --- Swapchain pass: the fullscreen composite (tonemap + bloom add + outline). -------------
    // No clear values: the pass's single colour attachment loads DONT_CARE, since the composite's
    // fullscreen triangle overwrites every pixel.
    VkRenderPassBeginInfo presentPass{};
    presentPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    presentPass.renderPass = m_swapchain->renderPass();
    presentPass.framebuffer = m_swapchain->framebuffer(imageIndex);
    presentPass.renderArea.extent = extent;
    presentPass.clearValueCount = 0;
    presentPass.pClearValues = nullptr;
    vkCmdBeginRenderPass(cmd, &presentPass, VK_SUBPASS_CONTENTS_INLINE);
    setFullViewport(cmd, extent);
    m_postProcess->recordComposite(cmd, tonemap, pbr, outlineWidthPx, composite);
    vkCmdEndRenderPass(cmd);

    if (captureInto != VK_NULL_HANDLE) {
        // FRAME CAPTURE: the pass left the image in its present layout — to a copy source, into the
        // host buffer, and back.
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_swapchain->image(imageIndex);
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureInto, 1, &region);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    }

    VK_CHECK(vkEndCommandBuffer(cmd));
}

} // namespace pose

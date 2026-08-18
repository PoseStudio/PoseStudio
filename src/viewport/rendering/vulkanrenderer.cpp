/**
 * @file vulkanrenderer.cpp
 * @brief Implementation of the per-frame rendering loop. See vulkanrenderer.h.
 */

#include "vulkanrenderer.h"

#include "hdrtarget.h"
#include "postprocess.h"
#include "vulkancontext.h"
#include "vulkanswapchain.h"

#include "grid.h"
#include "scene.h"

#include <array>
#include <cstdint>
#include <fstream>

namespace pose {

VulkanRenderer::VulkanRenderer(VulkanContext& context, VkExtent2D initialExtent,
                               std::string shaderDir)
    : m_context(context), m_shaderDir(std::move(shaderDir)), m_windowExtent(initialExtent) {
    m_swapchain = std::make_unique<VulkanSwapchain>(m_context, m_windowExtent);

    // The scene renders offscreen into the HDR target (linear RGBA16F, same MSAA structure as the
    // swapchain pass); scene pipelines are built against ITS render pass. The swapchain pass then
    // only ever runs the tonemapping composite.
    m_hdrTarget = std::make_unique<HdrTarget>(m_context, m_swapchain->extent());

    m_scene = std::make_unique<Scene>(m_context, m_hdrTarget->renderPass(),
                                      loadSpirv("mesh.vert.spv"), loadSpirv("mesh.frag.spv"),
                                      loadSpirv("skeleton.vert.spv"), loadSpirv("skeleton.frag.spv"),
                                      loadSpirv("shadow.vert.spv"), loadSpirv("shadow.frag.spv"),
                                      loadSpirv("background.vert.spv"),
                                      loadSpirv("background.frag.spv"));

    // The grid samples the scene's shadow map (ground/contact shadow) through the scene-wide
    // set 3, so its pipeline is built against that layout — Scene must exist first.
    m_grid = std::make_unique<Grid>(m_context, m_hdrTarget->renderPass(),
                                    loadSpirv("grid.vert.spv"), loadSpirv("grid.frag.spv"),
                                    m_scene->iblSetLayout());

    m_postProcess = std::make_unique<PostProcess>(
        m_context, m_swapchain->renderPass(), m_hdrTarget->resolveInfo(),
        m_hdrTarget->specResolveInfo(), m_swapchain->extent(),
        loadSpirv("fullscreen.vert.spv"), loadSpirv("bloombright.frag.spv"),
        loadSpirv("bloomblur.frag.spv"), loadSpirv("composite.frag.spv"),
        loadSpirv("sssblur.frag.spv"));

    createCommandPool();
    createCommandBuffers();
    createSyncObjects();

    const VkExtent2D extent = m_swapchain->extent();
    m_camera.setViewportSize(static_cast<float>(extent.width), static_cast<float>(extent.height));
}

VulkanRenderer::~VulkanRenderer() {
    // Nothing in flight may reference these objects when we destroy them.
    vkDeviceWaitIdle(m_context.device());

    destroySyncObjects();
    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_context.device(), m_commandPool, nullptr);
    }
    m_postProcess.reset();
    m_grid.reset();
    m_scene.reset();
    m_hdrTarget.reset();
    m_swapchain.reset();
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

int VulkanRenderer::pickModel(const Ray& ray) const {
    return m_scene->pickModel(ray);
}

void VulkanRenderer::deleteModel(std::size_t index) {
    // The model's buffers and descriptor sets may be referenced by command buffers still in
    // flight; wait for the device to finish before the scene frees them.
    vkDeviceWaitIdle(m_context.device());
    m_scene->removeModel(index);
}

bool VulkanRenderer::hasPosableFigure() const { return m_scene && m_scene->hasPosableFigure(); }

void VulkanRenderer::setShowSkeleton(bool on) {
    if (m_scene) {
        m_scene->setShowSkeleton(on);
    }
}

bool VulkanRenderer::showSkeleton() const { return m_scene && m_scene->showSkeleton(); }

void VulkanRenderer::setShadeMode(int mode) {
    if (m_scene) {
        m_scene->setShadeMode(mode);
    }
}

void VulkanRenderer::applyBakedEnvironment(const BakedEnvironment& baked) {
    if (m_scene) {
        m_scene->applyBakedEnvironment(baked);
    }
}

void VulkanRenderer::setLightingSettings(const LightingSettings& settings) {
    if (m_scene) {
        m_scene->setLightingSettings(settings);
    }
}

int VulkanRenderer::shadeMode() const { return m_scene ? m_scene->shadeMode() : 0; }

int VulkanRenderer::selectBoneAt(float px, float py, float vpW, float vpH) {
    return m_scene ? m_scene->selectBoneAt(px, py, vpW, vpH, m_camera) : -1;
}

bool VulkanRenderer::hasSelectedBone() const { return m_scene && m_scene->hasSelectedBone(); }

void VulkanRenderer::nudgeSelectedBone(const glm::vec3& deltaEulerDegrees) {
    if (m_scene) {
        m_scene->nudgeSelectedBone(deltaEulerDegrees);
    }
}

void VulkanRenderer::finalizePose() {
    if (m_scene) {
        m_scene->finalizePose();
    }
}

bool VulkanRenderer::groundFigure() {
    return m_scene ? m_scene->groundFigure() : false;
}

int VulkanRenderer::gizmoAxisAt(float px, float py, float vpW, float vpH) const {
    return m_scene ? m_scene->gizmoAxisAt(px, py, vpW, vpH, m_camera) : -1;
}

void VulkanRenderer::rotateGizmo(int axis, float prevX, float prevY, float curX, float curY,
                                 float vpW, float vpH) {
    if (m_scene) {
        m_scene->rotateGizmo(axis, prevX, prevY, curX, curY, vpW, vpH, m_camera);
    }
}

std::vector<std::pair<std::string, glm::vec3>> VulkanRenderer::capturePose() const {
    return m_scene ? m_scene->capturePose() : std::vector<std::pair<std::string, glm::vec3>>{};
}

void VulkanRenderer::applyPose(const std::vector<std::pair<std::string, glm::vec3>>& pose) {
    if (m_scene) {
        m_scene->applyPose(pose);
    }
}

bool VulkanRenderer::savePose(const std::string& path) const {
    return m_scene && m_scene->savePose(path);
}

bool VulkanRenderer::loadPose(const std::string& path) {
    return m_scene && m_scene->loadPose(path);
}

void VulkanRenderer::createCommandPool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // we reset+rerecord each frame
    ci.queueFamilyIndex = m_context.graphicsFamily();
    VK_CHECK(vkCreateCommandPool(m_context.device(), &ci, nullptr, &m_commandPool));
}

void VulkanRenderer::createCommandBuffers() {
    m_commandBuffers.resize(kMaxFramesInFlight);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());
    VK_CHECK(vkAllocateCommandBuffers(m_context.device(), &ai, m_commandBuffers.data()));
}

void VulkanRenderer::createSyncObjects() {
    VkDevice device = m_context.device();
    m_imageAvailableSemaphores.resize(kMaxFramesInFlight);
    m_inFlightFences.resize(kMaxFramesInFlight);
    m_renderFinishedSemaphores.resize(m_swapchain->imageCount());

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first wait doesn't block forever

    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(device, &semInfo, nullptr, &m_imageAvailableSemaphores[i]));
        VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &m_inFlightFences[i]));
    }
    for (auto& sem : m_renderFinishedSemaphores) {
        VK_CHECK(vkCreateSemaphore(device, &semInfo, nullptr, &sem));
    }
}

void VulkanRenderer::destroySyncObjects() {
    VkDevice device = m_context.device();
    for (VkSemaphore sem : m_imageAvailableSemaphores) {
        vkDestroySemaphore(device, sem, nullptr);
    }
    for (VkSemaphore sem : m_renderFinishedSemaphores) {
        vkDestroySemaphore(device, sem, nullptr);
    }
    for (VkFence fence : m_inFlightFences) {
        vkDestroyFence(device, fence, nullptr);
    }
    m_imageAvailableSemaphores.clear();
    m_renderFinishedSemaphores.clear();
    m_inFlightFences.clear();
}

void VulkanRenderer::recreateSwapchain() {
    // Skip while minimised — a zero-sized swapchain is invalid; we'll rebuild once the
    // window has real extent again.
    if (m_windowExtent.width == 0 || m_windowExtent.height == 0) {
        return;
    }
    vkDeviceWaitIdle(m_context.device());

    const uint32_t oldImageCount = m_swapchain->imageCount();
    m_swapchain->recreate(m_windowExtent);

    // The per-image renderFinished semaphores must match the (possibly new) image count.
    if (m_swapchain->imageCount() != oldImageCount) {
        for (VkSemaphore sem : m_renderFinishedSemaphores) {
            vkDestroySemaphore(m_context.device(), sem, nullptr);
        }
        m_renderFinishedSemaphores.resize(m_swapchain->imageCount());
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (auto& sem : m_renderFinishedSemaphores) {
            VK_CHECK(vkCreateSemaphore(m_context.device(), &semInfo, nullptr, &sem));
        }
    }

    const VkExtent2D extent = m_swapchain->extent();
    // The offscreen HDR target + bloom targets track the swapchain size (the device is idle here).
    m_hdrTarget->resize(extent);
    m_postProcess->resize(extent, m_hdrTarget->resolveInfo(), m_hdrTarget->specResolveInfo());
    m_camera.setViewportSize(static_cast<float>(extent.width), static_cast<float>(extent.height));
    m_framebufferResized = false;
}

bool VulkanRenderer::drawFrame() {
    if (m_windowExtent.width == 0 || m_windowExtent.height == 0) {
        return false; // minimised: nothing to draw (the next expose/resize re-arms drawing)
    }

    VkDevice device = m_context.device();
    VK_CHECK(vkWaitForFences(device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX));

    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(device, m_swapchain->handle(), UINT64_MAX,
                                             m_imageAvailableSemaphores[m_currentFrame],
                                             VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return true; // this frame was skipped; the caller must schedule one at the new size
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        VK_CHECK(acquire);
    }

    // Only reset the fence once we're committing to submit work that will signal it,
    // otherwise an early-out above would leave it unsignalled and deadlock next frame.
    VK_CHECK(vkResetFences(device, 1, &m_inFlightFences[m_currentFrame]));

    VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    recordCommandBuffer(cmd, imageIndex);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &m_imageAvailableSemaphores[m_currentFrame];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m_renderFinishedSemaphores[imageIndex];
    VK_CHECK(vkQueueSubmit(m_context.graphicsQueue(), 1, &submit, m_inFlightFences[m_currentFrame]));

    VkSwapchainKHR swapchain = m_swapchain->handle();
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &m_renderFinishedSemaphores[imageIndex];
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

void VulkanRenderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    // Key-light shadow pass first (its own depth-only render pass on the shadow map), so the
    // scene pass below can sample the fresh map. Also fits this frame's light matrix, which
    // Scene::record() writes into the camera UBO.
    m_scene->recordShadowPass(cmd);

    const VkExtent2D extent = m_swapchain->extent();

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = extent;

    // --- Offscreen HDR scene pass: backdrop + meshes + grid, in LINEAR HDR. --------------------
    // Five clear slots cover every attachment layout the HDR target uses (MSAA:
    // colourMS/depth/resolve/specMS/specResolve; single-sample: colour/depth/spec) — the
    // SPECULAR attachments clear to zero (no specular where nothing draws).
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
    scenePass.renderPass = m_hdrTarget->renderPass();
    scenePass.framebuffer = m_hdrTarget->framebuffer();
    scenePass.renderArea.extent = extent;
    scenePass.clearValueCount = m_hdrTarget->attachmentCount();
    scenePass.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &scenePass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Imported meshes first (opaque, depth test+write), then the grid — a transparent overlay
    // that depth-tests against scene geometry and blends; the scene's set 3 + light matrix +
    // shadow dials feed its PCSS ground shadow.
    m_scene->record(cmd, m_camera, m_currentFrame);
    const LightingSettings& lighting = m_scene->lightingSettings();
    m_grid->record(cmd, m_camera, m_scene->iblSet(), m_scene->lightViewProj(),
                   glm::vec4(lighting.shadowIntensity, lighting.shadowSoftness,
                             lighting.shadowReach, 0.0f));
    vkCmdEndRenderPass(cmd);

    // --- SSSSS then bloom (PBR mode only — the stylized modes author display-ready values). ----
    const bool pbr = m_scene->shadeMode() == 1;
    if (pbr) {
        m_postProcess->recordSss(cmd);   // diffuses skin in place (HDR alpha = the SSS mask)
        m_postProcess->recordBloom(cmd); // then blooms the diffused image
    }

    // --- Swapchain pass: the fullscreen composite (tonemap + bloom add). -----------------------
    VkRenderPassBeginInfo presentPass{};
    presentPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    presentPass.renderPass = m_swapchain->renderPass();
    presentPass.framebuffer = m_swapchain->framebuffer(imageIndex);
    presentPass.renderArea.extent = extent;
    presentPass.clearValueCount = static_cast<uint32_t>(clears.size());
    presentPass.pClearValues = clears.data(); // the composite overwrites every pixel anyway
    vkCmdBeginRenderPass(cmd, &presentPass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    m_postProcess->recordComposite(cmd, pbr && m_scene->lightingSettings().tonemap, pbr);
    vkCmdEndRenderPass(cmd);

    VK_CHECK(vkEndCommandBuffer(cmd));
}

std::vector<char> VulkanRenderer::loadSpirv(const std::string& fileName) const {
    const std::string path = m_shaderDir + "/" + fileName;
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw VulkanError("Could not open SPIR-V file: " + path +
                          " (was the shader-compile build step run?)");
    }
    const std::streamsize size = file.tellg();
    std::vector<char> buffer(static_cast<size_t>(size));
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
}

} // namespace pose

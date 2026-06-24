/**
 * @file vulkanrenderer.cpp
 * @brief Implementation of the per-frame rendering loop. See vulkanrenderer.h.
 */

#include "vulkanrenderer.h"

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

    m_scene = std::make_unique<Scene>(m_context, m_swapchain->renderPass(),
                                      loadSpirv("mesh.vert.spv"), loadSpirv("mesh.frag.spv"));

    m_grid = std::make_unique<Grid>(m_context, m_swapchain->renderPass(),
                                    loadSpirv("grid.vert.spv"), loadSpirv("grid.frag.spv"));

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
    m_grid.reset();
    m_scene.reset();
    m_swapchain.reset();
}

void VulkanRenderer::notifyResize(VkExtent2D newExtent) {
    m_windowExtent = newExtent;
    m_framebufferResized = true;
}

void VulkanRenderer::addModel(const ModelData& data) {
    m_scene->addModel(data);
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
    m_camera.setViewportSize(static_cast<float>(extent.width), static_cast<float>(extent.height));
    m_framebufferResized = false;
}

void VulkanRenderer::drawFrame() {
    if (m_windowExtent.width == 0 || m_windowExtent.height == 0) {
        return; // minimised: nothing to draw
    }

    VkDevice device = m_context.device();
    VK_CHECK(vkWaitForFences(device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX));

    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(device, m_swapchain->handle(), UINT64_MAX,
                                             m_imageAvailableSemaphores[m_currentFrame],
                                             VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return; // skip this frame; the next one renders at the new size
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

    VkResult presentResult = vkQueuePresentKHR(m_context.presentQueue(), &present);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR ||
        m_framebufferResized) {
        recreateSwapchain();
    } else if (presentResult != VK_SUCCESS) {
        VK_CHECK(presentResult);
    }

    m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;
}

void VulkanRenderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    const VkExtent2D extent = m_swapchain->extent();

    std::array<VkClearValue, 2> clears{};
    // Viewport background #3E4042 = (62, 64, 66). The swapchain is an sRGB format, so the
    // clear value is interpreted as LINEAR and hardware-encoded to sRGB on store — these
    // are the linear equivalents (sRGB->linear of each channel), not the raw 8-bit/255
    // fractions, so the displayed pixel comes back out as exactly #3E4042.
    clears[0].color = {{0.04816f, 0.05125f, 0.05447f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_swapchain->renderPass();
    rp.framebuffer = m_swapchain->framebuffer(imageIndex);
    rp.renderArea.extent = extent;
    rp.clearValueCount = static_cast<uint32_t>(clears.size());
    rp.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Imported meshes first (opaque, depth test+write), within the viewport/scissor set above.
    m_scene->record(cmd, m_camera, m_currentFrame);

    // Floor grid last: a transparent overlay that depth-tests against scene geometry and blends.
    // It binds its own pipeline and reuses the viewport/scissor above.
    m_grid->record(cmd, m_camera);

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

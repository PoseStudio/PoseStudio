/**
 * @file vulkanrenderer.h
 * @brief The per-frame engine: command buffers, frame synchronisation, and the
 *        acquire -> record -> submit -> present loop.
 *
 * Owns the swapchain and the things that depend on the surface, drives one frame per
 * drawFrame() call, and transparently rebuilds the swapchain when the window resizes or
 * the surface goes out of date.
 *
 * Like the rest of rendering/, this is intentionally Qt-free — it takes a plain
 * std::string shader directory — so the only Qt coupling in the whole viewport lives in
 * VulkanWindow / ViewportWidget.
 */

#ifndef VULKANRENDERER_H
#define VULKANRENDERER_H

#include "vulkancommon.h"

#include "../scene/camera.h"

#include <memory>
#include <string>
#include <vector>

namespace pose {

class VulkanContext;
class VulkanSwapchain;
class Grid;
class Scene;
struct ModelData;

/**
 * @class VulkanRenderer
 * @brief Renders frames for one window surface and manages its frame lifecycle.
 */
class VulkanRenderer {
public:
    /**
     * @param context      The device layer (must outlive the renderer).
     * @param initialExtent Window size in physical pixels at construction.
     * @param shaderDir    Directory holding the compiled *.spv files (see CMake).
     */
    VulkanRenderer(VulkanContext& context, VkExtent2D initialExtent, std::string shaderDir);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    /// Records and presents one frame. Cheaply no-ops while the window is zero-sized
    /// (e.g. minimised). Recreates the swapchain on its own when needed.
    void drawFrame();

    /// Records the new physical-pixel size; the actual swapchain rebuild is deferred to
    /// the next drawFrame() so a burst of resize events coalesces into one rebuild.
    void notifyResize(VkExtent2D newExtent);

    /// Uploads an already-parsed model (geometry + decoded textures) to the scene. Call from the
    /// GUI thread (the upload blocks briefly). Throws on Vulkan failure. Parsing + image decoding
    /// happen in the Qt layer (VulkanWindow), keeping this core free of file/codec concerns.
    void addModel(const ModelData& data);

    /// Exposed so the window's input handlers can drive the view (orbit/pan/dolly).
    Camera& camera() { return m_camera; }

private:
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void destroySyncObjects();
    void recreateSwapchain();
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);

    /// Reads a compiled SPIR-V blob from m_shaderDir. Throws VulkanError if missing.
    std::vector<char> loadSpirv(const std::string& fileName) const;

    VulkanContext& m_context;
    std::string    m_shaderDir;

    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<Scene>           m_scene; // imported meshes (opaque)
    std::unique_ptr<Grid>            m_grid;  // floor grid overlay

    VkCommandPool                m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;            // one per frame-in-flight

    // imageAvailable + inFlight are per frame-in-flight; renderFinished is per swapchain
    // image (a semaphore signalled at submit must not be reused until that image's
    // present completes, which is tracked per image, not per frame).
    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence>     m_inFlightFences;

    uint32_t   m_currentFrame = 0;
    VkExtent2D m_windowExtent = {0, 0};
    bool       m_framebufferResized = false;

    Camera m_camera;
};

} // namespace pose

#endif // VULKANRENDERER_H

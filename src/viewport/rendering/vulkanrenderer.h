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
#include <utility>
#include <vector>

namespace pose {

class VulkanContext;
class VulkanSwapchain;
class Grid;
class Scene;
struct ModelData;
struct BakedEnvironment;
struct LightingSettings;

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
    /// (e.g. minimised). Recreates the swapchain on its own when needed. Returns true when the
    /// caller should schedule one more frame (the swapchain was just rebuilt, or this frame was
    /// skipped mid-rebuild, so what's on screen doesn't yet reflect the current state) — the
    /// window renders on demand, not continuously, so this is the only self-rearm signal.
    bool drawFrame();

    /// Records the new physical-pixel size; the actual swapchain rebuild is deferred to
    /// the next drawFrame() so a burst of resize events coalesces into one rebuild.
    void notifyResize(VkExtent2D newExtent);

    /// Uploads an already-parsed model (geometry + decoded textures) to the scene. Call from the
    /// GUI thread (the upload blocks briefly). Throws on Vulkan failure. Parsing + image decoding
    /// happen in the Qt layer (VulkanWindow), keeping this core free of file/codec concerns.
    void addModel(const ModelData& data);

    /// Returns the index of the nearest model the @p ray hits (for picking), or -1 if none.
    int pickModel(const Ray& ray) const;

    /// Removes the model at @p index from the scene, waiting for the GPU to go idle first so its
    /// buffers/descriptors aren't freed while an in-flight frame still references them.
    void deleteModel(std::size_t index);

    /// Exposed so the window's input handlers can drive the view (orbit/pan/dolly).
    Camera& camera() { return m_camera; }

    /// Uploads a CPU-baked lighting environment (SH + prefiltered specular; see bakeEnvironment) and
    /// swaps it in. The bake is done off the render thread by the Qt layer — keeping both the CPU work
    /// and the image codec out of this core. No-op if the scene doesn't exist yet.
    void applyBakedEnvironment(const BakedEnvironment& baked);

    /// Applies the live lighting/exposure dials (Environment panel). No-op if the scene doesn't exist.
    void setLightingSettings(const LightingSettings& settings);

    // --- Posing UI (forwarded to the Scene) ---
    bool hasPosableFigure() const;
    void setShowSkeleton(bool on);
    bool showSkeleton() const;

    // --- Shading (forwarded to the Scene) ---
    /// Sets the viewport shade mode (see mesh.frag's mode table). Takes effect on the next frame.
    void setShadeMode(int mode);
    int  shadeMode() const;
    /// Selects the figure joint nearest the pixel (@p px,@p py) in a @p vpW × @p vpH viewport.
    /// Returns the selected bone index, or -1 if none.
    int  selectBoneAt(float px, float py, float vpW, float vpH);
    bool hasSelectedBone() const;
    void nudgeSelectedBone(const glm::vec3& deltaEulerDegrees);
    /// Settles the figure after an interactive pose edit (applies pose correctives). Call on drag end.
    void finalizePose();

    // --- Rotate gizmo (forwarded to the Scene; use the renderer's own camera) ---
    int  gizmoAxisAt(float px, float py, float vpW, float vpH) const;
    void rotateGizmo(int axis, float prevX, float prevY, float curX, float curY, float vpW, float vpH);

    // --- Pose snapshot (for undo/redo) ---
    std::vector<std::pair<std::string, glm::vec3>> capturePose() const;
    void applyPose(const std::vector<std::pair<std::string, glm::vec3>>& pose);
    /// Saves / loads the posed figure's joint rotations to/from @p path. Returns false if there's
    /// no figure or the file can't be opened.
    bool savePose(const std::string& path) const;
    bool loadPose(const std::string& path);

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

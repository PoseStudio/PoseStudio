/**
 * @file scene.h
 * @brief Owns the lit mesh pipeline, the per-frame camera/lighting UBO + descriptors, and the
 *        list of imported models. The mesh analogue of Grid.
 *
 * Kept self-contained so VulkanRenderer doesn't accumulate per-feature pipeline/descriptor code:
 * the renderer just constructs a Scene, forwards imports to addModel(), and calls record() inside
 * the render pass (before the transparent grid). Qt-free (Vulkan + std + GLM).
 */

#ifndef SCENE_H
#define SCENE_H

#include "vulkanbuffer.h"

#include <vulkan/vulkan.h>

#include <memory>
#include <vector>

namespace pose {

class VulkanContext;
class VulkanPipeline;
class VulkanTexture;
class Camera;
class Model;
struct ModelData;

/**
 * @class Scene
 * @brief The collection of imported meshes plus the machinery to draw them lit.
 */
class Scene {
public:
    Scene(VulkanContext& context, VkRenderPass renderPass,
          const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv);
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    /// Uploads CPU geometry to the GPU and adds it to the scene. Call from the GUI thread between
    /// frames (the upload blocks briefly on its own one-time submit; it touches no in-flight state).
    void addModel(const ModelData& data);

    /// Number of models currently in the scene.
    std::size_t modelCount() const { return m_models.size(); }

    /// Updates this frame's camera UBO, binds the pipeline + camera set, and records every model.
    /// The caller has begun the render pass and set the dynamic viewport/scissor.
    void record(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex);

private:
    void createDescriptorResources();

    VulkanContext&                  m_context;
    std::unique_ptr<VulkanPipeline> m_pipeline;

    // Per-frame camera/lighting UBO (set 0, binding 0): one buffer + one set per frame-in-flight.
    VkDescriptorSetLayout        m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool             m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VulkanBuffer>    m_cameraBuffers;
    std::vector<VkDescriptorSet> m_cameraSets;

    // Per-material diffuse texture (set 1, binding 0). The layout is shared; each Model owns the
    // pool/sets for its meshes. Untextured meshes point at a shared 1x1 white fallback so the
    // shader always samples and one pipeline serves both.
    VkDescriptorSetLayout          m_materialSetLayout = VK_NULL_HANDLE;
    std::unique_ptr<VulkanTexture> m_fallbackTexture;

    std::vector<std::unique_ptr<Model>> m_models;
};

} // namespace pose

#endif // SCENE_H

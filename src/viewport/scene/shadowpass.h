/**
 * @file shadowpass.h
 * @brief The key light's depth-only shadow pass: the shadow map, its skinned depth pipeline, and
 *        the per-frame light frustum fit.
 *
 * Every frame, before the main scene pass, the key light's view of the scene is rendered into a
 * depth map (ShadowMap) through an orthographic frustum fitted around the casters AND the spots
 * where their shadows land on the floor — the ground shadow is only correct where the RECEIVER
 * is inside the frustum, so the floor patch must be covered too. The fitted light matrix is the
 * pass's other product: the Scene writes it into the camera UBO (mesh.frag's self-shadow lookup)
 * and hands it to the grid (the PCSS ground shadow). Owning the map, the pipeline, and the matrix
 * together keeps the Scene down to "fit, then record" and lets the shadow map be created before
 * the scene-wide descriptor set that samples it is written. Qt-free (Vulkan + std + GLM).
 */

#ifndef SHADOWPASS_H
#define SHADOWPASS_H

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace pose {

class Model;
class ShadowMap;
class VulkanContext;
class VulkanPipeline;

class ShadowPass {
public:
    /// Builds the shadow map and the depth-only pipeline (shadow.vert/frag: the mesh pass's
    /// skinned vertex layout rendered into the map's single-sample pass with a static depth bias
    /// against acne and no color attachment). Its only descriptor set is the per-model POSE
    /// layout (@p poseSetLayout, bound at index 0 — see Model::recordShadow).
    ShadowPass(VulkanContext& context, VkDescriptorSetLayout poseSetLayout,
               const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv);
    ~ShadowPass();

    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;

    /// The map the pass renders — bound scene-wide (set 3 bindings 2 + 3) by the Scene.
    const ShadowMap& shadowMap() const { return *m_shadowMap; }

    /// The light's ortho view-projection from the last fit(): the shadow-map space the shaders
    /// look up. Parked on a degenerate projection (every receiver maps to depth 0, so the
    /// LESS_OR_EQUAL compare always passes = fully lit) while shadows are off or nothing casts.
    const glm::mat4& lightViewProj() const { return m_lightViewProj; }

    /// Fits the frustum around @p models' world bounds and their floor projections along
    /// @p keyDir (the normalized direction TO the light). Returns true when the pass should be
    /// recorded; false — with the matrix parked as above — when @p enabled is false or there
    /// are no casters. In both false cases the map keeps its previous contents: harmless,
    /// since the degenerate matrix makes every lookup pass without reading it.
    bool fit(const std::vector<std::unique_ptr<Model>>& models, const glm::vec3& keyDir,
             bool enabled);

    /// Records the pass: begins the map's render pass, draws every model's opaque meshes
    /// through the fitted matrix, ends it. Call only when fit() returned true.
    void record(VkCommandBuffer cmd, const std::vector<std::unique_ptr<Model>>& models,
                uint32_t frameIndex);

private:
    std::unique_ptr<ShadowMap>      m_shadowMap;
    std::unique_ptr<VulkanPipeline> m_pipeline;
    glm::mat4                       m_lightViewProj{1.0f};
};

} // namespace pose

#endif // SHADOWPASS_H

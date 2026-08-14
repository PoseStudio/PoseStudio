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

#include "environment.h"
#include "lightingsettings.h"
#include "vulkanbuffer.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <glm/mat3x3.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pose {

class VulkanContext;
class VulkanPipeline;
class VulkanTexture;
class Camera;
class Model;
class IblMaps;
struct ModelData;
struct Ray;

/**
 * @class Scene
 * @brief The collection of imported meshes plus the machinery to draw them lit.
 */
class Scene {
public:
    Scene(VulkanContext& context, VkRenderPass renderPass, const std::vector<char>& vertSpirv,
          const std::vector<char>& fragSpirv, const std::vector<char>& skeletonVertSpirv,
          const std::vector<char>& skeletonFragSpirv);
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    /// Uploads CPU geometry to the GPU and adds it to the scene. Call from the GUI thread between
    /// frames (the upload blocks briefly on its own one-time submit; it touches no in-flight state).
    void addModel(const ModelData& data);

    /// Number of models currently in the scene.
    std::size_t modelCount() const { return m_models.size(); }

    /// Uploads a CPU-baked environment (SH irradiance + prefiltered specular; see bakeEnvironment) and
    /// points the IBL descriptor set at it. Waits for the GPU to idle first, since it swaps the maps the
    /// mesh pipeline samples. The heavy CPU bake is done by the caller (off the render thread for
    /// interactive HDRI switches); this does only the fast GPU work.
    void applyBakedEnvironment(const BakedEnvironment& baked);

    /// Live lighting/exposure controls (the Environment panel drives these). All shader-side, so this
    /// is just a cached value read into the next frame's UBO — no re-bake.
    void                   setLightingSettings(const LightingSettings& settings) { m_lighting = settings; }
    const LightingSettings& lightingSettings() const { return m_lighting; }

    /// Returns the index of the nearest model whose bounding box @p ray hits, or -1 if none.
    int pickModel(const Ray& ray) const;

    /// Removes the model at @p index (no-op if out of range). The caller MUST have ensured the GPU
    /// is idle first (the model's buffers/descriptors may be referenced by in-flight frames) —
    /// VulkanRenderer::deleteModel() does this.
    void removeModel(std::size_t index);

    /// Updates this frame's camera UBO, binds the pipeline + camera set, and records every model.
    /// The caller has begun the render pass and set the dynamic viewport/scissor.
    void record(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex);

    // --- Posing UI ---
    /// Whether the scene holds a posable figure (a model with a skeleton).
    bool hasPosableFigure() const;
    /// Show/hide the skeleton overlay (drawn over the figure so joints are visible + clickable).
    void setShowSkeleton(bool on) { m_showSkeleton = on; }
    bool showSkeleton() const { return m_showSkeleton; }

    // --- Shading ---
    /// Selects the viewport shade mode (index into the shader's mode table; see mesh.frag). Written
    /// into the per-frame camera UBO, so the whole scene re-shades on the next frame — no pipeline swap.
    void setShadeMode(int mode) { m_shadeMode = mode; }
    int  shadeMode() const { return m_shadeMode; }
    /// Selects the figure joint nearest the pixel (@p px, @p py) in a @p vpW × @p vpH viewport (only
    /// if within a small radius). Returns the selected bone index, or -1 if none is selected.
    int selectBoneAt(float px, float py, float vpW, float vpH, const Camera& camera);
    /// True if a joint is currently selected on the figure.
    bool hasSelectedBone() const;
    /// Rotates the selected joint by @p deltaEulerDegrees (accumulated), re-posing the figure.
    void nudgeSelectedBone(const glm::vec3& deltaEulerDegrees);
    /// Settles the figure after an interactive pose edit: applies pose correctives, which are
    /// deferred during a drag (they re-upload geometry) and applied here on release.
    void finalizePose();

    // --- Rotate gizmo (three axis rings on the selected joint) ---
    /// Returns which gizmo ring (0=X,1=Y,2=Z) the pixel (@p px,@p py) is over, or -1. Only valid when
    /// a joint is selected (the rings are drawn around it).
    int gizmoAxisAt(float px, float py, float vpW, float vpH, const Camera& camera) const;
    /// Rotates the selected joint about gizmo axis @p axis by the screen-angle the cursor swept around
    /// the joint from (@p prevX,@p prevY) to (@p curX,@p curY). Accumulates (call per mouse-move).
    void rotateGizmo(int axis, float prevX, float prevY, float curX, float curY, float vpW, float vpH,
                     const Camera& camera);

    // --- Pose snapshot (for undo/redo) ---
    std::vector<std::pair<std::string, glm::vec3>> capturePose() const;
    void applyPose(const std::vector<std::pair<std::string, glm::vec3>>& pose);

    /// Saves / loads the figure's pose (per-joint rotations) to/from a plain-text file. Returns false
    /// if there's no figure or the file can't be opened.
    bool savePose(const std::string& path) const;
    bool loadPose(const std::string& path);

private:
    void createDescriptorResources();
    Model* figureModel() const; // first model with a skeleton, or nullptr

    VulkanContext&                  m_context;
    std::unique_ptr<VulkanPipeline> m_pipeline;            // opaque pass (depth write on)
    std::unique_ptr<VulkanPipeline> m_transparentPipeline; // alpha-blended pass (depth write off)
    std::unique_ptr<VulkanPipeline> m_skeletonPipeline;    // line overlay for the posing skeleton
    // Host-mapped line vertices (pos+color), one buffer per frame-in-flight: record() rewrites the
    // overlay every frame, so a single shared buffer would be CPU-written while the previous
    // frame's GPU read of it is still in flight.
    std::vector<VulkanBuffer>       m_skeletonVertexBuffers;
    bool                            m_showSkeleton = false;
    int                             m_shadeMode = 1;        // viewport shade mode (see mesh.frag); 1 = PBR/IBL

    // Lighting environment: the baked diffuse-irradiance SH (feeds the per-frame camera UBO so the PBR
    // mode is image-based-lit) and the environment-independent split-sum BRDF LUT (integrated once,
    // reused on every re-bake). The prefiltered specular cubemap lives in m_iblMaps. A CPU-baked
    // BakedEnvironment (SH + specular) is applied via applyBakedEnvironment(); no source image is kept.
    EnvironmentSH    m_environmentSH;
    BrdfLut          m_brdfLut;
    LightingSettings m_lighting; // live exposure/diffuse/specular/fill/key/rotation/tonemap dials (Environment panel)

    // Per-frame camera/lighting UBO (set 0, binding 0): one buffer + one set per frame-in-flight.
    VkDescriptorSetLayout        m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool             m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VulkanBuffer>    m_cameraBuffers;
    std::vector<VkDescriptorSet> m_cameraSets;

    // Per-material textures (set 1): binding 0 = diffuse (sRGB), binding 1 = detail normal/bump map
    // (linear). The layout is shared; each Model owns the pool/sets for its meshes. Meshes without a
    // given map point at a shared fallback (1x1 white diffuse / flat normal) so the shader always
    // samples and one pipeline serves textured and untextured meshes alike.
    VkDescriptorSetLayout          m_materialSetLayout = VK_NULL_HANDLE;
    std::unique_ptr<VulkanTexture> m_fallbackTexture; // 1x1 opaque white
    std::unique_ptr<VulkanTexture> m_fallbackNormal;  // 1x1 flat normal (128,128,255), linear

    // Per-model skinning joint matrices (set 2, binding 0): a storage buffer of skin matrices read
    // in the vertex stage. The layout is shared; each Model owns its own buffer + set (see mesh.h).
    VkDescriptorSetLayout m_jointSetLayout = VK_NULL_HANDLE;

    // Image-based lighting (set 3): the prefiltered specular cubemap + BRDF LUT (owned by m_iblMaps),
    // bound scene-wide. The diffuse half is the SH in m_environmentSH (fed via the camera UBO). One
    // static set, its images (re)filled by applyBakedEnvironment() when the environment changes.
    VkDescriptorSetLayout    m_iblSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool         m_iblPool = VK_NULL_HANDLE;
    VkDescriptorSet          m_iblSet = VK_NULL_HANDLE;
    std::unique_ptr<IblMaps> m_iblMaps;

    std::vector<std::unique_ptr<Model>> m_models;
};

} // namespace pose

#endif // SCENE_H

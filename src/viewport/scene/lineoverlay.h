/**
 * @file lineoverlay.h
 * @brief The viewport's line overlays: the posing skeleton, the joint-pin markers, and the
 *        orthographic side views' floor line — one host-mapped line buffer, one draw.
 *
 * Everything drawn as lines over the scene goes through here, at the end of the main pass, so
 * the Scene records exactly one overlay draw per frame whatever is showing. The skeleton is
 * hidden by default — joints are grabbed directly on the figure, so the character isn't
 * cluttered with a bone cage — and picking never depends on it (bonepicker.h hit-tests the joints
 * regardless). The pin markers, by contrast, always show: a user pin persists across drags and
 * the user must see where it is. The floor line exists because an orthographic side view sees
 * the floor plane exactly edge-on, where the grid shader's ray/plane intersection has nothing to
 * hit. The overlay runs even with no models in the scene — that is what keeps the floor line on
 * screen in an empty viewport. Qt-free (Vulkan + std + GLM).
 */

#ifndef LINEOVERLAY_H
#define LINEOVERLAY_H

#include "vulkanbuffer.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace pose {

class Camera;
class Model;
class VulkanContext;
class VulkanPipeline;

/// A vertex of the line overlay: a world-space point + its colour. Must match skeleton.vert.
struct LineVertex {
    glm::vec3 pos;
    glm::vec3 color;
};

/// A pointer drawn by the overlay — a VR controller's ray (VR Mode, vulkanwindow_vr.cpp): the
/// segment @p from -> @p to in @p color, with a small marker at @p to when @p marker (the ray's
/// end on a joint, a model or the floor). Set per frame; empty outside VR Mode.
struct PointerLine {
    glm::vec3 from{0.0f};
    glm::vec3 to{0.0f};
    glm::vec3 color{1.0f};
    bool      marker = false;
};

class LineOverlay {
public:
    /// Builds the line pipeline (skeleton.vert/frag, LINE_LIST, depth test OFF so the lines are
    /// visible through the body, alpha and the specular attachment masked — overlay lines must
    /// not touch the SSS mask) against the main pass, with @p cameraSetLayout as its only set,
    /// plus one host-mapped vertex buffer per frame in flight.
    LineOverlay(VulkanContext& context, VkRenderPass renderPass,
                VkDescriptorSetLayout cameraSetLayout, const std::vector<char>& vertSpirv,
                const std::vector<char>& fragSpirv);
    ~LineOverlay();

    LineOverlay(const LineOverlay&) = delete;
    LineOverlay& operator=(const LineOverlay&) = delete;

    /// Show/hide the skeleton line list (View -> Show Skeleton / the strip's Skeleton button).
    void setShowSkeleton(bool on) { m_showSkeleton = on; }
    bool showSkeleton() const { return m_showSkeleton; }
    /// The pointer lines to draw from now on (VR Mode's controller rays; empty = none).
    void setPointerLines(std::vector<PointerLine> lines) { m_pointerLines = std::move(lines); }

    /// Emits this frame's lines into frame slot @p frameIndex's buffer and draws them (nothing
    /// is recorded when there are none). @p cameraSet is this frame's set-0 descriptor;
    /// @p activeFigure the posing target whose skeleton and contact pins are drawn (null =
    /// none); user pins are drawn on EVERY figure in @p models. The caller has begun the main
    /// render pass.
    void record(VkCommandBuffer cmd, const Camera& camera, VkDescriptorSet cameraSet,
                const std::vector<std::unique_ptr<Model>>& models, const Model* activeFigure,
                uint32_t frameIndex);

private:
    std::unique_ptr<VulkanPipeline> m_pipeline;
    // Host-mapped line vertices (pos+color), one buffer per frame-in-flight: record() rewrites the
    // overlay every frame, so a single shared buffer would be CPU-written while the previous
    // frame's GPU read of it is still in flight.
    std::vector<VulkanBuffer>       m_vertexBuffers;
    bool                            m_showSkeleton = false;
    std::vector<PointerLine>        m_pointerLines; // see setPointerLines
};

} // namespace pose

#endif // LINEOVERLAY_H

/**
 * @file lineoverlay.cpp
 * @brief The line overlay's pipeline, buffers, and per-frame emission. See lineoverlay.h.
 */

#include "lineoverlay.h"

#include "camera.h"
#include "model.h"
#include "vulkancommon.h" // kMaxFramesInFlight
#include "vulkanpipeline.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace pose {

namespace {

constexpr uint32_t kMaxSkeletonVerts = 8192; // 2 per bone segment + pin markers; far above any figure's

// Screen-roughly-constant overlay marker radius: a small fraction of the distance to the camera,
// so a joint marker keeps a consistent on-screen size as you dolly in/out.
float markerRadius(const glm::vec3& center, const glm::vec3& cameraPos) {
    return 0.048f * glm::length(center - cameraPos);
}

} // namespace

LineOverlay::LineOverlay(VulkanContext& context, VkRenderPass renderPass,
                         VkDescriptorSetLayout cameraSetLayout, const std::vector<char>& vertSpirv,
                         const std::vector<char>& fragSpirv) {
    // Skeleton overlay: a colored line list drawn over the figure (depth test OFF, so every joint
    // is visible through the body). Uses set 0 (camera) only; per-vertex color.
    PipelineConfig lineConfig;
    lineConfig.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    lineConfig.depthTestEnable = false;
    lineConfig.depthWriteEnable = false;
    lineConfig.blendEnable = false;
    lineConfig.colorWriteAlpha = false; // overlay lines must not touch the SSS mask
    lineConfig.colorAttachmentCount = 2; // the spec target is present but masked off
    lineConfig.pushConstantSize = 0;
    VkVertexInputBindingDescription lineBinding{};
    lineBinding.binding = 0;
    lineBinding.stride = sizeof(LineVertex);
    lineBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 2> lineAttrs{};
    lineAttrs[0].location = 0;
    lineAttrs[0].binding = 0;
    lineAttrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    lineAttrs[0].offset = offsetof(LineVertex, pos);
    lineAttrs[1].location = 1;
    lineAttrs[1].binding = 0;
    lineAttrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    lineAttrs[1].offset = offsetof(LineVertex, color);
    lineConfig.vertexBindings.assign(1, lineBinding);
    lineConfig.vertexAttributes.assign(lineAttrs.begin(), lineAttrs.end());
    lineConfig.descriptorSetLayouts = {cameraSetLayout}; // set 0 = camera UBO
    m_pipeline = std::make_unique<VulkanPipeline>(context, renderPass, vertSpirv, fragSpirv,
                                                  lineConfig);

    // One overlay vertex buffer per frame-in-flight (see the member comment in the header).
    m_vertexBuffers.reserve(kMaxFramesInFlight);
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        m_vertexBuffers.emplace_back(context, kMaxSkeletonVerts * sizeof(LineVertex),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                         VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }
}

LineOverlay::~LineOverlay() = default;

void LineOverlay::record(VkCommandBuffer cmd, const Camera& camera, VkDescriptorSet cameraSet,
                         const std::vector<std::unique_ptr<Model>>& models,
                         const Model* activeFigure, uint32_t frameIndex) {
    VulkanBuffer& lineBuffer = m_vertexBuffers[frameIndex];
    auto* verts = static_cast<LineVertex*>(lineBuffer.mappedData());
    uint32_t count = 0;
    const glm::vec3 cameraPos = camera.position();

    // Orthographic side views (the Front/Back/Left/Right hotkeys) see the floor plane exactly
    // edge-on, where the grid shader's ray/plane intersection has nothing to hit — so draw the
    // floor as the one line it is from there: the y = 0 trace of the view plane through the
    // target, along the camera's right vector, in the grid's own line grey. Pure elevation
    // drawings get their ground line, as in any DCC's fixed side camera.
    if (camera.orthographic() && std::abs(camera.pitch()) < 1e-3f && count + 2 <= kMaxSkeletonVerts) {
        const glm::mat4 view = camera.view();
        const glm::vec3 right(view[0][0], view[1][0], view[2][0]); // world-space screen right
        const glm::vec3 base(camera.target().x, 0.0f, camera.target().z);
        constexpr float kFloorLineHalfLength = 200.0f;
        const glm::vec3 floorLineGrey(0.30f); // the grid's major-line brightness (grid.frag)
        verts[count++] = {base - right * kFloorLineHalfLength, floorLineGrey};
        verts[count++] = {base + right * kFloorLineHalfLength, floorLineGrey};
    }

    // Pin markers: a small wireframe octahedron. Screen-constant sizing (markerRadius). 12 edges
    // = 24 line vertices each.
    const auto emitPinMarker = [&](const glm::vec3& c, float r, const glm::vec3& col) {
        if (count + 24 > kMaxSkeletonVerts) {
            return;
        }
        const glm::vec3 ax[3] = {glm::vec3(r, 0.0f, 0.0f), glm::vec3(0.0f, r, 0.0f),
                                 glm::vec3(0.0f, 0.0f, r)};
        for (int i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3;
            for (int si = -1; si <= 1; si += 2) {
                const glm::vec3 a = c + ax[i] * static_cast<float>(si);
                for (int sj = -1; sj <= 1; sj += 2) {
                    verts[count++] = {a, col};
                    verts[count++] = {c + ax[j] * static_cast<float>(sj), col};
                }
            }
        }
    };
    const glm::vec3 userPinColor(1.0f, 0.55f, 0.12f);
    const glm::vec3 contactPinColor(0.25f, 0.9f, 0.95f);

    // Posing overlay: (only when enabled) the skeleton line list, plus the pin markers. The
    // skeleton is hidden by default — joints are grabbed directly on the figure — but they stay
    // pickable regardless, because picking (Scene::selectBoneAt) is independent of what's drawn
    // here.
    if (const Model* fig = activeFigure; fig && fig->boneCount() > 0) {
        const int selected = fig->highlightBone();

        // Skeleton as line segments (joint -> parent); the selected joint's segments are highlighted.
        if (m_showSkeleton) {
            const glm::vec3 boneColor(0.25f, 0.85f, 1.0f); // cyan
            const glm::vec3 selColor(1.0f, 0.8f, 0.1f);    // yellow highlight
            for (std::size_t i = 0; i < fig->boneCount() && count + 2 <= kMaxSkeletonVerts; ++i) {
                const int parent = fig->boneParent(i);
                if (parent < 0) {
                    continue;
                }
                const bool hot = (static_cast<int>(i) == selected || parent == selected);
                const glm::vec3 c = hot ? selColor : boneColor;
                verts[count++] = {fig->boneWorldPosition(static_cast<std::size_t>(parent)), c};
                verts[count++] = {fig->boneWorldPosition(i), c};
            }
        }

        // A smaller cyan marker on each ground-contact pin while an IK drag is live, so it's
        // visible which feet the solve is holding planted.
        for (const int node : fig->activeContactPins()) {
            if (node >= 0 && static_cast<std::size_t>(node) < fig->boneCount()) {
                const glm::vec3 c = fig->boneWorldPosition(static_cast<std::size_t>(node));
                emitPinMarker(c, 0.3f * markerRadius(c, cameraPos), contactPinColor);
            }
        }
    }

    // An orange marker on every USER-pinned joint of every figure, always shown — the pin
    // persists across drags.
    for (const std::unique_ptr<Model>& anyFig : models) {
        if (!anyFig->hasSkeleton()) {
            continue;
        }
        for (std::size_t i = 0; i < anyFig->boneCount(); ++i) {
            if (anyFig->isBonePinned(i)) {
                const glm::vec3 c = anyFig->boneWorldPosition(i);
                emitPinMarker(c, 0.45f * markerRadius(c, cameraPos), userPinColor);
            }
        }
    }

    // The VR controllers' pointer rays (setPointerLines): a segment each, and a marker where the
    // ray ends on something (a joint, a model, the floor).
    for (const PointerLine& pointer : m_pointerLines) {
        if (count + 2 > kMaxSkeletonVerts) {
            break;
        }
        verts[count++] = {pointer.from, pointer.color};
        verts[count++] = {pointer.to, pointer.color};
        if (pointer.marker) {
            emitPinMarker(pointer.to, 0.25f * markerRadius(pointer.to, cameraPos), pointer.color);
        }
    }

    if (count > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->layout(), 0, 1,
                                &cameraSet, 0, nullptr);
        const VkBuffer vb = lineBuffer.handle();
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdDraw(cmd, count, 1, 0, 0);
    }
}

} // namespace pose

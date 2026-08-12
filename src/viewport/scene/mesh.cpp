/**
 * @file mesh.cpp
 * @brief Implementation of Mesh and Model. See mesh.h.
 */

#include "mesh.h"

#include "camera.h" // Ray
#include "modeldata.h"
#include "vertex.h"
#include "vulkancommands.h"
#include "vulkancommon.h"
#include "vulkancontext.h"

#include <glm/gtc/matrix_transform.hpp>

#include <string>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pose {

namespace {

// Builds a rotation matrix from Euler angles (degrees) composed in @p order (e.g. "YZX"), matching
// the figure format's per-joint rotation_order. Each listed axis rotation is applied in turn.
glm::mat4 eulerMatrix(const glm::vec3& degrees, const std::string& order) {
    glm::mat4 m(1.0f);
    for (const char c : order) {
        float angle = 0.0f;
        glm::vec3 axis(0.0f);
        if (c == 'X' || c == 'x') {
            angle = degrees.x;
            axis = glm::vec3(1.0f, 0.0f, 0.0f);
        } else if (c == 'Y' || c == 'y') {
            angle = degrees.y;
            axis = glm::vec3(0.0f, 1.0f, 0.0f);
        } else if (c == 'Z' || c == 'z') {
            angle = degrees.z;
            axis = glm::vec3(0.0f, 0.0f, 1.0f);
        } else {
            continue;
        }
        m = m * glm::rotate(glm::mat4(1.0f), glm::radians(angle), axis);
    }
    return m;
}

// Evaluates a corrective driver spline at @p x: a Catmull-Rom Hermite through the (ascending-in-x)
// knots, clamped flat outside the knot range and — within each segment — clamped to that segment's
// endpoint values so the smoothing can never overshoot into a wrong-signed correction.
float evalSpline(const std::vector<CorrectiveKnot>& knots, float x) {
    if (knots.empty()) {
        return 0.0f;
    }
    if (knots.size() == 1 || x <= knots.front().x) {
        return knots.front().y;
    }
    if (x >= knots.back().x) {
        return knots.back().y;
    }
    std::size_t i = 0;
    while (i + 1 < knots.size() && x > knots[i + 1].x) {
        ++i;
    }
    const CorrectiveKnot& p1 = knots[i];
    const CorrectiveKnot& p2 = knots[i + 1];
    const float h = p2.x - p1.x;
    if (h <= 1e-6f) {
        return p1.y;
    }
    const float u = (x - p1.x) / h;
    const float y0 = (i > 0) ? knots[i - 1].y : p1.y;                    // one-sided at the ends
    const float y3 = (i + 2 < knots.size()) ? knots[i + 2].y : p2.y;
    const float m1 = 0.5f * (p2.y - y0);
    const float m2 = 0.5f * (y3 - p1.y);
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const float h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2;
    const float h11 = u3 - u2;
    const float y = h00 * p1.y + h10 * m1 + h01 * p2.y + h11 * m2;
    return std::clamp(y, std::min(p1.y, p2.y), std::max(p1.y, p2.y));
}

} // namespace

Mesh::Mesh(VulkanContext& context, const MeshData& data, VkDescriptorSetLayout materialSetLayout,
           VkDescriptorPool materialPool, const VulkanTexture& fallbackDiffuse,
           const VulkanTexture& fallbackNormal, ImmediateBatch& batch)
    : m_indexCount(static_cast<uint32_t>(data.indices.size())), m_baseColor(data.baseColor),
      m_roughness(data.roughness), m_opacity(data.opacity) {
    const VkDeviceSize vertexBytes = sizeof(Vertex) * data.vertices.size();
    const VkDeviceSize indexBytes = sizeof(uint32_t) * data.indices.size();
    // Record all uploads into the shared batch instead of submitting per buffer/texture.
    m_vertexBuffer = createDeviceLocalBuffer(context, data.vertices.data(), vertexBytes,
                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, batch);
    m_indexBuffer = createDeviceLocalBuffer(context, data.indices.data(), indexBytes,
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, batch);

    // Upload the diffuse map if the Qt layer decoded one for this mesh; else use the white fallback.
    if (data.diffuseWidth > 0 && data.diffuseHeight > 0 && !data.diffusePixels.empty()) {
        m_texture = std::make_unique<VulkanTexture>(context, data.diffusePixels.data(),
                                                    data.diffuseWidth, data.diffuseHeight, batch);
    }
    // Upload the detail (normal/bump) map if present — a LINEAR texture — and remember its mode. With
    // no map we bind the flat-normal fallback and force mode 0 (no perturbation).
    if (data.normalWidth > 0 && data.normalHeight > 0 && !data.normalPixels.empty()) {
        m_normalTexture = std::make_unique<VulkanTexture>(context, data.normalPixels.data(),
                                                          data.normalWidth, data.normalHeight, batch,
                                                          /*srgb=*/false);
        m_normalMode = data.normalMode;
    }
    const VulkanTexture& diffuse = m_texture ? *m_texture : fallbackDiffuse;
    const VulkanTexture& detail = m_normalTexture ? *m_normalTexture : fallbackNormal;

    // Allocate this mesh's set-1 descriptor from the owning Model's pool: binding 0 = diffuse,
    // binding 1 = detail map.
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = materialPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &materialSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(context.device(), &allocInfo, &m_materialSet));

    std::array<VkDescriptorImageInfo, 2> imageInfos{};
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[0].imageView = diffuse.imageView();
    imageInfos[0].sampler = diffuse.sampler();
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].imageView = detail.imageView();
    imageInfos[1].sampler = detail.sampler();

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_materialSet;
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(context.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
}

void Mesh::record(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& model) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &m_materialSet, 0,
                            nullptr); // set 1 = this mesh's diffuse texture

    MeshPushConstants push{};
    push.model = model;
    push.baseColor = glm::vec4(m_baseColor, m_opacity);
    push.material = glm::vec4(m_roughness, static_cast<float>(m_normalMode), 0.0f, 0.0f);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);

    const VkBuffer vertexBuffer = m_vertexBuffer.handle();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

void Mesh::reuploadVertices(VulkanContext& context, const std::vector<Vertex>& vertices,
                            ImmediateBatch& batch) {
    // Recreate the device-local vertex buffer with the re-morphed data (same count). The previous
    // buffer is freed on assignment; the caller has made the GPU idle so it isn't in use.
    const VkDeviceSize vertexBytes = sizeof(Vertex) * vertices.size();
    m_vertexBuffer = createDeviceLocalBuffer(context, vertices.data(), vertexBytes,
                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, batch);
}

Model::Model(VulkanContext& context, const ModelData& data, VkDescriptorSetLayout materialSetLayout,
             VkDescriptorSetLayout jointSetLayout, const VulkanTexture& fallbackDiffuse,
             const VulkanTexture& fallbackNormal)
    : m_context(context) {
    uint32_t meshCount = 0;
    for (const MeshData& meshData : data.meshes) {
        if (!meshData.indices.empty()) {
            ++meshCount;
        }
    }
    if (meshCount == 0) {
        return; // nothing to draw; no pool/sets needed
    }

    // Compute a local-space AABB over every (non-empty) mesh's vertices for mouse picking.
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (const MeshData& meshData : data.meshes) {
        if (meshData.indices.empty()) {
            continue;
        }
        for (const Vertex& vertex : meshData.vertices) {
            lo = glm::min(lo, vertex.pos);
            hi = glm::max(hi, vertex.pos);
        }
    }
    if (lo.x <= hi.x) { // at least one vertex seen
        m_boundsMin = lo;
        m_boundsMax = hi;
        m_hasBounds = true;
    }

    // Descriptors from this model's pool: one set-1 per mesh (two samplers each) + one set-2 joint
    // set for the whole model.
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = meshCount * 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = meshCount + 1;
    VK_CHECK(vkCreateDescriptorPool(m_context.device(), &poolInfo, nullptr, &m_materialPool));

    // One upload batch for the whole model: every mesh's vertex/index buffers and texture record
    // into it, and it's submitted exactly once below — collapsing what used to be ~3 blocking
    // submits per mesh (the dominant cost of importing a model with many material groups) into a
    // single GPU round-trip. Trade-off: all staging buffers are held until that submit completes,
    // so peak staging memory is the sum across meshes rather than one-at-a-time.
    const bool hasCorrectives = !data.correctives.empty();
    ImmediateBatch batch(m_context);
    m_meshes.reserve(meshCount);
    std::vector<std::vector<uint32_t>> perMeshBaseIndex; // parallel to m_meshes (for corrective mapping)
    if (hasCorrectives) {
        m_baseVertices.reserve(meshCount);
        perMeshBaseIndex.reserve(meshCount);
    }
    for (const MeshData& meshData : data.meshes) {
        if (meshData.indices.empty()) {
            continue;
        }
        m_meshes.emplace_back(m_context, meshData, materialSetLayout, m_materialPool, fallbackDiffuse,
                              fallbackNormal, batch);
        if (hasCorrectives) {
            m_baseVertices.push_back(meshData.vertices);    // uncorrected base, for re-morphing
            perMeshBaseIndex.push_back(meshData.baseVertex); // source base index per render vertex
        }
    }
    batch.submitAndWait();

    if (hasCorrectives) {
        buildRuntimeCorrectives(data, perMeshBaseIndex);
    }

    // Build the runtime skeleton from the model's bones (empty for a static model). inverseBind and
    // localBind are precomputed; computeSkinMatrices() then fills the joint buffer — every skin
    // matrix is identity at bind pose, so a freshly imported model draws exactly as its geometry.
    m_bones.resize(data.bones.size());
    for (std::size_t i = 0; i < data.bones.size(); ++i) {
        const ModelBone& src = data.bones[i];
        GpuBone& bone = m_bones[i];
        bone.parent = src.parent;
        bone.inverseBind = glm::inverse(src.bindGlobal);
        bone.localBind = (src.parent >= 0 && src.parent < static_cast<int>(data.bones.size()))
                             ? glm::inverse(data.bones[src.parent].bindGlobal) * src.bindGlobal
                             : src.bindGlobal;
        bone.orient = eulerMatrix(src.orientation, "XYZ"); // rest orientation frame for pose rotations
        bone.invOrient = glm::inverse(bone.orient);
        bone.rotationOrder = src.rotationOrder;
        bone.rotMin = src.rotationMin;
        bone.rotMax = src.rotationMax;
        bone.rotLimited = src.rotationLimited;
        bone.poseLocal = bone.localBind;
        m_boneIndex.emplace(src.name, static_cast<int>(i));
        m_boneNames.push_back(src.name);
    }
    m_boneEuler.assign(m_bones.size(), glm::vec3(0.0f));
    m_boneWorldPos.assign(m_bones.size(), glm::vec3(0.0f));
    m_boneGizmoFrame.assign(m_bones.size(), glm::mat4(1.0f));
    m_jointCount = static_cast<uint32_t>(m_bones.empty() ? 1 : m_bones.size());

    // Host-mapped storage buffer of skin matrices (updated in-place when the pose changes).
    m_jointBuffer = VulkanBuffer(m_context, m_jointCount * sizeof(glm::mat4),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                     VMA_ALLOCATION_CREATE_MAPPED_BIT);

    VkDescriptorSetAllocateInfo jointAlloc{};
    jointAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    jointAlloc.descriptorPool = m_materialPool;
    jointAlloc.descriptorSetCount = 1;
    jointAlloc.pSetLayouts = &jointSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_context.device(), &jointAlloc, &m_jointSet));

    VkDescriptorBufferInfo jointInfo{};
    jointInfo.buffer = m_jointBuffer.handle();
    jointInfo.offset = 0;
    jointInfo.range = m_jointCount * sizeof(glm::mat4);

    VkWriteDescriptorSet jointWrite{};
    jointWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    jointWrite.dstSet = m_jointSet;
    jointWrite.dstBinding = 0;
    jointWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    jointWrite.descriptorCount = 1;
    jointWrite.pBufferInfo = &jointInfo;
    vkUpdateDescriptorSets(m_context.device(), 1, &jointWrite, 0, nullptr);

    computeSkinMatrices(); // bind pose (all skin matrices identity) until a caller poses a bone
}

void Model::computeSkinMatrices() {
    auto* dst = static_cast<glm::mat4*>(m_jointBuffer.mappedData());
    if (dst == nullptr) {
        return;
    }
    if (m_bones.empty()) {
        dst[0] = glm::mat4(1.0f); // static model: a single identity joint its vertices bind to
        return;
    }
    // Accumulate each bone's posed global transform down the hierarchy (parents precede children),
    // then skinMatrix = poseGlobal · inverseBind.
    std::vector<glm::mat4> poseGlobal(m_bones.size());
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const GpuBone& bone = m_bones[i];
        poseGlobal[i] =
            (bone.parent >= 0) ? poseGlobal[bone.parent] * bone.poseLocal : bone.poseLocal;
        dst[i] = poseGlobal[i] * bone.inverseBind;
        // The joint's world position (for the posing overlay / picking) = model · poseGlobal · origin.
        m_boneWorldPos[i] = glm::vec3(m_transform * poseGlobal[i][3]);
        // Gizmo frame: the joint's rotation-channel axes = parentGlobal · localBind · orient (the frame
        // R(euler) acts in, i.e. before this joint's own rotation). Its origin equals the joint origin.
        const glm::mat4 parentGlobal =
            (bone.parent >= 0) ? poseGlobal[bone.parent] : glm::mat4(1.0f);
        m_boneGizmoFrame[i] = parentGlobal * bone.localBind * bone.orient;
    }
}

bool Model::selectedBoneFrame(glm::vec3& center, glm::mat3& axes) const {
    if (m_selectedBone < 0 || m_selectedBone >= static_cast<int>(m_bones.size())) {
        return false;
    }
    const glm::mat4 f = m_transform * m_boneGizmoFrame[static_cast<std::size_t>(m_selectedBone)];
    center = glm::vec3(f[3]);
    axes = glm::mat3(glm::normalize(glm::vec3(f[0])), glm::normalize(glm::vec3(f[1])),
                     glm::normalize(glm::vec3(f[2])));
    return true;
}

void Model::clampBoneEuler(int index) {
    if (index < 0 || index >= static_cast<int>(m_bones.size())) {
        return;
    }
    const GpuBone& bone = m_bones[static_cast<std::size_t>(index)];
    glm::vec3& e = m_boneEuler[static_cast<std::size_t>(index)];
    for (int a = 0; a < 3; ++a) {
        if (bone.rotLimited[a]) {
            e[a] = glm::clamp(e[a], bone.rotMin[a], bone.rotMax[a]);
        }
    }
}

void Model::applyBoneEuler(int index) {
    if (index < 0 || index >= static_cast<int>(m_bones.size())) {
        return;
    }
    clampBoneEuler(index); // keep the joint within its anatomical range of motion
    GpuBone& bone = m_bones[static_cast<std::size_t>(index)];
    const glm::mat4 rot = eulerMatrix(m_boneEuler[static_cast<std::size_t>(index)], bone.rotationOrder);
    bone.poseLocal = bone.localBind * bone.orient * rot * bone.invOrient;
    computeSkinMatrices();
}

void Model::nudgeSelectedBone(const glm::vec3& deltaEulerDegrees) {
    if (m_selectedBone < 0 || m_selectedBone >= static_cast<int>(m_bones.size())) {
        return;
    }
    m_boneEuler[static_cast<std::size_t>(m_selectedBone)] += deltaEulerDegrees;
    applyBoneEuler(m_selectedBone);
}

std::vector<std::pair<std::string, glm::vec3>> Model::capturePose() const {
    std::vector<std::pair<std::string, glm::vec3>> pose;
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const glm::vec3& e = m_boneEuler[i];
        if (e.x != 0.0f || e.y != 0.0f || e.z != 0.0f) {
            pose.emplace_back(m_boneNames[i], e);
        }
    }
    return pose;
}

void Model::applyPose(const std::vector<std::pair<std::string, glm::vec3>>& pose) {
    for (glm::vec3& e : m_boneEuler) {
        e = glm::vec3(0.0f); // reset to bind pose first
    }
    for (const auto& [name, euler] : pose) {
        const auto it = m_boneIndex.find(name);
        if (it != m_boneIndex.end()) {
            m_boneEuler[static_cast<std::size_t>(it->second)] = euler;
        }
    }
    // Recompute every bone's posed local transform from its (limit-clamped) Euler, then the skin
    // matrices once. Clamping here too keeps a loaded pose within the figure's range of motion.
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        clampBoneEuler(static_cast<int>(i));
        GpuBone& b = m_bones[i];
        b.poseLocal = b.localBind * b.orient * eulerMatrix(m_boneEuler[i], b.rotationOrder) * b.invOrient;
    }
    computeSkinMatrices();
    refreshCorrectives(); // re-morph for the restored pose
}

void Model::setBoneRotation(const std::string& boneName, const glm::vec3& eulerDegrees) {
    const auto it = m_boneIndex.find(boneName);
    if (it == m_boneIndex.end()) {
        return;
    }
    // applyBoneEuler poses in the joint's oriented frame: localBind · orient · R(euler, order) ·
    // orient⁻¹. At rest (euler 0) orient·orient⁻¹ cancels, leaving the bind pose; a rotation then acts
    // about the joint's true axes (an elbow bends anatomically), composed in its rotation order.
    m_boneEuler[static_cast<std::size_t>(it->second)] = eulerDegrees;
    applyBoneEuler(it->second);
    refreshCorrectives();
}

void Model::buildRuntimeCorrectives(const ModelData& data,
                                    const std::vector<std::vector<uint32_t>>& perMeshBaseIndex) {
    // Invert each mesh's baseVertex array: base vertex -> the render (mesh, local) vertices it fed. A
    // base vertex on a UV/zone seam feeds several render vertices (possibly across meshes), so a
    // corrective's single delta must reach all of them.
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> baseToRender;
    for (uint32_t mi = 0; mi < perMeshBaseIndex.size(); ++mi) {
        const std::vector<uint32_t>& bidx = perMeshBaseIndex[mi];
        for (uint32_t lv = 0; lv < bidx.size(); ++lv) {
            baseToRender[bidx[lv]].emplace_back(mi, lv);
        }
    }

    std::unordered_set<uint32_t> affected;
    m_correctives.reserve(data.correctives.size());
    for (const PoseCorrective& pc : data.correctives) {
        RuntimeCorrective rc;
        rc.sumFormulas = pc.sumFormulas;
        rc.gateScale = pc.gateScale;
        std::unordered_map<uint32_t, std::size_t> slotOfMesh; // mesh index -> index into rc.meshDeltas
        for (const auto& [baseIdx, delta] : pc.deltas) {
            const auto it = baseToRender.find(baseIdx);
            if (it == baseToRender.end()) {
                continue; // delta targets a vertex not present in any rendered mesh
            }
            for (const auto& [mi, lv] : it->second) {
                std::size_t slot;
                const auto s = slotOfMesh.find(mi);
                if (s == slotOfMesh.end()) {
                    slot = rc.meshDeltas.size();
                    slotOfMesh.emplace(mi, slot);
                    RuntimeCorrective::MeshDelta md;
                    md.mesh = mi;
                    rc.meshDeltas.push_back(std::move(md));
                } else {
                    slot = s->second;
                }
                rc.meshDeltas[slot].localVertex.push_back(lv);
                rc.meshDeltas[slot].delta.push_back(delta);
            }
        }
        if (rc.meshDeltas.empty()) {
            continue; // none of this corrective's deltas landed on rendered geometry
        }
        for (const RuntimeCorrective::MeshDelta& md : rc.meshDeltas) {
            affected.insert(md.mesh);
        }
        m_correctives.push_back(std::move(rc));
    }

    m_correctiveWeight.assign(m_correctives.size(), 0.0f);
    m_correctiveMeshes.assign(affected.begin(), affected.end());
    std::sort(m_correctiveMeshes.begin(), m_correctiveMeshes.end());

    // Base vertices are only needed for meshes a corrective actually touches; free the rest (and all
    // of them if no corrective survived).
    if (m_correctives.empty()) {
        m_baseVertices.clear();
    } else {
        for (uint32_t mi = 0; mi < m_baseVertices.size(); ++mi) {
            if (affected.find(mi) == affected.end()) {
                std::vector<Vertex>().swap(m_baseVertices[mi]);
            }
        }
    }
}

float Model::evalCorrectiveWeight(std::size_t correctiveIndex) const {
    const RuntimeCorrective& rc = m_correctives[correctiveIndex];
    float sum = 0.0f;
    std::vector<float> st;
    st.reserve(8);
    for (const CorrectiveFormula& f : rc.sumFormulas) {
        st.clear();
        for (const CorrectiveOp& op : f.ops) {
            switch (op.kind) {
                case CorrectiveOp::Kind::PushRotation: {
                    float angle = 0.0f;
                    const auto it = m_boneIndex.find(op.bone);
                    if (it != m_boneIndex.end()) {
                        angle = m_boneEuler[static_cast<std::size_t>(it->second)][op.axis];
                    }
                    st.push_back(angle);
                    break;
                }
                case CorrectiveOp::Kind::PushConst:
                    st.push_back(op.value);
                    break;
                case CorrectiveOp::Kind::Spline: {
                    const float d = st.empty() ? 0.0f : st.back();
                    if (!st.empty()) {
                        st.pop_back();
                    }
                    st.push_back(evalSpline(op.knots, d));
                    break;
                }
                case CorrectiveOp::Kind::Mult:
                    if (st.size() >= 2) {
                        const float b = st.back();
                        st.pop_back();
                        st.back() *= b;
                    }
                    break;
                case CorrectiveOp::Kind::Add:
                    if (st.size() >= 2) {
                        const float b = st.back();
                        st.pop_back();
                        st.back() += b;
                    }
                    break;
            }
        }
        sum += st.empty() ? 0.0f : st.back();
    }
    return sum * rc.gateScale;
}

void Model::refreshCorrectives() {
    if (m_correctives.empty()) {
        return;
    }
    // Re-evaluate weights; bail if none moved enough to matter (the common case mid-drag / for a joint
    // that has no corrective).
    bool changed = false;
    for (std::size_t i = 0; i < m_correctives.size(); ++i) {
        const float w = evalCorrectiveWeight(i);
        if (std::fabs(w - m_correctiveWeight[i]) > 1e-4f) {
            changed = true;
        }
        m_correctiveWeight[i] = w;
    }
    if (!changed) {
        return;
    }

    // Re-morph each affected mesh from its base and re-upload. The old buffers may be referenced by an
    // in-flight frame, so make the GPU idle first (this runs between frames on a settled pose, not per
    // drag-move, so the stall is infrequent).
    vkDeviceWaitIdle(m_context.device());
    ImmediateBatch batch(m_context);
    for (const uint32_t mi : m_correctiveMeshes) {
        std::vector<Vertex> verts = m_baseVertices[mi]; // start from the uncorrected base
        for (std::size_t c = 0; c < m_correctives.size(); ++c) {
            const float w = m_correctiveWeight[c];
            if (std::fabs(w) < 1e-4f) {
                continue;
            }
            for (const RuntimeCorrective::MeshDelta& md : m_correctives[c].meshDeltas) {
                if (md.mesh != mi) {
                    continue;
                }
                for (std::size_t k = 0; k < md.localVertex.size(); ++k) {
                    verts[md.localVertex[k]].pos += w * md.delta[k];
                }
            }
        }
        m_meshes[mi].reuploadVertices(m_context, verts, batch);
    }
    batch.submitAndWait();
}

Model::~Model() {
    // Destroying the pool frees all of this model's set-1 descriptors; the meshes (textures +
    // buffers) are freed afterwards as members. The device is idle by the time a Model dies
    // (Scene waits before tearing models down), so the freed sets are never referenced again.
    if (m_materialPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_context.device(), m_materialPool, nullptr);
    }
}

void Model::record(VkCommandBuffer cmd, VkPipelineLayout layout, bool transparentPass) const {
    // Set 2 = this model's skin matrices (shared by all its meshes).
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &m_jointSet, 0,
                            nullptr);
    for (const Mesh& mesh : m_meshes) {
        if (mesh.isTransparent() == transparentPass) {
            mesh.record(cmd, layout, m_transform);
        }
    }
}

bool Model::intersectRay(const Ray& ray, float& tOut) const {
    if (!m_hasBounds) {
        return false;
    }
    // Transform the ray into local space so we can slab-test the AABB directly (equivalent to an
    // oriented-box test in world space, but cheaper). The transform is affine, so the ray
    // parameter t is preserved — the t we find is the same world-space distance for every model.
    const glm::mat4 inv = glm::inverse(m_transform);
    const glm::vec3 o = glm::vec3(inv * glm::vec4(ray.origin, 1.0f));
    const glm::vec3 d = glm::vec3(inv * glm::vec4(ray.direction, 0.0f));

    float tMin = 0.0f; // ignore hits behind the ray origin
    float tMax = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) < 1e-8f) {
            // Ray parallel to this slab: miss unless the origin is already within it.
            if (o[axis] < m_boundsMin[axis] || o[axis] > m_boundsMax[axis]) {
                return false;
            }
            continue;
        }
        const float invD = 1.0f / d[axis];
        float t1 = (m_boundsMin[axis] - o[axis]) * invD;
        float t2 = (m_boundsMax[axis] - o[axis]) * invD;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) {
            return false;
        }
    }
    tOut = tMin;
    return true;
}

} // namespace pose

/**
 * @file model.cpp
 * @brief Model construction (upload, correctives, armature, descriptors), the record paths, and
 *        the picking/grounding queries. See model.h.
 */

#include "model.h"

#include "descriptorutil.h"
#include "modeldata.h"
#include "ray.h"
#include "textureuploadcache.h"
#include "vertex.h"
#include "vulkancommands.h"
#include "vulkancontext.h"
#include "vulkanimage.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace pose {

Model::Model(VulkanContext& context, const ModelData& data, VkDescriptorSetLayout materialSetLayout,
             VkDescriptorSetLayout poseSetLayout, const VulkanTexture& fallbackDiffuse,
             const VulkanTexture& fallbackNormal) {
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

    // Descriptors from this model's pool: one set-1 per mesh (six samplers each: diffuse, detail,
    // roughness map, spec mask, translucency map, micro-detail normal) + one set-2 pose set per
    // frame slot (joints + corrective weights + deltas).
    const VkDevice device = context.device();
    m_descriptorPool = createDescriptorPool(
        device,
        {poolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, meshCount * 6),
         poolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxFramesInFlight * 3)},
        meshCount + kMaxFramesInFlight);

    // One upload batch for the whole model: every mesh's vertex/index buffers and texture record
    // into it, and it's submitted exactly once below — collapsing what used to be ~3 blocking
    // submits per mesh (the dominant cost of importing a model with many material groups) into a
    // single GPU round-trip. Trade-off: all staging buffers are held until that submit completes,
    // so peak staging memory is the sum across meshes rather than one-at-a-time.
    ImmediateBatch batch(context);
    // Meshes sharing a DecodedImage (zones sampling the same atlas file) share one VulkanTexture
    // through this cache — one staging upload + mip chain per unique image instead of per mesh.
    TextureUploadCache uploads;
    m_meshes.reserve(meshCount);
    const bool skinned = !data.bones.empty();
    // The armature — the runtime skeleton + pose (see armature.h) — built from the model's bones
    // (empty for a static model: one identity joint) BEFORE the meshes upload: the soft-body delta
    // fields it lays over the skin (the buttocks, armaturebuttocks.cpp) ride in the vertex data
    // as the pose correctives' ranges do, and they need the rig's roles and the body mesh sample.
    // Bind transforms are translation-only, so each bone's local rest offset is the difference of
    // the global rest positions.
    for (const MeshData& mesh : data.meshes) {
        m_linearSkinning = m_linearSkinning || mesh.linearSkinning; // (an FBX importer sets it on every mesh)
    }
    std::vector<ArmatureBone> armatureBones(data.bones.size());
    for (std::size_t i = 0; i < data.bones.size(); ++i) {
        const ModelBone& src = data.bones[i];
        ArmatureBone& dst = armatureBones[i];
        dst.name = src.name;
        dst.parent = src.parent;
        const glm::vec3 parentGlobal =
            (src.parent >= 0 && src.parent < static_cast<int>(data.bones.size()))
                ? glm::vec3(data.bones[static_cast<std::size_t>(src.parent)].bindGlobal[3])
                : glm::vec3(0.0f);
        dst.localBindTranslation = glm::vec3(src.bindGlobal[3]) - parentGlobal;
        dst.orientation = src.orientation;
        dst.rotationOrder = src.rotationOrder;
        dst.rotationMin = src.rotationMin;
        dst.rotationMax = src.rotationMax;
        dst.rotationLimited = src.rotationLimited;
        dst.drivenBy = src.drivenBy;
        dst.driveFactor = src.driveFactor;
        dst.roleHint = src.roleHint;
    }
    m_armature.build(armatureBones);
    // The BODY MESH SAMPLE for the self-collision volumes (bodymesh.h): every skinned vertex's
    // bind position with the bone that weighs most on it, handed to the armature.
    auto dominantJoint = [&](const Vertex& v) -> int {
        int best = 0;
        for (int j = 1; j < 4; ++j) {
            if (v.weights[j] > v.weights[best]) {
                best = j;
            }
        }
        return (v.weights[best] > 0.0f && v.joints[best] < data.bones.size()) ? static_cast<int>(v.joints[best]) : -1;
    };
    if (skinned) {
        std::vector<BodyMeshPoint> bodyMesh;
        for (const MeshData& meshData : data.meshes) {
            if (meshData.indices.empty()) {
                continue;
            }
            for (const Vertex& v : meshData.vertices) {
                const int bone = dominantJoint(v);
                if (bone >= 0) {
                    bodyMesh.push_back({v.pos, bone});
                }
            }
        }
        if (!bodyMesh.empty()) {
            m_armature.setBodyMesh(std::move(bodyMesh));
        }
    }
    m_armature.prepareIkRig(); // (the rig and the standard's roles exist from the import on)
    m_armature.setupSoftBodyFields();
    // Debug hook: POSESTUDIO_DUMP_SKELETON=<path> dumps the imported skeleton (one bone per
    // line) so the IK harness (tools/ikharness/) can run its drag-loop tests against REAL figure
    // rigs instead of synthetic approximations. No-op unless the environment variable is set.
    if (const char* dumpPath = std::getenv("POSESTUDIO_DUMP_SKELETON");
        dumpPath != nullptr && m_armature.hasSkeleton()) {
        m_armature.dump(dumpPath);
    }

    // THE SOFT-BODY DELTA FIELDS (armaturebuttocks.cpp): per cheek two correctives — a delta of
    // the field's weight along the lateral axis and one along the trunk — on every render vertex
    // the field reaches, blended by the GPU at the mass's displacement.
    std::vector<SoftBodyField> softFields;
    if (skinned) {
        softFields.resize(4);
        for (std::size_t f = 0; f < softFields.size(); ++f) {
            softFields[f].id = std::string("softbody:buttock_") + (f < 2 ? "l_" : "r_") + (f % 2 == 0 ? "x" : "y");
            softFields[f].softIndex = static_cast<int>(f);
            softFields[f].perMesh.resize(meshCount);
        }
        uint32_t k = 0;
        bool     any = false;
        for (const MeshData& meshData : data.meshes) {
            if (meshData.indices.empty()) {
                continue;
            }
            for (uint32_t lv = 0; lv < meshData.vertices.size(); ++lv) {
                const Vertex& v = meshData.vertices[lv];
                int           side = -1;
                const float   w = m_armature.softBodyFieldWeight(v.pos, dominantJoint(v), side);
                if (w > 1e-3f && side >= 0) {
                    softFields[static_cast<std::size_t>(side) * 2].perMesh[k].push_back({lv, glm::vec3(w, 0.0f, 0.0f)});
                    softFields[static_cast<std::size_t>(side) * 2 + 1].perMesh[k].push_back({lv, glm::vec3(0.0f, w, 0.0f)});
                    any = true;
                }
            }
            ++k;
        }
        if (!any) {
            softFields.clear();
        }
    }
    // Pose correctives (and the soft-body fields) resolve BEFORE the meshes upload: each touched
    // vertex's packed range into the GPU delta table rides in its vertex data (see Mesh's
    // correctiveRanges parameter).
    std::vector<std::vector<uint32_t>> perMeshRanges; // parallel to m_meshes; empty = untouched mesh
    std::vector<CorrectiveEntry>       correctiveEntries;
    if (!data.correctives.empty() || !softFields.empty()) {
        m_correctives.build(data, perMeshRanges, correctiveEntries, softFields);
    }
    if (skinned) {
        // Ground samples: the skinning inputs groundGap() needs to find the posed lowest point
        // (the GPU buffers are device-local and unreadable). Reserved once, summed across meshes
        // (see GroundSampler::reserve).
        std::size_t totalVerts = 0;
        std::size_t totalTris = 0;
        for (const MeshData& meshData : data.meshes) {
            if (!meshData.indices.empty()) {
                totalVerts += meshData.vertices.size();
                if (meshData.opacity >= 0.999f && !meshData.hasOpacityMask) {
                    totalTris += meshData.indices.size() / 3; // (the pickable surface: Model::pickSurface)
                }
            }
        }
        m_ground.reserve(totalVerts, totalTris);
    }
    for (const MeshData& meshData : data.meshes) {
        if (meshData.indices.empty()) {
            continue;
        }
        const std::size_t mi = m_meshes.size();
        const std::vector<uint32_t>* ranges =
            (mi < perMeshRanges.size() && !perMeshRanges[mi].empty()) ? &perMeshRanges[mi] : nullptr;
        m_meshes.emplace_back(context, meshData, materialSetLayout, m_descriptorPool.get(),
                              fallbackDiffuse, fallbackNormal, uploads, batch, ranges);
        if (skinned) {
            // (Opaque skin only is pickable: a click on the eye's clear shells is a click on the eye.)
            m_ground.add(meshData.vertices, meshData.indices, meshData.opacity >= 0.999f && !meshData.hasOpacityMask);
        }
    }
    m_correctives.createBuffers(context, correctiveEntries, batch);
    batch.submitAndWait();

    // Partition the meshes for the record paths (m_meshes is complete and never reallocates
    // again, so the pointers stay valid).
    for (const Mesh& mesh : m_meshes) {
        (mesh.isTransparent() ? m_transparentMeshes : m_opaqueMeshes).push_back(&mesh);
    }
    m_jointCount = m_armature.jointCount();

    // Host-mapped storage buffers of skinning DUAL QUATERNIONS (2 vec4 per joint — see
    // Armature::skinDualQuats) — ONE PER FRAME IN FLIGHT (a single shared buffer raced the GPU
    // and tore skinned frames). Each frame slot gets its own buffer + set-2 descriptor;
    // uploadJointsIfDirty() fills the current slot's buffer at record time, when that slot's
    // fence guarantees the GPU is done with it.
    // (TWICE the joints: the second half carries, per joint, whether its flesh is lit with the
    // selection — see m_highlightUploaded; mesh.vert reads the joint count off the buffer's length.)
    allocateDescriptorSets(device, m_descriptorPool.get(), poseSetLayout, kMaxFramesInFlight,
                           m_jointSets.data());
    const VkDeviceSize jointBytes = m_jointCount * 4 * sizeof(glm::vec4);
    for (int f = 0; f < kMaxFramesInFlight; ++f) {
        const auto slot = static_cast<std::size_t>(f);
        m_jointBuffers[slot] =
            VulkanBuffer(context, jointBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT);
        // Set 2 for this slot: 0 = joints, 1 = corrective weights, 2 = the shared delta table.
        const VkDescriptorBufferInfo infos[3] = {
            bufferInfo(m_jointBuffers[slot].handle(), jointBytes),
            bufferInfo(m_correctives.weightBuffer(static_cast<uint32_t>(f))),
            bufferInfo(m_correctives.deltaBuffer()),
        };
        writeBufferDescriptors(device, m_jointSets[slot], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               infos, 3);
    }
}

// The pool frees every set it issued; the meshes (textures + buffers) and the pose buffers are
// freed as members before it (reverse declaration order). The device is idle by the time a
// Model dies (Scene waits before tearing models down), so nothing in flight references them.
Model::~Model() = default;

void Model::uploadJointsIfDirty(uint32_t frameIndex) {
    m_correctives.uploadIfDirty(frameIndex, m_armature); // the pose's other per-frame data
    const std::vector<glm::vec4>& quats = m_armature.skinDualQuats();
    if (quats.empty() || frameIndex >= static_cast<uint32_t>(kMaxFramesInFlight)) {
        return;
    }
    auto* dst = static_cast<glm::vec4*>(m_jointBuffers[frameIndex].mappedData());
    if (dst == nullptr) {
        return;
    }
    // The buffer's second half: which joints' flesh is lit WITH the selection (the face rig under
    // a selected head) — per slot, when the selection has changed since the slot was written.
    const std::uint64_t selectionKey = (static_cast<std::uint64_t>(m_armature.groupVersion()) << 32) |
                                       static_cast<std::uint32_t>(m_armature.highlightBone() + 2);
    if (m_highlightUploaded[frameIndex] != selectionKey) {
        glm::vec4* lit = dst + std::size_t{2} * m_jointCount;
        for (uint32_t j = 0; j < m_jointCount; ++j) {
            lit[std::size_t{2} * j] = glm::vec4(m_armature.highlightedWithSelection(static_cast<int>(j)) ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
            lit[std::size_t{2} * j + 1] = glm::vec4(0.0f);
        }
        // ... and, in entry 0's .y, whether this model's skin blends LINEARLY (skinning.glsl).
        if (m_jointCount > 0) {
            lit[0].y = m_linearSkinning ? 1.0f : 0.0f;
        }
        m_highlightUploaded[frameIndex] = selectionKey;
    }
    if (m_jointUploaded[frameIndex] == m_armature.skinVersion()) {
        return;
    }
    const std::size_t bytes =
        std::min(quats.size(), std::size_t{2} * m_jointCount) * sizeof(glm::vec4);
    std::memcpy(dst, quats.data(), bytes);
    m_jointUploaded[frameIndex] = m_armature.skinVersion();
}

void Model::setBoneRotation(const std::string& boneName, const glm::vec3& eulerDegrees) {
    if (m_armature.setBoneRotation(boneName, eulerDegrees)) {
        refreshCorrectives();
    }
}

void Model::applyPose(const std::vector<std::pair<std::string, glm::vec3>>& pose) {
    m_armature.applyPose(pose);
    refreshCorrectives(); // the restored pose's corrective weights
}

void Model::setTransform(const glm::mat4& transform) {
    m_armature.setTransform(transform);
}

void Model::bindPose(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t setIndex,
                     uint32_t frameIndex) {
    // A model whose meshes were all index-empty never created its pool/pose sets (the
    // constructor early-returns) — binding the VK_NULL_HANDLE set would be invalid Vulkan usage.
    if (m_meshes.empty()) {
        return;
    }
    uploadJointsIfDirty(frameIndex);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, setIndex, 1,
                            &m_jointSets[frameIndex], 0, nullptr);
}

void Model::recordShaded(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex,
                         MeshDrawKind kind, MeshFilter filter) {
    if (m_meshes.empty()) {
        return;
    }
    // Set 2 = this frame slot's pose data (shared by all this model's meshes), uploaded first if
    // the slot hasn't seen the pose (a no-op when the shadow pass already did this frame).
    bindPose(cmd, layout, 2, frameIndex);
    const glm::mat4& transform = m_armature.transform();
    const int selected = m_armature.highlightBone();
    const int twin = m_armature.selectedHighlightTwin();
    if (filter == MeshFilter::Opaque) {
        for (const Mesh* mesh : m_opaqueMeshes) {
            mesh->record(cmd, layout, transform, kind, selected, twin);
        }
    } else {
        for (const Mesh& mesh : m_meshes) { // every mesh: cards and shells are geometry too
            mesh.record(cmd, layout, transform, kind, selected, twin);
        }
    }
}

void Model::recordPositionOnly(VkCommandBuffer cmd, VkPipelineLayout layout,
                               const glm::mat4& viewProj, uint32_t frameIndex, MeshFilter filter) {
    if (m_meshes.empty()) {
        return;
    }
    // The position-only pipelines' only set (index 0) is the pose layout — the same set object
    // the main pass binds at index 2 (set compatibility is by layout, not index), so a posed
    // figure casts its posed shadow / traces its posed silhouette. The shadow pass runs first in
    // the frame, so it typically performs the frame slot's pose upload.
    bindPose(cmd, layout, 0, frameIndex);
    ShadowPushConstants push{};
    push.viewProj = viewProj;
    push.model = m_armature.transform();
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    if (filter == MeshFilter::Opaque) {
        for (const Mesh* mesh : m_opaqueMeshes) {
            mesh->recordDepth(cmd);
        }
    } else {
        for (const Mesh& mesh : m_meshes) {
            if (filter == MeshFilter::All || !mesh.hasOpacityMask()) {
                mesh.recordDepth(cmd);
            }
        }
    }
}

void Model::recordOpaque(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex) {
    recordShaded(cmd, layout, frameIndex, MeshDrawKind::Opaque, MeshFilter::Opaque);
}

void Model::recordWire(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex) {
    recordShaded(cmd, layout, frameIndex, MeshDrawKind::Wire, MeshFilter::All);
}

void Model::recordDepthFill(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex) {
    recordShaded(cmd, layout, frameIndex, MeshDrawKind::DepthOnly, MeshFilter::Opaque);
}

void Model::recordShadow(VkCommandBuffer cmd, VkPipelineLayout layout,
                         const glm::mat4& lightViewProj, uint32_t frameIndex) {
    // No alpha in this pass — cards/shells would cast solid shadows, so only the opaque meshes.
    recordPositionOnly(cmd, layout, lightViewProj, frameIndex, MeshFilter::Opaque);
}

void Model::recordSilhouette(VkCommandBuffer cmd, VkPipelineLayout layout,
                             const glm::mat4& viewProj, uint32_t frameIndex) {
    // A cutout card's real shape lives in a map this pass can't read, so those are left out;
    // translucent shells keep their silhouette.
    recordPositionOnly(cmd, layout, viewProj, frameIndex, MeshFilter::NotMasked);
}

glm::vec3 Model::posedCentroid(const Mesh& mesh) const {
    glm::vec3 local = mesh.centroid();
    if (m_armature.hasSkeleton()) {
        const std::size_t joint = mesh.dominantJoint();
        if (joint < m_armature.boneCount()) {
            local = glm::vec3(m_armature.poseGlobal(joint) * m_armature.inverseBind(joint) *
                              glm::vec4(local, 1.0f));
        }
    }
    return glm::vec3(m_armature.transform() * glm::vec4(local, 1.0f));
}

bool Model::worldBounds(glm::vec3& outMin, glm::vec3& outMax) const {
    if (!m_hasBounds) {
        return false;
    }
    // Transform all 8 local AABB corners (the transform may rotate; min/max of the corners is the
    // tight world AABB of the local box).
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? m_boundsMax.x : m_boundsMin.x,
                               (i & 2) ? m_boundsMax.y : m_boundsMin.y,
                               (i & 4) ? m_boundsMax.z : m_boundsMin.z);
        const glm::vec3 world = glm::vec3(m_armature.transform() * glm::vec4(corner, 1.0f));
        outMin = glm::min(outMin, world);
        outMax = glm::max(outMax, world);
    }
    // A POSED figure can leave its bind-pose box (a forward lean carries the head well outside
    // it), and the shadow frustum is fitted from these bounds — geometry crossing the fitted
    // map's edge showed as transient dark bands/streaks while posing. Union in the posed bone
    // positions, padded by a flesh margin (the skin extends past the joints — the skull above
    // the head joint, toes past the toe joints). Static models are exact via the box alone.
    if (m_armature.hasSkeleton()) {
        constexpr float kFleshMargin = 0.25f;
        glm::vec3 boneMin(std::numeric_limits<float>::max());
        glm::vec3 boneMax(std::numeric_limits<float>::lowest());
        for (std::size_t i = 0; i < m_armature.boneCount(); ++i) {
            const glm::vec3& p = m_armature.boneWorldPosition(i);
            boneMin = glm::min(boneMin, p);
            boneMax = glm::max(boneMax, p);
        }
        outMin = glm::min(outMin, boneMin - glm::vec3(kFleshMargin));
        outMax = glm::max(outMax, boneMax + glm::vec3(kFleshMargin));
    }
    return true;
}

bool Model::intersectRay(const Ray& ray, float& tOut) const {
    if (!m_hasBounds) {
        return false;
    }
    if (m_armature.hasSkeleton()) {
        // A POSED figure is picked against the world-space box of its live joints plus a flesh
        // margin, not its bind box: full-body IK walks and crouches move the whole figure
        // through the hip's pose translation, so after a walk the mesh sat far outside the
        // bind bounds — a right-click on the character found its joints (those are picked by
        // live position) but not the model, and the context menu lost its Delete entry.
        constexpr float kFleshMargin = 0.16f; // metres: the torso's depth around the spine
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        for (std::size_t i = 0; i < m_armature.boneCount(); ++i) {
            const glm::vec3& p = m_armature.boneWorldPosition(i);
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        lo -= glm::vec3(kFleshMargin);
        hi += glm::vec3(kFleshMargin);
        float tMin = 0.0f;
        float tMax = std::numeric_limits<float>::max();
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(ray.direction[axis]) < 1e-8f) {
                if (ray.origin[axis] < lo[axis] || ray.origin[axis] > hi[axis]) {
                    return false;
                }
                continue;
            }
            const float invD = 1.0f / ray.direction[axis];
            float t1 = (lo[axis] - ray.origin[axis]) * invD;
            float t2 = (hi[axis] - ray.origin[axis]) * invD;
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
    // Transform the ray into local space so we can slab-test the AABB directly (equivalent to an
    // oriented-box test in world space, but cheaper). The transform is affine, so the ray
    // parameter t is preserved — the t we find is the same world-space distance for every model.
    const glm::mat4 inv = glm::inverse(m_armature.transform());
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

bool Model::groundGap(float& lowestY) const {
    return m_ground.lowestY(m_armature, m_boundsMin, m_boundsMax, m_hasBounds, lowestY);
}

bool Model::pickSurface(const Ray& ray, float& tOut, glm::vec3& hitWorld, int& bone) const {
    return m_ground.pickSurface(m_armature, ray, tOut, hitWorld, bone);
}

} // namespace pose

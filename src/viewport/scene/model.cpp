/**
 * @file model.cpp
 * @brief Model construction (upload, correctives, armature, descriptors), the record paths, the
 *        bounds and picking/grounding queries, and the zones' materials, texture slots and
 *        smoothing (the Materials tab's Model side). See model.h.
 */

#include "model.h"

#include "splatcloud.h"
#include "splatimporter.h"

#include "descriptorutil.h"
#include "modeldata.h"
#include "ray.h"
#include "smoothnormals.h"
#include "tangentgen.h"
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

namespace {

/// The joint that weighs most on vertex @p v — whose skin it is, for the body mesh sample and
/// the soft-body fields — or -1 for an unweighted vertex or a joint index past @p boneCount.
int dominantJointOf(const Vertex& v, std::size_t boneCount) {
    int best = 0;
    for (int j = 1; j < 4; ++j) {
        if (v.weights[j] > v.weights[best]) {
            best = j;
        }
    }
    return (v.weights[best] > 0.0f && v.joints[best] < boneCount) ? static_cast<int>(v.joints[best]) : -1;
}

/// The importer's bones as the armature takes them. Bind transforms are translation-only, so
/// each bone's local rest offset is the difference of the global rest positions.
std::vector<ArmatureBone> armatureBonesOf(const ModelData& data) {
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
    return armatureBones;
}

/// The BODY MESH SAMPLE for the self-collision volumes (bodymesh.h): every skinned vertex's
/// bind position with the bone that weighs most on it.
std::vector<BodyMeshPoint> bodyMeshOf(const ModelData& data) {
    std::vector<BodyMeshPoint> bodyMesh;
    for (const MeshData& meshData : data.meshes) {
        if (meshData.indices.empty()) {
            continue;
        }
        for (const Vertex& v : meshData.vertices) {
            const int bone = dominantJointOf(v, data.bones.size());
            if (bone >= 0) {
                bodyMesh.push_back({v.pos, bone});
            }
        }
    }
    return bodyMesh;
}

/// THE SOFT-BODY DELTA FIELDS: per cheek two correctives (armaturebuttocks.cpp) — a delta of
/// the field's weight along the lateral axis and one along the trunk — and per BONELESS BREAST
/// three (armaturebreastfield.cpp: sideways, along the chest, out along the tether; none on a
/// figure whose breasts ride pectoral bones), on every render vertex the field reaches, blended
/// by the GPU at the mass's displacement. The indices are the armature's (softBodyWeight).
/// Per non-empty mesh of @p data (@p meshCount of them, in the Model's order); empty when no
/// field reaches a vertex. @p armature has its rig and its body mesh sample by now.
std::vector<SoftBodyField> softBodyFieldsOf(const ModelData& data, const Armature& armature, uint32_t meshCount) {
    std::vector<SoftBodyField> softFields;
    constexpr std::size_t kButtockFields = Armature::kSoftBodyButtockFields;
    constexpr std::size_t kBreastBase = Armature::kSoftBodyBreastBase;
    softFields.resize(kButtockFields + Armature::kSoftBodyBreastFields);
    for (std::size_t f = 0; f < softFields.size(); ++f) {
        if (f < kButtockFields) {
            softFields[f].id = std::string("softbody:buttock_") + (f < 2 ? "l_" : "r_") + (f % 2 == 0 ? "x" : "y");
        } else {
            const std::size_t b = f - kBreastBase;
            softFields[f].id = std::string("softbody:breast_") + (b < 3 ? "l_" : "r_") + "xyz"[b % 3];
        }
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
            const int     bone = dominantJointOf(v, data.bones.size());
            int           side = -1;
            const float   w = armature.softBodyFieldWeight(v.pos, bone, side);
            if (w > 1e-3f && side >= 0) {
                softFields[static_cast<std::size_t>(side) * 2].perMesh[k].push_back({lv, glm::vec3(w, 0.0f, 0.0f)});
                softFields[static_cast<std::size_t>(side) * 2 + 1].perMesh[k].push_back({lv, glm::vec3(0.0f, w, 0.0f)});
                any = true;
            }
            float breast[2] = {0.0f, 0.0f};
            armature.breastFieldWeights(v.pos, bone, breast);
            for (std::size_t s = 0; s < 2; ++s) {
                if (breast[s] > 1e-3f) {
                    for (int axis = 0; axis < 3; ++axis) {
                        glm::vec3 delta(0.0f);
                        delta[axis] = breast[s];
                        softFields[kBreastBase + s * 3 + static_cast<std::size_t>(axis)].perMesh[k].push_back({lv, delta});
                    }
                    any = true;
                }
            }
        }
        ++k;
    }
    if (!any) {
        softFields.clear();
    }
    return softFields;
}

} // namespace

Model::Model(VulkanContext& context, const ModelData& data, VkDescriptorSetLayout materialSetLayout,
             VkDescriptorSetLayout poseSetLayout, const VulkanTexture& fallbackDiffuse,
             const VulkanTexture& fallbackNormal) {
    // THE ORDER IS LOAD-BEARING. Each step needs the ones before it:
    //   1. the bounds and the descriptor pool (sized by the non-empty meshes);
    //   2. the armature, its body mesh sample, its rig and its soft-body setup — before the
    //      meshes, because
    //   3. the soft-body fields and the pose correctives are resolved into per-vertex ranges
    //      that ride in the vertex data as it uploads;
    //   4. the meshes (and the CPU surface beside them) and the corrective buffers, in ONE
    //      upload batch, submitted once;
    //   5. the mesh partition, then the per-frame pose + material buffers and their sets, which
    //      bind the correctives' buffers and are sized by the meshes.
    uint32_t meshCount = 0;
    for (const MeshData& meshData : data.meshes) {
        if (!meshData.indices.empty()) {
            ++meshCount;
        }
    }
    if (meshCount == 0) {
        // Nothing to draw; no pool/sets needed. The armature is built all the same — static:
        // the identity joint and the model transform — so that no Model exists with an unbuilt
        // one (armature.h's one contract). Scene::addModel does not keep such a model.
        m_armature.build({});
        return;
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

    // Descriptors from this model's pool: one set-1 per mesh (its kMaterialBindingCount samplers)
    // + one set-2 pose set per frame slot (joints + corrective weights + deltas + the materials).
    const VkDevice device = context.device();
    m_descriptorPool = createDescriptorPool(
        device,
        {poolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, meshCount * kMaterialBindingCount),
         poolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxFramesInFlight * 4)},
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
    for (const MeshData& mesh : data.meshes) {
        m_linearSkinning = m_linearSkinning || mesh.linearSkinning; // (an FBX importer sets it on every mesh)
    }
    m_armature.build(armatureBonesOf(data));
    if (skinned) {
        std::vector<BodyMeshPoint> bodyMesh = bodyMeshOf(data);
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

    // The soft-body delta fields the armature lays over the skin (softBodyFieldsOf).
    const std::vector<SoftBodyField> softFields =
        skinned ? softBodyFieldsOf(data, m_armature, meshCount) : std::vector<SoftBodyField>();
    // Pose correctives (and the soft-body fields) resolve BEFORE the meshes upload: each touched
    // vertex's packed range into the GPU delta table rides in its vertex data (see Mesh's
    // correctiveRanges parameter).
    std::vector<std::vector<uint32_t>> perMeshRanges; // parallel to m_meshes; empty = untouched mesh
    std::vector<CorrectiveEntry>       correctiveEntries;
    if (!data.correctives.empty() || !softFields.empty()) {
        m_correctives.build(data, perMeshRanges, correctiveEntries, softFields);
    }
    // Ground samples: the skinning inputs groundGap() needs to find the posed lowest point (the
    // GPU buffers are device-local and unreadable), and the SURFACE a click is on (pickSurface).
    // Reserved once, summed across meshes (see GroundSampler::reserve). A STATIC model keeps its
    // samples too — a click on a prop names its material zone for the Materials tab — unless it
    // is huge: past the cap its box alone is pickable, as before (a click's convenience is not
    // worth doubling a million-triangle scan's memory).
    constexpr std::size_t kStaticPickTriangleCap = 1000000;
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
    const bool sampled = skinned || totalTris <= kStaticPickTriangleCap;
    if (sampled) {
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
                              fallbackDiffuse, fallbackNormal, uploads, batch, ranges, static_cast<uint32_t>(mi));
        if (sampled) {
            // (Opaque skin only is pickable: a click on the eye's clear shells is a click on the eye.)
            m_ground.add(meshData.vertices, meshData.indices, meshData.opacity >= 0.999f && !meshData.hasOpacityMask,
                         static_cast<int>(mi));
        }
    }
    m_correctives.createBuffers(context, correctiveEntries, batch);
    batch.submitAndWait();

    // Partition the meshes for the record paths (m_meshes is complete and never reallocates
    // again, so the pointers stay valid).
    repartitionMeshes();
    m_jointCount = m_armature.jointCount();

    createPoseBuffers(context, poseSetLayout);
}

void Model::createPoseBuffers(VulkanContext& context, VkDescriptorSetLayout poseSetLayout) {
    const VkDevice device = context.device();
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
        // THE MATERIAL BUFFER (2026-11-09): every mesh's MaterialGpu, one host-mapped buffer per
        // frame slot like the joints' (a material edit rewrites the slot's entries at record time,
        // behind that slot's fence), bound as binding 3 of the same set.
        const VkDeviceSize materialBytes = static_cast<VkDeviceSize>(m_meshes.size()) * sizeof(MaterialGpu);
        m_materialBuffers[slot] =
            VulkanBuffer(context, materialBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT);
        // Set 2 for this slot: 0 = joints, 1 = corrective weights, 2 = the shared delta table,
        // 3 = the materials.
        const VkDescriptorBufferInfo infos[4] = {
            bufferInfo(m_jointBuffers[slot].handle(), jointBytes),
            bufferInfo(m_correctives.weightBuffer(static_cast<uint32_t>(f))),
            bufferInfo(m_correctives.deltaBuffer()),
            bufferInfo(m_materialBuffers[slot].handle(), materialBytes),
        };
        writeBufferDescriptors(device, m_jointSets[slot], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               infos, 4);
    }
}

void Model::uploadMaterialsIfDirty(uint32_t frameIndex) {
    if (frameIndex >= static_cast<uint32_t>(kMaxFramesInFlight) || m_meshes.empty()) {
        return;
    }
    // (revision + 1, so a never-written slot — 0 — is behind the first revision, 0.)
    if (m_materialUploaded[frameIndex] == m_materialRevision + 1) {
        return;
    }
    auto* dst = static_cast<MaterialGpu*>(m_materialBuffers[frameIndex].mappedData());
    if (dst == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < m_meshes.size(); ++i) {
        dst[i] = m_meshes[i].gpu();
    }
    m_materialUploaded[frameIndex] = m_materialRevision + 1;
}

Model::Model(VulkanContext& context, const SplatData& data, VkDescriptorSetLayout splatSetLayout) {
    // A splat scene: the cloud in place of meshes, the bounds its centres', a static armature
    // (no bones: the identity joint and the model transform).
    m_splats = std::make_unique<SplatCloud>(context, data, splatSetLayout);
    if (m_splats->count() > 0) {
        m_boundsMin = m_splats->boundsMin();
        m_boundsMax = m_splats->boundsMax();
        m_hasBounds = true;
    }
    m_armature.build({});
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
    uploadMaterialsIfDirty(frameIndex); // (the materials ride the same set: binding 3)
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

void Model::recordDepthCutout(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex, bool fine) {
    if (m_transparentMeshes.empty()) {
        return;
    }
    bindPose(cmd, layout, 2, frameIndex);
    const glm::mat4& transform = m_armature.transform();
    const MeshDrawKind kind = fine ? MeshDrawKind::DepthCutoutFine : MeshDrawKind::DepthCutout;
    for (const Mesh* mesh : m_transparentMeshes) {
        mesh->record(cmd, layout, transform, kind, -1, -1); // (no joint highlight in a depth draw)
    }
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
    return boundsThrough(m_armature.transform(), outMin, outMax);
}

bool Model::posedBounds(glm::vec3& outMin, glm::vec3& outMax) const {
    // WHERE THE FIGURE IS: her bind box goes with her HIPS. A figure's place is her root's pose
    // translation (the Ctrl figure move, the Ground button, a hip walk, the Transform tab's
    // Figure group) and her heading its rotation — the model transform of a figure stays the
    // identity — so a bind box taken through the model transform alone stays where she was
    // IMPORTED: moved three metres, her "bounds" spanned both places, and a light added or
    // aimed at her pointed between them, Frame Selected framed the gap. The root's skin
    // transform (poseGlobal · inverseBind) is exactly the identity while the root is at bind,
    // so an unmoved figure's box is worldBounds' to the bit.
    glm::mat4 boxToWorld = m_armature.transform();
    if (m_armature.hasSkeleton()) {
        const int root = m_armature.figureRoot();
        if (root >= 0 && static_cast<std::size_t>(root) < m_armature.boneCount()) {
            const auto r = static_cast<std::size_t>(root);
            boxToWorld = boxToWorld * (m_armature.poseGlobal(r) * m_armature.inverseBind(r));
        }
    }
    return boundsThrough(boxToWorld, outMin, outMax);
}

bool Model::boundsThrough(const glm::mat4& boxToWorld, glm::vec3& outMin, glm::vec3& outMax) const {
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
        const glm::vec3 world = glm::vec3(boxToWorld * glm::vec4(corner, 1.0f));
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
        glm::vec3 boneMin;
        glm::vec3 boneMax;
        jointWorldBox(boneMin, boneMax);
        outMin = glm::min(outMin, boneMin - glm::vec3(kFleshMargin));
        outMax = glm::max(outMax, boneMax + glm::vec3(kFleshMargin));
    }
    return true;
}

void Model::jointWorldBox(glm::vec3& lo, glm::vec3& hi) const {
    lo = glm::vec3(std::numeric_limits<float>::max());
    hi = glm::vec3(std::numeric_limits<float>::lowest());

    // THE BONES THAT DO NOT HANG FROM THE FIGURE'S ROOT GO WITH IT. A figure's place is her
    // root's pose translation (posedBounds), and a real skeleton has bones the root does not
    // carry: the figure node above the hips, standing at the origin, and a merged follower's
    // scene node beside them. They stay where she was IMPORTED when she is moved in her pose,
    // and counted as they stand they kept every box built from the joints spanning both places
    // — a light added after a three-metre move was still aimed half way between the two, and
    // the box a click is tested against still covered the empty spot. They are taken through
    // the root's own motion (its skin transform), as the bind box is. While the root is at its
    // bind place that motion is exactly the identity and every bone is read as it stands.
    const int       root = m_armature.figureRoot();
    const bool      haveRoot = root >= 0 && static_cast<std::size_t>(root) < m_armature.boneCount();
    const glm::mat4 rootMotion = haveRoot ? m_armature.poseGlobal(static_cast<std::size_t>(root)) *
                                                m_armature.inverseBind(static_cast<std::size_t>(root))
                                          : glm::mat4(1.0f);
    const bool      rootMoved = haveRoot && rootMotion != glm::mat4(1.0f);
    const glm::mat4 carried = rootMoved ? m_armature.transform() * rootMotion : glm::mat4(1.0f);
    const auto      hangsFromRoot = [&](std::size_t bone) {
        int node = static_cast<int>(bone);
        for (std::size_t guard = 0; node >= 0 && guard <= m_armature.boneCount(); ++guard) {
            if (node == root) {
                return true;
            }
            node = m_armature.boneParent(static_cast<std::size_t>(node));
        }
        return false;
    };

    for (std::size_t i = 0; i < m_armature.boneCount(); ++i) {
        glm::vec3 p = m_armature.boneWorldPosition(i);
        if (rootMoved && !hangsFromRoot(i)) {
            // (Its place in the model's own space — the pose matrix's translation — carried.)
            p = glm::vec3(carried * glm::vec4(glm::vec3(m_armature.poseGlobal(i)[3]), 1.0f));
        }
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
}

bool Model::eyeInsideSplatBox(const Ray& ray) const {
    glm::vec3 lo, hi;
    return m_splats && worldBounds(lo, hi) && glm::all(glm::greaterThanEqual(ray.origin, lo)) &&
           glm::all(glm::lessThanEqual(ray.origin, hi));
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
        glm::vec3 lo;
        glm::vec3 hi;
        jointWorldBox(lo, hi);
        lo -= glm::vec3(kFleshMargin);
        hi += glm::vec3(kFleshMargin);
        return rayHitsBox(ray.origin, ray.direction, lo, hi, tOut);
    }
    // A splat scene is an environment the camera usually stands INSIDE: from within its box
    // no click is on it (what the user means is the figure in it), so it neither steals the
    // selection nor the context menu.
    if (eyeInsideSplatBox(ray)) {
        return false;
    }
    // Transform the ray into local space so we can slab-test the AABB directly (equivalent to an
    // oriented-box test in world space, but cheaper). The transform is affine, so the ray
    // parameter t is preserved — the t we find is the same world-space distance for every model.
    const glm::mat4 inv = glm::inverse(m_armature.transform());
    const glm::vec3 o = glm::vec3(inv * glm::vec4(ray.origin, 1.0f));
    const glm::vec3 d = glm::vec3(inv * glm::vec4(ray.direction, 0.0f));
    return rayHitsBox(o, d, m_boundsMin, m_boundsMax, tOut);
}

bool Model::groundGap(float& lowestY) const {
    return m_ground.lowestY(m_armature, m_boundsMin, m_boundsMax, m_hasBounds, lowestY);
}

bool Model::pickSurface(const Ray& ray, float& tOut, glm::vec3& hitWorld, int& bone, int* mesh) const {
    return m_ground.pickSurface(m_armature, ray, tOut, hitWorld, bone, mesh);
}

bool Model::pickExact(const Ray& ray, ModelHit& hit) const {
    hit = ModelHit{};
    const float length = glm::length(ray.direction);
    if (length < 1.0e-9f) {
        return false;
    }
    if (m_splats) {
        // (From inside its box a splat scene is the place the user stands in: see intersectRay.)
        if (eyeInsideSplatBox(ray)) {
            return false;
        }
        // The ray in the cloud's own frame; the unit world direction is taken through the inverse
        // transform as it is, so the parameter stays the world's distance.
        const glm::mat4 inverse = glm::inverse(m_armature.transform());
        const glm::vec3 origin = glm::vec3(inverse * glm::vec4(ray.origin, 1.0f));
        const glm::vec3 direction = glm::vec3(inverse * glm::vec4(ray.direction / length, 0.0f));
        float           opacity = 0.0f;
        if (!m_splats->pick(origin, direction, hit.t, opacity)) {
            return false;
        }
        hit.faint = opacity < 0.5f;
        if (!hit.faint) {
            hit.tSolid = hit.t; // (where the opacity reaches a half: the capture is what is seen from there on)
        }
        return true;
    }
    // Which meshes are see-through as their materials stand NOW (a zone made translucent on the
    // Materials tab hides nothing behind it, whatever it was at import).
    std::vector<char> seeThrough(m_meshes.size(), 0);
    for (std::size_t i = 0; i < m_meshes.size(); ++i) {
        seeThrough[i] = m_meshes[i].isTransparent() ? 1 : 0;
    }
    return m_ground.pickAnySurface(m_armature, ray, seeThrough, hit.t, hit.tSolid);
}

// --- THE MATERIALS TAB: the zones' editable materials ---------------------------------------------

void Model::repartitionMeshes() {
    m_opaqueMeshes.clear();
    m_transparentMeshes.clear();
    for (const Mesh& mesh : m_meshes) {
        (mesh.isTransparent() ? m_transparentMeshes : m_opaqueMeshes).push_back(&mesh);
    }
}

std::string Model::zoneKey(std::size_t i) const {
    if (i >= m_meshes.size()) {
        return std::string();
    }
    const std::string& name = m_meshes[i].name();
    return name.empty() ? "#" + std::to_string(i) : name;
}

bool Model::setMeshMaterial(std::size_t i, const MaterialParams& params) {
    if (i >= m_meshes.size()) {
        return false;
    }
    Mesh&      mesh = m_meshes[i];
    const bool wasTransparent = mesh.isTransparent();
    if (!mesh.setMaterial(params)) {
        return false;
    }
    if (mesh.isTransparent() != wasTransparent) {
        repartitionMeshes(); // (the pointers are unchanged; only the lists' membership moves)
    }
    ++m_materialRevision;
    return true;
}

bool Model::setMeshTexture(std::size_t i, MaterialMap map, const std::shared_ptr<const DecodedImage>& plain, int channel,
                           const std::shared_ptr<const DecodedImage>& gpuImage, const std::string& source) {
    if (i >= m_meshes.size()) {
        return false;
    }
    Mesh&      mesh = m_meshes[i];
    const bool wasTransparent = mesh.isTransparent();
    mesh.setSlotImage(map, plain, channel, gpuImage, source, &m_slotUploads);
    if (mesh.isTransparent() != wasTransparent) {
        repartitionMeshes();
    }
    ++m_materialRevision;
    return true;
}

void Model::setMeshMetalBaked(std::size_t i, float scalar) {
    if (i >= m_meshes.size() || m_meshes[i].metalBaked() == scalar) {
        return;
    }
    m_meshes[i].setMetalBaked(scalar);
    ++m_materialRevision;
}

void Model::setMeshNormalFlipBaked(std::size_t i, int flip) {
    if (i < m_meshes.size()) {
        m_meshes[i].setNormalFlipBaked(flip); // (a record: the binding itself changed through setMeshTexture)
    }
}

bool Model::setMeshOverlays(std::size_t i, const std::vector<MaterialOverlay>& overlays) {
    if (i >= m_meshes.size() || !m_meshes[i].setOverlays(overlays)) {
        return false;
    }
    ++m_materialRevision;
    return true;
}

bool Model::smoothingPending() const {
    for (const Mesh& mesh : m_meshes) {
        if (mesh.smoothingPending()) {
            return true;
        }
    }
    return false;
}

bool Model::applySmoothing() {
    if (!smoothingPending()) {
        return false;
    }
    // Every zone that is to be smooth, from its file geometry (kept apart first: the lists below
    // point into the meshes).
    for (Mesh& mesh : m_meshes) {
        if (mesh.material().smoothByAngle) {
            mesh.prepareFileGeometry();
        }
    }
    std::vector<SmoothMesh> zones(m_meshes.size());
    for (std::size_t i = 0; i < m_meshes.size(); ++i) {
        const Mesh& mesh = m_meshes[i];
        zones[i].vertices = &mesh.fileVertices();
        zones[i].indices = &mesh.fileIndices();
        zones[i].smooth = mesh.material().smoothByAngle;
        zones[i].angleDeg = mesh.material().smoothAngle;
    }
    std::vector<SmoothedMesh> smoothed = smoothByAngle(zones);
    static_assert(sizeof(Vertex) == 88, "Vertex is compared bytewise here: no padding");
    bool changed = false;
    for (std::size_t i = 0; i < m_meshes.size(); ++i) {
        Mesh&       mesh = m_meshes[i];
        const float angle = mesh.material().smoothAngle;
        if (!mesh.material().smoothByAngle) {
            if (mesh.smoothingApplied()) {
                mesh.restoreFileGeometry();
                changed = true;
            }
            continue;
        }
        SmoothedMesh& made = smoothed[i];
        if (made.indices.size() != mesh.fileIndices().size()) {
            mesh.markSmoothed(angle); // (nothing to smooth: a mesh with no triangle)
            continue;
        }
        computeTangents(made.vertices, made.indices);
        const bool same = made.indices == mesh.cpuIndices() && made.vertices.size() == mesh.cpuVertices().size() &&
                          std::memcmp(made.vertices.data(), mesh.cpuVertices().data(), made.vertices.size() * sizeof(Vertex)) == 0;
        if (same) {
            mesh.markSmoothed(angle);
        } else {
            mesh.setSmoothedGeometry(std::move(made.vertices), std::move(made.indices), angle);
            changed = true;
        }
    }
    if (changed) {
        ++m_materialRevision;
    }
    return changed;
}

std::vector<int> Model::overlayZones(std::size_t i) const {
    std::vector<int> zones;
    if (i >= m_meshes.size()) {
        return zones;
    }
    // The zones painted on mesh i's color map: those IMPORTED with the same base-color image
    // (the decode shares one image between the zones of one map, so identity is the test; the
    // imported image, so a map replaced on the tab does not break the group apart).
    const std::shared_ptr<const DecodedImage> map = m_meshes[i].slotImage(MaterialMap::BaseColor, /*imported=*/true);
    for (std::size_t k = 0; k < m_meshes.size(); ++k) {
        if (k == i || (map && m_meshes[k].slotImage(MaterialMap::BaseColor, /*imported=*/true) == map)) {
            zones.push_back(static_cast<int>(k));
        }
    }
    return zones;
}

std::vector<MaterialParams> Model::captureMaterials() const {
    std::vector<MaterialParams> out;
    out.reserve(m_meshes.size());
    for (const Mesh& mesh : m_meshes) {
        out.push_back(mesh.material());
    }
    return out;
}

void Model::applyMaterials(const std::vector<MaterialParams>& materials) {
    const std::size_t n = std::min(materials.size(), m_meshes.size());
    for (std::size_t i = 0; i < n; ++i) {
        setMeshMaterial(i, materials[i]);
    }
}

std::vector<Model::MaterialOverride> Model::materialOverrides() const {
    std::vector<MaterialOverride> out;
    for (std::size_t i = 0; i < m_meshes.size(); ++i) {
        const Mesh& mesh = m_meshes[i];
        if (mesh.material() == mesh.importedMaterial()) {
            continue;
        }
        MaterialOverride o;
        o.zone = zoneKey(i);
        o.params = mesh.material();
        for (int m = 0; m < kMaterialMapCount; ++m) {
            const std::string& now = mesh.material().maps[static_cast<std::size_t>(m)].source;
            if (now != mesh.importedMaterial().maps[static_cast<std::size_t>(m)].source) {
                o.textures.emplace_back(materialMapName(static_cast<MaterialMap>(m)), now);
            }
        }
        out.push_back(std::move(o));
    }
    return out;
}

bool Model::applyMaterialOverride(const std::string& zone, const MaterialParams& params, const std::vector<std::string>* named) {
    bool any = false;
    for (std::size_t i = 0; i < m_meshes.size(); ++i) {
        if (zoneKey(i) == zone) {
            // The values the file named, over the zone's own (materialparams.h): what the file
            // says nothing of — a value newer than the file — stays the zone's.
            const MaterialParams next = named != nullptr ? materialWithNamedValues(m_meshes[i].material(), params, *named) : params;
            any = setMeshMaterial(i, next) || any;
        }
    }
    return any;
}

} // namespace pose

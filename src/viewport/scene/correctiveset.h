/**
 * @file correctiveset.h
 * @brief A model's pose correctives (joint-driven corrective morphs) as the GPU blends
 *        them: the resolved driver formulas, the static per-vertex delta table, and the per-frame
 *        weight buffers.
 *
 * Correctives fix the mesh crumpling ("candy-wrapper") at deep elbow/knee/shoulder/hip bends.
 * Each one is a sparse displacement field over the base cage plus a driver formula in joint-angle
 * terms. They are blended ON THE GPU, live: at construction every corrective's base-vertex deltas
 * are mapped through each mesh's baseVertex array onto the render vertices (a seam vertex feeds
 * several) into ONE static device-local delta table per model (set 2 binding 2), and each touched
 * vertex carries its packed run (Vertex::correctiveRange, stamped into the upload copy). The
 * per-frame WEIGHTS ride a host-mapped storage buffer PER FRAME IN FLIGHT (set 2 binding 1, the
 * joint-buffer scheme) — re-evaluated at record time whenever the pose moved (Armature::skinVersion)
 * and copied into the current frame slot when it is behind the weight version. A weight change
 * costs a few hundred bytes per frame, never a geometry re-upload, so the correctives track a drag
 * continuously. (This replaced a CPU re-morph that could only afford to run once a drag ENDED —
 * the figure visibly deformed uncorrected through every drag and snapped right at mouse-up.)
 * A model without correctives keeps 16-byte zero placeholders bound so its set 2 stays complete.
 * Qt-free (Vulkan + std + GLM).
 */

#ifndef CORRECTIVESET_H
#define CORRECTIVESET_H

#include "modeldata.h"    // CorrectiveFormula (the driver-formula IR the importer produced)
#include "vulkanbuffer.h"
#include "vulkancommon.h" // kMaxFramesInFlight

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pose {

class Armature;
class ImmediateBatch;
class VulkanContext;

/// One (corrective, displacement) entry of the GPU delta buffer — the vertex shaders'
/// CorrectiveDelta (std430: uint + 3 floats, 16 bytes). Entries are grouped per render
/// vertex; Vertex::correctiveRange addresses a vertex's run.
struct CorrectiveEntry {
    uint32_t  corrective; ///< Index into the set's correctives / the weights buffer.
    glm::vec3 delta;      ///< Bind-space displacement at weight 1 (model units).
};
static_assert(sizeof(CorrectiveEntry) == 16, "must match the shaders' CorrectiveDelta");

/// A SOFT-BODY DELTA FIELD (armaturebuttocks.cpp), blended as a corrective is: per non-empty mesh
/// (the Model's order) the render vertices it touches with their bind-space deltas at weight 1,
/// and its weight each frame is Armature::softBodyWeight(softIndex) — a displacement in metres —
/// rather than a driver formula of the pose.
struct SoftBodyField {
    std::string                                                    id;
    int                                                            softIndex = 0;
    std::vector<std::vector<std::pair<uint32_t, glm::vec3>>>       perMesh;
};

class CorrectiveSet {
public:
    /// Resolves the model's correctives for the GPU: keeps each one's driver formulas (bone
    /// names pre-resolved to indices) and maps every base-vertex delta through each mesh's
    /// baseVertex array onto the render vertices (a seam vertex feeds several), producing the
    /// flat, per-vertex-grouped @p entries and, per non-empty mesh, the packed range each of its
    /// vertices carries (@p perMeshRanges — parallel to the meshes the Model builds next; an
    /// untouched mesh's vector stays empty). Runs once at construction, BEFORE the meshes
    /// upload, because the ranges ride in the vertex data.
    void build(const ModelData& data, std::vector<std::vector<uint32_t>>& perMeshRanges,
               std::vector<CorrectiveEntry>& entries, const std::vector<SoftBodyField>& softFields = {});

    /// Creates the GPU side: the static delta table from @p entries (device-local, riding
    /// @p batch — or a 16-byte zero placeholder when empty) and the per-frame-slot weight
    /// buffers (one float per corrective, 16-byte minimum, zero-filled — a fresh model renders
    /// uncorrected until its first evaluation at record time). Always call it, correctives or
    /// not: the placeholders keep the model's set 2 complete.
    void createBuffers(VulkanContext& context, const std::vector<CorrectiveEntry>& entries,
                       ImmediateBatch& batch);

    bool empty() const { return m_correctives.empty(); }
    /// THE BLEND ON THE CPU (Model::bakePosedMesh — a render engine's geometry): the delta table
    /// as the vertex stage reads it (Vertex::correctiveRange addresses a vertex's run) and the
    /// weights as last evaluated (refresh() first for the pose as it stands).
    const std::vector<CorrectiveEntry>& entries() const { return m_entries; }
    const std::vector<float>&           weights() const { return m_weight; }
    /// The delta table (set 2 binding 2) and frame slot @p frameIndex's weight buffer (binding 1).
    VkBuffer deltaBuffer() const { return m_deltaBuffer.handle(); }
    VkBuffer weightBuffer(uint32_t frameIndex) const { return m_weightBuffers[frameIndex].handle(); }

    /// Writes the current weights into frame slot @p frameIndex's buffer if that slot hasn't
    /// seen the current weight version — re-evaluating them first when @p armature's pose moved
    /// since the last evaluation (Armature::skinVersion is the pose's change counter). No-op
    /// without correctives.
    void uploadIfDirty(uint32_t frameIndex, const Armature& armature);

    /// Re-evaluates the weights against @p armature's current pose now (the GPU blend picks
    /// them up at the next recorded frame) and, with POSESTUDIO_DUMP_CORRECTIVES set, prints the
    /// active ones. No-op without correctives. The correctives no longer NEED this to show:
    /// every recorded frame re-evaluates them from the live pose (uploadIfDirty), so they track
    /// a drag continuously — this is the explicit "pose settled" hook (Scene::finalizePose,
    /// Model::setBoneRotation/applyPose): cheap, and where the dump lives.
    void refresh(const Armature& armature);

private:
    // One pose corrective resolved for this model: its driver formulas in joint-angle terms (the
    // deltas themselves live in the GPU delta buffer). opBone[f][k] is the pre-resolved bone
    // index of sumFormulas[f].ops[k] (a PushRotation); -1 for other ops / unknown bones.
    struct RuntimeCorrective {
        std::string                    id; // corrective morph id (diagnostics)
        std::vector<CorrectiveFormula> sumFormulas;
        std::vector<std::vector<int>>  opBone;
        float                          gateScale = 1.0f;
        bool                           clamped = false; // clamp the driven weight to [min,max]
        float                          clampMin = 0.0f;
        float                          clampMax = 1.0f;
        int                            softIndex = -1; ///< A soft-body field's weight index (Armature::softBodyWeight); -1 for a pose corrective.
    };

    /// Evaluates one corrective's blend weight from the pose (Σ sumFormulas × gateScale).
    float evalWeight(std::size_t correctiveIndex, const Armature& armature) const;
    /// Re-evaluates every corrective's weight from the pose; returns whether any moved (past a
    /// 1e-4 threshold) and, if so, bumps m_version so the frame slots re-upload.
    bool evaluateWeights(const Armature& armature);

    std::vector<RuntimeCorrective>                m_correctives;
    std::vector<float>                            m_weight; // current weight per corrective
    std::vector<CorrectiveEntry>                  m_entries; // the delta table's CPU copy (see entries())
    VulkanBuffer                                  m_deltaBuffer;
    std::array<VulkanBuffer, kMaxFramesInFlight>  m_weightBuffers;
    std::array<std::uint64_t, kMaxFramesInFlight> m_uploaded{}; // weight version per slot
    std::uint64_t                                 m_version = 1;
    std::uint64_t                                 m_evalSkinVersion = ~std::uint64_t{0};
};

} // namespace pose

#endif // CORRECTIVESET_H

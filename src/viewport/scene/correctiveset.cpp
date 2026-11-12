/**
 * @file correctiveset.cpp
 * @brief Corrective resolution, weight evaluation, and the per-frame weight upload. See
 *        correctiveset.h.
 */

#include "correctiveset.h"

#include "armature.h"
#include "vulkancommands.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace pose {

namespace {

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

void CorrectiveSet::build(const ModelData& data, std::vector<std::vector<uint32_t>>& perMeshRanges,
                          std::vector<CorrectiveEntry>& entries, const std::vector<SoftBodyField>& softFields) {
    // Bone names -> indices, resolved once (the armature is built from data.bones in this same
    // order, so these ARE the runtime bone indices the pose is read by).
    std::unordered_map<std::string, int> boneOf;
    boneOf.reserve(data.bones.size());
    for (std::size_t i = 0; i < data.bones.size(); ++i) {
        boneOf.emplace(data.bones[i].name, static_cast<int>(i));
    }

    // The non-empty meshes, in the order the Model builds them (= its mesh order).
    std::vector<const MeshData*> meshes;
    meshes.reserve(data.meshes.size());
    uint32_t baseCount = 0;
    for (const MeshData& md : data.meshes) {
        if (md.indices.empty()) {
            continue;
        }
        meshes.push_back(&md);
        for (const uint32_t b : md.baseVertex) {
            baseCount = std::max(baseCount, b + 1);
        }
    }
    const std::size_t meshCount = meshes.size();
    perMeshRanges.assign(meshCount, {});
    entries.clear();

    // Invert each mesh's baseVertex array: base vertex -> the render (mesh, local) vertices it
    // fed. A base vertex on a UV/zone seam feeds several render vertices (possibly across
    // meshes), so a corrective's single delta must reach all of them. Dense CSR over the (small)
    // base cage: one counting pass, one fill pass.
    std::vector<uint32_t> fanStart(static_cast<std::size_t>(baseCount) + 1, 0);
    for (const MeshData* md : meshes) {
        for (const uint32_t b : md->baseVertex) {
            ++fanStart[b + 1];
        }
    }
    for (std::size_t i = 1; i < fanStart.size(); ++i) {
        fanStart[i] += fanStart[i - 1];
    }
    std::vector<std::pair<uint32_t, uint32_t>> fan(fanStart.back()); // (mesh, local vertex)
    {
        std::vector<uint32_t> cursor(fanStart.begin(), fanStart.end() - 1);
        for (uint32_t k = 0; k < meshCount; ++k) {
            const std::vector<uint32_t>& bidx = meshes[k]->baseVertex;
            for (uint32_t lv = 0; lv < bidx.size(); ++lv) {
                fan[cursor[bidx[lv]]++] = {k, lv};
            }
        }
    }

    // Pass 1: how many correctives touch each render vertex, and which correctives land on any
    // rendered geometry at all (those are the ones kept — the GPU index is the compacted one).
    std::vector<std::vector<uint32_t>> count(meshCount);
    for (uint32_t k = 0; k < meshCount; ++k) {
        count[k].assign(meshes[k]->vertices.size(), 0);
    }
    std::vector<int> compactIndex(data.correctives.size(), -1);
    std::size_t total = 0;
    for (std::size_t ci = 0; ci < data.correctives.size(); ++ci) {
        bool landed = false;
        for (const auto& [baseIdx, delta] : data.correctives[ci].deltas) {
            if (baseIdx >= baseCount) {
                continue; // targets a vertex not present in any rendered mesh
            }
            for (uint32_t f = fanStart[baseIdx]; f < fanStart[baseIdx + 1]; ++f) {
                const auto [k, lv] = fan[f];
                ++count[k][lv];
                ++total;
                landed = true;
            }
        }
        if (landed) {
            compactIndex[ci] = static_cast<int>(m_correctives.size());
            RuntimeCorrective rc;
            const PoseCorrective& pc = data.correctives[ci];
            rc.id = pc.id;
            rc.sumFormulas = pc.sumFormulas;
            rc.gateScale = pc.gateScale;
            rc.clamped = pc.clamped;
            rc.clampMin = pc.clampMin;
            rc.clampMax = pc.clampMax;
            rc.opBone.resize(rc.sumFormulas.size());
            for (std::size_t f = 0; f < rc.sumFormulas.size(); ++f) {
                const std::vector<CorrectiveOp>& ops = rc.sumFormulas[f].ops;
                rc.opBone[f].assign(ops.size(), -1);
                for (std::size_t k = 0; k < ops.size(); ++k) {
                    if (ops[k].kind == CorrectiveOp::Kind::PushRotation) {
                        const auto it = boneOf.find(ops[k].bone);
                        rc.opBone[f][k] = it == boneOf.end() ? -1 : it->second;
                    }
                }
            }
            m_correctives.push_back(std::move(rc));
        }
    }
    // The SOFT-BODY FIELDS join the table as correctives of their own, after the pose
    // correctives (their deltas are per render vertex already; their weights come from the
    // armature's soft body, not the pose).
    std::vector<int> softCompact(softFields.size(), -1);
    for (std::size_t si = 0; si < softFields.size(); ++si) {
        const SoftBodyField& sf = softFields[si];
        bool                 landed = false;
        for (uint32_t k = 0; k < meshCount && k < sf.perMesh.size(); ++k) {
            for (const auto& [lv, delta] : sf.perMesh[k]) {
                if (lv < count[k].size()) {
                    ++count[k][lv];
                    ++total;
                    landed = true;
                }
            }
        }
        if (landed) {
            softCompact[si] = static_cast<int>(m_correctives.size());
            RuntimeCorrective rc;
            rc.id = sf.id;
            rc.softIndex = sf.softIndex;
            m_correctives.push_back(std::move(rc));
        }
    }
    m_weight.assign(m_correctives.size(), 0.0f);
    if (m_correctives.empty()) {
        return;
    }

    // The packed range is (first entry << 8) | count: 24 bits of entry offset, 8 of count. Cap
    // the total at the guaranteed storage-buffer range (2^23 entries × 16 B = 128 MB) — a real
    // figure lands around a million — and a vertex's run at 255 (its later correctives are
    // dropped; no authored content approaches this).
    constexpr std::size_t kMaxEntries = std::size_t{1} << 23;
    constexpr uint32_t    kMaxPerVertex = 255;
    if (total > kMaxEntries) {
        std::fprintf(stderr,
                     "[correctives] %zu delta entries exceed the GPU table limit (%zu); "
                     "pose correctives disabled for this model\n",
                     total, kMaxEntries);
        m_correctives.clear();
        m_weight.clear();
        perMeshRanges.assign(meshCount, {});
        return;
    }

    // Prefix the runs into the flat table and stamp each touched vertex's packed range.
    std::vector<std::vector<uint32_t>> start(meshCount);
    std::size_t running = 0;
    bool capped = false;
    for (uint32_t k = 0; k < meshCount; ++k) {
        const std::vector<uint32_t>& cnt = count[k];
        start[k].assign(cnt.size(), 0);
        bool touched = false;
        for (std::size_t lv = 0; lv < cnt.size(); ++lv) {
            if (cnt[lv] == 0) {
                continue;
            }
            touched = true;
            const uint32_t n = std::min(cnt[lv], kMaxPerVertex);
            capped = capped || n != cnt[lv];
            start[k][lv] = static_cast<uint32_t>(running);
            running += n;
        }
        if (touched) {
            perMeshRanges[k].assign(cnt.size(), 0);
            for (std::size_t lv = 0; lv < cnt.size(); ++lv) {
                if (cnt[lv] != 0) {
                    perMeshRanges[k][lv] = (start[k][lv] << 8) | std::min(cnt[lv], kMaxPerVertex);
                }
            }
        }
    }
    if (capped) {
        std::fprintf(stderr, "[correctives] a vertex is touched by more than %u correctives; "
                             "the extra ones are dropped for it\n", kMaxPerVertex);
    }

    // Pass 2: fill the table, each vertex's run in corrective order.
    entries.resize(running);
    std::vector<std::vector<uint32_t>> filled(meshCount);
    for (uint32_t k = 0; k < meshCount; ++k) {
        filled[k].assign(count[k].size(), 0);
    }
    for (std::size_t ci = 0; ci < data.correctives.size(); ++ci) {
        if (compactIndex[ci] < 0) {
            continue;
        }
        const auto gpuIndex = static_cast<uint32_t>(compactIndex[ci]);
        for (const auto& [baseIdx, delta] : data.correctives[ci].deltas) {
            if (baseIdx >= baseCount) {
                continue;
            }
            for (uint32_t f = fanStart[baseIdx]; f < fanStart[baseIdx + 1]; ++f) {
                const auto [k, lv] = fan[f];
                if (filled[k][lv] >= kMaxPerVertex) {
                    continue;
                }
                entries[start[k][lv] + filled[k][lv]++] = {gpuIndex, delta};
            }
        }
    }
    for (std::size_t si = 0; si < softFields.size(); ++si) {
        if (softCompact[si] < 0) {
            continue;
        }
        const auto gpuIndex = static_cast<uint32_t>(softCompact[si]);
        for (uint32_t k = 0; k < meshCount && k < softFields[si].perMesh.size(); ++k) {
            for (const auto& [lv, delta] : softFields[si].perMesh[k]) {
                if (lv >= count[k].size() || filled[k][lv] >= kMaxPerVertex) {
                    continue;
                }
                entries[start[k][lv] + filled[k][lv]++] = {gpuIndex, delta};
            }
        }
    }
    if (std::getenv("POSESTUDIO_DUMP_CORRECTIVES") != nullptr) {
        std::fprintf(stderr, "[correctives] %zu correctives -> %zu GPU delta entries (%.1f MB)\n",
                     m_correctives.size(), entries.size(),
                     static_cast<double>(entries.size() * sizeof(CorrectiveEntry)) / (1024.0 * 1024.0));
    }
}

void CorrectiveSet::createBuffers(VulkanContext& context,
                                  const std::vector<CorrectiveEntry>& entries,
                                  ImmediateBatch& batch) {
    // The corrective delta table: one static device-local buffer for the whole model (the vertex
    // shaders index it through each vertex's range), riding the same upload batch as the meshes;
    // a 16-byte zero placeholder keeps set 2 complete for a model without correctives.
    m_entries = entries; // (kept: the CPU blend of a render engine's geometry reads the table)
    if (!entries.empty()) {
        m_deltaBuffer = createDeviceLocalBuffer(context, entries.data(),
                                                sizeof(CorrectiveEntry) * entries.size(),
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, batch);
    } else {
        m_deltaBuffer = makeZeroedHostStorageBuffer(context, sizeof(CorrectiveEntry));
    }
    // The weight buffers ride the per-frame-slot scheme of the joint buffers: one float per
    // corrective (16-byte minimum, zero-filled — a fresh model renders uncorrected until its
    // first evaluation at record time), host-mapped and rewritten in place by uploadIfDirty.
    const VkDeviceSize weightBytes = std::max<VkDeviceSize>(16, sizeof(float) * m_correctives.size());
    for (int f = 0; f < kMaxFramesInFlight; ++f) {
        m_weightBuffers[static_cast<std::size_t>(f)] = makeZeroedHostStorageBuffer(context, weightBytes);
    }
}

float CorrectiveSet::evalWeight(std::size_t correctiveIndex, const Armature& armature) const {
    if (m_correctives[correctiveIndex].softIndex >= 0) {
        return armature.softBodyWeight(m_correctives[correctiveIndex].softIndex); // (a soft-body field: the mass's displacement)
    }
    const RuntimeCorrective& rc = m_correctives[correctiveIndex];
    float sum = 0.0f;
    // A fixed-capacity stack: the authored formulas are push/push/spline/mult shapes of depth
    // <= 4, and this runs for every corrective on every pose change (each 60 Hz drag tick), so a
    // heap-allocated stack would cost ~100 mallocs per tick. Pushes past the capacity are dropped
    // (a malformed formula degrades, never overruns).
    constexpr std::size_t kStackCapacity = 16;
    std::array<float, kStackCapacity> st{};
    for (std::size_t fi = 0; fi < rc.sumFormulas.size(); ++fi) {
        const CorrectiveFormula& f = rc.sumFormulas[fi];
        std::size_t depth = 0;
        const auto push = [&](float v) {
            if (depth < kStackCapacity) {
                st[depth++] = v;
            }
        };
        for (std::size_t oi = 0; oi < f.ops.size(); ++oi) {
            const CorrectiveOp& op = f.ops[oi];
            switch (op.kind) {
                case CorrectiveOp::Kind::PushRotation: {
                    float angle = 0.0f;
                    const int bone = rc.opBone[fi][oi];
                    if (bone >= 0) {
                        angle = armature.boneEuler(static_cast<std::size_t>(bone))[op.axis];
                    }
                    push(angle);
                    break;
                }
                case CorrectiveOp::Kind::PushConst:
                    push(op.value);
                    break;
                case CorrectiveOp::Kind::Spline: {
                    const float d = depth == 0 ? 0.0f : st[depth - 1];
                    if (depth > 0) {
                        --depth;
                    }
                    push(evalSpline(op.knots, d));
                    break;
                }
                case CorrectiveOp::Kind::Mult:
                    if (depth >= 2) {
                        const float b = st[depth - 1];
                        --depth;
                        st[depth - 1] *= b;
                    }
                    break;
                case CorrectiveOp::Kind::Add:
                    if (depth >= 2) {
                        const float b = st[depth - 1];
                        --depth;
                        st[depth - 1] += b;
                    }
                    break;
            }
        }
        sum += depth == 0 ? 0.0f : st[depth - 1];
    }
    const float w = sum * rc.gateScale;
    // The channel clamp is part of the authored driver (see PoseCorrective::clamped): linear ramp
    // drivers rely on it to switch off outside their intended range — a knee-EXTENSION flexion
    // (rotation/x × -1/11) must stay 0 through the 155° of flexion, not run to -14.
    return rc.clamped ? glm::clamp(w, rc.clampMin, rc.clampMax) : w;
}

bool CorrectiveSet::evaluateWeights(const Armature& armature) {
    bool changed = false;
    for (std::size_t i = 0; i < m_correctives.size(); ++i) {
        const float w = evalWeight(i, armature);
        // Only commit a move past the threshold, so the stored weight is always exactly what the
        // GPU has (sub-threshold drift accumulates until it trips, never silently diverges).
        if (std::fabs(w - m_weight[i]) > 1e-4f) {
            m_weight[i] = w;
            changed = true;
        }
    }
    if (changed) {
        ++m_version;
    }
    return changed;
}

void CorrectiveSet::uploadIfDirty(uint32_t frameIndex, const Armature& armature) {
    if (m_correctives.empty() || frameIndex >= static_cast<uint32_t>(kMaxFramesInFlight)) {
        return;
    }
    // The pose moved since the weights were last evaluated (any posing path — a drag tick, a
    // dial turn, an IK settle, a pose load — bumps the armature's skin version).
    if (m_evalSkinVersion != armature.skinVersion()) {
        evaluateWeights(armature);
        m_evalSkinVersion = armature.skinVersion();
    }
    if (m_uploaded[frameIndex] == m_version) {
        return;
    }
    auto* dst = static_cast<float*>(m_weightBuffers[frameIndex].mappedData());
    if (dst == nullptr) {
        return;
    }
    std::memcpy(dst, m_weight.data(), sizeof(float) * m_weight.size());
    m_uploaded[frameIndex] = m_version;
}

void CorrectiveSet::refresh(const Armature& armature) {
    if (m_correctives.empty()) {
        return;
    }
    evaluateWeights(armature);
    m_evalSkinVersion = armature.skinVersion();
    // Diagnostic hook: POSESTUDIO_DUMP_CORRECTIVES=1 prints every corrective whose weight is
    // non-zero when a pose settles — which correctives fire, and how hard. No-op unless set.
    static const bool dump = std::getenv("POSESTUDIO_DUMP_CORRECTIVES") != nullptr;
    if (dump) {
        std::fprintf(stderr, "[correctives] active after pose change:\n");
        for (std::size_t i = 0; i < m_correctives.size(); ++i) {
            if (std::fabs(m_weight[i]) >= 1e-4f) {
                std::fprintf(stderr, "  %-48s w=%.3f\n", m_correctives[i].id.c_str(), m_weight[i]);
            }
        }
        std::fflush(stderr);
    }
}

} // namespace pose

/**
 * @file ikrigvolumes.cpp
 * @brief IkRig's SELF-COLLISION BODY VOLUMES (bodyvolume.h): the tapered capsules fitted to the
 *        figure's own mesh at build (buildBodyVolumes — the torso as ONE banded profile along the
 *        spine, the limbs by their median radial distance, the legs by their inner surface, the
 *        head from the skull; from the skeleton's hip spacing without a mesh), the drag-start mask
 *        of pairs already overlapping (maskOverlappingVolumes) and the drag target's clamp out of
 *        them and the floor (clampOutOfVolumes). See ikrig.cpp for the TU layout. Qt-free.
 */

#include "ikrig.h"
#include "ikmath.h"

#include "ikrig_constants.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pose {

namespace {

/// IK_VOLUME_TRACE: every capsule as built, and the rest-overlap exemptions.
const bool kTrace = std::getenv("IK_VOLUME_TRACE") != nullptr;
/// IK_VOLUME_NO_MESH: the A/B probe — the volumes sized from the skeleton, the mesh sample unread.
const bool kNoMesh = std::getenv("IK_VOLUME_NO_MESH") != nullptr;
/// IK_VOLUME_NO_RIDERS: the A/B probe — a hand's riders tested by no volume.
const bool kNoRiders = std::getenv("IK_VOLUME_NO_RIDERS") != nullptr;
/// IK_VOLUME_TORSO_NO_SEGMENTS: the A/B probe — the fitted torso tests the upper arm's joints alone.
const bool kTorsoNoSegments = std::getenv("IK_VOLUME_TORSO_NO_SEGMENTS") != nullptr;
/// IK_VOLUME_NO_LIMBS: the A/B probe — no limb-against-limb capsules (the shins, the arms).
const bool kNoLimbVolumes = std::getenv("IK_VOLUME_NO_LIMBS") != nullptr;

// THE MESH FIT. With a body mesh sample (bodymesh.h) each capsule is sized from the vertices
// that follow its bones: a limb is a TAPERED capsule (the median radial distance regressed to
// its ends: the thigh's hip flesh and its knee), the torso a stack of per-bone segments, each
// covered by a centre capsule and two side capsules whose axes run ahead of and beside the spine
// (the section's front/back extent gives the depth and the forward offset — the breasts; its
// width the sides), the head a sphere-ended capsule reaching the crown. Without a sample the
// volumes are sized from the hip-socket half-spacing as before (and IK_VOLUME_NO_MESH=1 forces
// that, the A/B probe).
constexpr int   kBands = 4;          // bands along the axis for the taper regression
constexpr int   kMinBandPoints = 24; // a band with fewer points says nothing
constexpr float kLimbPct = 0.5f;     // a limb's radius: the median radial distance
constexpr float kInnerPct = 0.8f;    // a leg's inner surface: the extent toward the other leg
constexpr float kSidePct = 0.85f;    // a torso section's half-width
constexpr float kFrontPct = 0.9f;    // ... its front
constexpr float kBackPct = 0.1f;     // ... and its back
constexpr float kHeadPct = 0.75f;    // the skull's radius
constexpr float kFitLo = -0.1f;      // the axis window a member point must fall in
constexpr float kFitHi = 1.1f;

/// The percentile @p p of @p v (reordered).
float pct(std::vector<float>& v, float p) {
    if (v.empty()) {
        return 0.0f;
    }
    std::size_t k = static_cast<std::size_t>(p * static_cast<float>(v.size() - 1) + 0.5f);
    k = std::min(k, v.size() - 1);
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

} // namespace

/**
 * @brief The body-volume fit's working state and its parts, run in order by run(): the body's
 *        parts by their roles, the torso, the head, the thighs, the limb pairs, then the segment
 *        radii and the rest-overlap exemption. A nested type of IkRig (declared private in the
 *        header), so it reads and writes the rig directly; defined here alone. Written as one
 *        760-line function until the IK refactoring round of 2026-11-03; the parts are its
 *        blocks verbatim.
 */
struct IkRig::VolumeFitter {
    IkRig&                            rig;
    const std::vector<std::string>&   names;
    const std::vector<BodyMeshPoint>* mesh;
    const std::size_t                 n;
    const RigRoles&                   roles;
    const bool                        fitted; ///< A body mesh sample to size the capsules from.

    // The body's parts by their ROLES (rigroles.h; until 2026-11-02 by tolerant name fragments),
    // found in prepare(): per side (0 = left, 1 = right throughout) the limb roots — the upper
    // arms and the thighs — and each limb's joints.
    int armRoot[2] = {-1, -1};
    int thighRoot[2] = {-1, -1};
    int knee[2] = {-1, -1};
    int ankle[2] = {-1, -1};
    int elbow[2] = {-1, -1};
    int hand[2] = {-1, -1};
    int upperArm[2] = {-1, -1};
    int neckBase = -1;
    int headNode = -1;
    /// Limb membership: 0/1 = the arms, 2/3 = the legs (the root included; excluded from the tested set).
    std::vector<int> limbOf;
    /// RIDERS of a hand or a foot — the fingers, thumbs, carpals, toes, metatarsals (see prepare()).
    std::vector<char> rider;
    /// Half the hip sockets' spacing: what the skeleton-sized capsules are measured in.
    float halfSpacing = 0.0f;
    /// The limbs the torso and the head keep out: the arms (the legs answer to no torso volume).
    const std::vector<int> arms{0, 1};

    VolumeFitter(IkRig& r, const std::vector<std::string>& boneNames, const std::vector<BodyMeshPoint>* sample)
        : rig(r),
          names(boneNames),
          mesh(sample),
          n(r.m_bindPos.size()),
          roles(r.m_roles),
          fitted(sample != nullptr && !sample->empty() && !kNoMesh) {}

    /// Every part in order.
    void run() {
        prepare();
        torso();
        head();
        thighs();
        limbPairs();
        segmentRadii();
        segmentRestExemption();
    }

    // ---- The pieces every part shares.

    bool isHelperBone(std::size_t i) const { return roles.role(static_cast<int>(i)) == RigRole::Helper; }
    glm::vec3 bindOf(int i) const { return rig.m_bindPos[static_cast<std::size_t>(i)]; }

    /// A per-band statistic regressed to the axis ends: a straight line through the band centres,
    /// read at t = 0 and t = 1. ok = false if no band had enough points.
    struct EndPair {
        float a = 0.0f;
        float b = 0.0f;
        bool  ok = false;
    };
    /// The points of a member set along an axis, binned into bands by their parameter t: the
    /// radial distance, and — with a frame — the lateral (|u|) and forward (w) components of the
    /// radial vector.
    struct Bands {
        std::vector<std::vector<float>> r, u, w;
        std::vector<float>              axial; // t * L of every member point (the head's crown)
        int                             total = 0;
    };
    /// A capsule to add: its ends, its radii and extensions, its offsets and lateral references,
    /// which limbs it keeps out and how (see addVolume()).
    struct VolumeSpec {
        int               a = -1;
        int               b = -1;
        float             radius = 0.0f;
        float             radiusB = -1.0f;
        float             extendA = 0.0f;
        float             extendB = 0.0f;
        glm::vec3         offA{0.0f};
        glm::vec3         offB{0.0f};
        int               refL = -1;
        int               refR = -1;
        std::vector<int>  limbs;
        bool              segments = false;
        bool              riders = false;
        std::vector<char> segmentNodes; // per node: its segment is tested even without `segments` (kVolumeAppliesJointSegment)
        const char*       what = "";
    };

    /// The member points of a capsule from a to b: the bones on the chain from b (exclusive,
    /// unless includeB) up to a (inclusive) and their RIDERS — off-chain descendants whose
    /// subtree is token (the pectorals on the chest, the newest generation's leaf twist
    /// helpers on the thigh, the face on the head), never a real limb or the next chain node;
    /// the root segment also takes the PELVIS bone (the buttocks), a real-mass off-chain child
    /// of the hip that is not a limb.
    std::vector<char> chainMembers(int a, int b, bool includeB) const {
        std::vector<char> member(n, 0);
        std::vector<int>  chain;
        bool              reached = false;
        for (int cur = b; cur >= 0; cur = rig.m_parents[static_cast<std::size_t>(cur)]) {
            if (cur != b || includeB) {
                chain.push_back(cur);
            }
            if (cur == a) {
                reached = true;
                break;
            }
        }
        if (!reached) {
            return member;
        }
        std::vector<char> onChain(n, 0);
        for (const int c : chain) {
            onChain[static_cast<std::size_t>(c)] = 1;
        }
        onChain[static_cast<std::size_t>(b)] = 1;
        for (const int c : chain) {
            member[static_cast<std::size_t>(c)] = 1;
            std::vector<int> stack(rig.m_children[static_cast<std::size_t>(c)].begin(),
                                   rig.m_children[static_cast<std::size_t>(c)].end());
            while (!stack.empty()) {
                const int d = stack.back();
                stack.pop_back();
                const std::size_t di = static_cast<std::size_t>(d);
                if (onChain[di] || !rig.m_bodyNode[di]) {
                    continue;
                }
                const bool token = rig.m_subtreeMass[di] < kTokenLimbMass;
                const bool pelvis = c == rig.m_pelvis && limbOf[di] < 0 && rig.m_parents[di] == rig.m_pelvis;
                if (!token && !pelvis) {
                    continue;
                }
                member[di] = 1;
                stack.insert(stack.end(), rig.m_children[di].begin(), rig.m_children[di].end());
            }
        }
        return member;
    }
    /// The percentile @p p of each band regressed to the axis ends (see EndPair).
    static EndPair regress(std::vector<std::vector<float>>& bands, float p) {
        EndPair out;
        float   sx = 0.0f, sy = 0.0f, sxx = 0.0f, sxy = 0.0f;
        int     count = 0;
        for (int k = 0; k < kBands; ++k) {
            std::vector<float>& band = bands[static_cast<std::size_t>(k)];
            if (static_cast<int>(band.size()) < kMinBandPoints) {
                continue;
            }
            const float x = kFitLo + (kFitHi - kFitLo) * (static_cast<float>(k) + 0.5f) / static_cast<float>(kBands);
            const float y = pct(band, p);
            sx += x; sy += y; sxx += x * x; sxy += x * y;
            ++count;
        }
        if (count == 0) {
            return out;
        }
        out.ok = true;
        if (count == 1) {
            out.a = out.b = sy;
            return out;
        }
        const float cnt = static_cast<float>(count);
        const float denom = cnt * sxx - sx * sx;
        const float slope = std::abs(denom) > 1e-9f ? (cnt * sxy - sx * sy) / denom : 0.0f;
        const float icpt = (sy - slope * sx) / cnt;
        out.a = icpt;
        out.b = icpt + slope;
        return out;
    }
    /// The points of @p member along the axis A0->B0 (already extended), banded (see Bands). With
    /// @p half, only the extent TOWARD it counts (a limb's inner surface).
    Bands collect(const std::vector<char>& member, const glm::vec3& A0, const glm::vec3& B0,
                  const glm::vec3* lat, const glm::vec3* fwd, const glm::vec3* half) const {
        Bands out;
        out.r.assign(kBands, {});
        out.u.assign(kBands, {});
        out.w.assign(kBands, {});
        const glm::vec3 ab = B0 - A0;
        const float     L = glm::length(ab);
        if (L < 1e-6f) {
            return out;
        }
        const glm::vec3 up = ab / L;
        for (const BodyMeshPoint& pt : *mesh) {
            if (pt.bone < 0 || pt.bone >= static_cast<int>(n) || !member[static_cast<std::size_t>(pt.bone)]) {
                continue;
            }
            const glm::vec3 rel = pt.pos - A0;
            const float     axial = glm::dot(rel, up);
            const float     t = axial / L;
            out.axial.push_back(axial);
            if (t < kFitLo || t > kFitHi) {
                continue;
            }
            int k = static_cast<int>((t - kFitLo) / (kFitHi - kFitLo) * static_cast<float>(kBands));
            k = std::max(0, std::min(kBands - 1, k));
            const glm::vec3 radial = rel - up * axial;
            if (half != nullptr) {
                // The limb's extent TOWARD @p half (its inner surface): the far side and the
                // front/back are not the surface this capsule stands for.
                const float toward = glm::dot(radial, *half);
                if (toward <= 0.0f) {
                    continue;
                }
                out.r[static_cast<std::size_t>(k)].push_back(toward);
                ++out.total;
                continue;
            }
            out.r[static_cast<std::size_t>(k)].push_back(glm::length(radial));
            if (lat != nullptr && fwd != nullptr) {
                out.u[static_cast<std::size_t>(k)].push_back(std::abs(glm::dot(radial, *lat)));
                out.w[static_cast<std::size_t>(k)].push_back(glm::dot(radial, *fwd));
            }
            ++out.total;
        }
        return out;
    }
    /// The section frame at bind for lateral references (refR -> refL, orthogonalized to the
    /// axis; forward = their cross) — the same frame volumeAxis rebuilds in every pose.
    bool bindFrame(int refL, int refR, const glm::vec3& up, glm::vec3& lat, glm::vec3& fwd) const {
        if (refL < 0 || refR < 0) {
            return false;
        }
        lat = bindOf(refL) - bindOf(refR);
        lat -= up * glm::dot(lat, up);
        if (glm::length(lat) < 1e-6f) {
            return false;
        }
        lat = glm::normalize(lat);
        fwd = glm::cross(lat, up);
        return true;
    }
    /// Adds the capsule @p spec describes to the rig's volumes, marking which joints and riders
    /// of the limbs it keeps out it applies to — never one that overlaps it at bind.
    void addVolume(const VolumeSpec& spec) {
        if (spec.a < 0 || spec.b < 0 || spec.radius <= 0.0f) {
            return;
        }
        BodyVolume v;
        v.a = spec.a;
        v.b = spec.b;
        v.radius = spec.radius;
        v.radiusB = spec.radiusB;
        v.extendA = spec.extendA;
        v.extendB = spec.extendB;
        v.offA = spec.offA;
        v.offB = spec.offB;
        v.refL = spec.refL;
        v.refR = spec.refR;
        v.segments = spec.segments;
        v.applies.assign(n, 0);
        // The capsule at BIND, for the rest-overlap exemption.
        const VolumeAxis ax = volumeAxis(v, static_cast<int>(n), [&](int i) { return bindOf(i); });
        if (!ax.ok) {
            return;
        }
        int applied = 0;
        int ridersApplied = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!rig.m_bodyNode[i] || limbOf[i] < 0 || isHelperBone(i)) {
                continue;
            }
            const int limb = limbOf[i];
            if (std::find(spec.limbs.begin(), spec.limbs.end(), limb) == spec.limbs.end()) {
                continue;
            }
            char  applies = 0;
            float clearance = 0.0f;
            if (rider[i]) {
                // A hand's rider, where the volume takes riders.
                if (!spec.riders || kNoRiders || rig.m_riderHand[i] < 0) {
                    continue;
                }
                applies = kVolumeAppliesRider;
                clearance = rig.m_riderClearance;
            } else {
                // Never a token-LIMB joint (the fingers, the toes): riders the solve never
                // places (they follow the hand rigidly), so a thumb can still touch the chest
                // — as it would — while the hand keeps its clearance. A mid-limb twist helper
                // (its subtree carries the hand) IS tested: it samples the forearm's middle,
                // where a forearm laid across the chest cuts through it with the elbow and
                // the hand both outside.
                const int root = limb < 2 ? armRoot[limb] : thighRoot[limb - 2];
                if (rig.m_subtreeMass[i] < kTokenLimbMass || static_cast<int>(i) == root) {
                    continue;
                }
                applies = (i < spec.segmentNodes.size() && spec.segmentNodes[i]) ? kVolumeAppliesJointSegment
                                                                                  : kVolumeAppliesJoint;
                clearance = rig.m_volumeClearance[i];
            }
            const glm::vec3& P = rig.m_bindPos[i];
            const float      t = volumeAxisParam(ax, P);
            const float      dist = glm::length(P - (ax.A + ax.ab * t));
            if (dist < volumeRadiusAt(v, t) + clearance) {
                if (kTrace) {
                    std::fprintf(stderr, "[volume] %s overlaps volume %d-%d at bind (%.1f < %.1f mm): exempt\n",
                                 names[i].c_str(), spec.a, spec.b, dist * 1000.0f,
                                 (volumeRadiusAt(v, t) + clearance) * 1000.0f);
                }
                continue;
            }
            v.applies[i] = applies;
            if (applies == kVolumeAppliesRider) {
                ++ridersApplied;
            } else {
                ++applied;
            }
        }
        if (kTrace) {
            std::fprintf(stderr, "[volume] %s %s-%s radius %.1f/%.1f mm ext %.1f/%.1f off (%.1f,%.1f)/(%.1f,%.1f)%s applies to %d joints, %d riders\n",
                         spec.what, names[static_cast<std::size_t>(spec.a)].c_str(),
                         names[static_cast<std::size_t>(spec.b)].c_str(), v.radius * 1000.0f,
                         volumeRadiusAt(v, 1.0f) * 1000.0f, v.extendA * 1000.0f, v.extendB * 1000.0f,
                         v.offA.x * 1000.0f, v.offA.z * 1000.0f, v.offB.x * 1000.0f, v.offB.z * 1000.0f,
                         v.segments ? " segments" : "", applied, ridersApplied);
        }
        rig.m_bodyVolumes.push_back(std::move(v));
    }
    /// A limb capsule (a -> b, circular, tapered) fitted to its bones' vertices; @p fallback is
    /// the skeleton-sized radius, which also bounds the fit (a sample assigned to the wrong
    /// bones must not make a thigh a metre wide or a wrist a pin). With @p half, only the
    /// vertices on that side of the axis count: a LEG's capsule stands for its INNER surface,
    /// the one the other leg meets — a thigh is 10cm to its outer hip flesh and 6cm to the
    /// inner thigh, and an all-round radius held the crossing legs 15cm apart at the crotch,
    /// where they rest touching (the legs-crossed knee trembled 440mm against it).
    void fitLimb(VolumeSpec& spec, float fallback, const glm::vec3* half) const {
        spec.radius = fallback;
        spec.radiusB = -1.0f;
        if (!fitted) {
            return;
        }
        const std::vector<char> member = chainMembers(spec.a, spec.b, false);
        const glm::vec3         A0 = bindOf(spec.a);
        const glm::vec3         B0 = bindOf(spec.b);
        glm::vec3               halfDir(0.0f);
        const glm::vec3*        halfPtr = nullptr;
        if (half != nullptr && glm::length(B0 - A0) > 1e-6f) {
            const glm::vec3 up = glm::normalize(B0 - A0);
            halfDir = *half - up * glm::dot(*half, up);
            if (glm::length(halfDir) > 1e-6f) {
                halfDir = glm::normalize(halfDir);
                halfPtr = &halfDir;
            }
        }
        // The inner surface takes a higher percentile than the all-round median: the median of
        // an extent is half-way to the surface (the points spread from the axis outward).
        Bands         bands = collect(member, A0, B0, nullptr, nullptr, halfPtr);
        const EndPair r = regress(bands.r, halfPtr != nullptr ? kInnerPct : kLimbPct);
        if (!r.ok) {
            return;
        }
        spec.radius = glm::clamp(r.a, 0.5f * fallback, 2.5f * fallback);
        spec.radiusB = glm::clamp(r.b, 0.5f * fallback, 2.5f * fallback);
    }

    // ---- The parts.

    /// The body's parts by their roles, the limb membership, the riders, the hands' and feet's
    /// wider clearance, the hip half-spacing.
    void prepare() {
        const RigSide sides[2] = {RigSide::Left, RigSide::Right};
        for (int side = 0; side < 2; ++side) {
            armRoot[side] = roles.find(RigRole::UpperArm, sides[side]);
            thighRoot[side] = roles.find(RigRole::Thigh, sides[side]);
        }
        limbOf.assign(n, -1);
        for (std::size_t i = 0; i < n; ++i) {
            for (int cur = static_cast<int>(i); cur >= 0; cur = rig.m_parents[static_cast<std::size_t>(cur)]) {
                if (cur == armRoot[0]) { limbOf[i] = 0; break; }
                if (cur == armRoot[1]) { limbOf[i] = 1; break; }
                if (cur == thighRoot[0]) { limbOf[i] = 2; break; }
                if (cur == thighRoot[1]) { limbOf[i] = 3; break; }
            }
        }
        for (int side = 0; side < 2; ++side) {
            knee[side] = roles.find(RigRole::Shin, sides[side]);
            ankle[side] = roles.find(RigRole::Foot, sides[side]);
            elbow[side] = roles.find(RigRole::Forearm, sides[side]);
            hand[side] = roles.find(RigRole::Hand, sides[side]);
            upperArm[side] = armRoot[side];
        }
        neckBase = rig.m_neckBase; // (the roles', set at build before the volumes)
        headNode = rig.m_headNode;
        // Hands and feet carry their fingers and toes outside the joint: a wider clearance.
        for (std::size_t i = 0; i < n; ++i) {
            const RigRole r = roles.role(static_cast<int>(i));
            if (rig.m_bodyNode[i] && (r == RigRole::Hand || r == RigRole::Foot)) {
                rig.m_volumeClearance[i] = 0.02f * rig.m_sizeScale;
            }
        }
        halfSpacing = (thighRoot[0] >= 0 && thighRoot[1] >= 0)
                          ? 0.5f * std::abs(bindOf(thighRoot[0]).x - bindOf(thighRoot[1]).x)
                          : 0.08f * rig.m_sizeScale;
        // RIDERS of a hand or a foot — the fingers, thumbs, carpals, toes, metatarsals — are never
        // tested as JOINTS, whatever mass their subtrees sum to: the hand joint stands for the
        // hand, the foot joint for the foot (their clearance is the wider one for it). The mass
        // rule alone let the third generation's THUMB through: its subtree cleared the token
        // threshold, and the other arm's capsules pushed the thumb 7cm while the hand it rides sat
        // outside them. A HAND's riders are tested as riders instead (kVolumeAppliesRider) against
        // the torso and the head, where their positions are exact: a hand pressed into the chest
        // stops at its fingertips.
        rider.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            for (int cur = rig.m_parents[i]; cur >= 0; cur = rig.m_parents[static_cast<std::size_t>(cur)]) {
                if (cur == hand[0] || cur == hand[1]) {
                    rider[i] = 1;
                    rig.m_riderHand[i] = cur;
                    break;
                }
                if (cur == ankle[0] || cur == ankle[1]) {
                    rider[i] = 1;
                    break;
                }
            }
        }
    }

    /// THE TORSO: fitted as ONE PROFILE along the whole spine (banded by height, each band's
    /// section measured over thousands of vertices), sampled at each spine joint for that
    /// bone's capsules — a centre capsule and two side capsules per segment. A per-bone fit was
    /// tried first and rejected: a spine bone is 3-10cm long, its vertices a thin slab that
    /// interleaves with its neighbours', and a taper regressed within one slab extrapolated
    /// nonsense (a neck segment 137mm wide, a chest 50mm at one end and 120mm at the other).
    /// Without a fit (no sample, or nothing to fit to): one skeleton-sized cylinder.
    void torso() {
        bool torsoFitted = false;
        if (fitted && rig.m_pelvis >= 0 && neckBase >= 0) {
            std::vector<int> chain; // the neck base down to the root
            bool             reached = false;
            for (int cur = neckBase; cur >= 0; cur = rig.m_parents[static_cast<std::size_t>(cur)]) {
                chain.push_back(cur);
                if (cur == rig.m_pelvis) {
                    reached = true;
                    break;
                }
            }
            // The lateral references: the arm roots for the segments at and above the chest node
            // the arms hang from, the hip sockets below (a twisted trunk turns its shoulders).
            int armJunction = -1;
            if (armRoot[0] >= 0) {
                for (int cur = rig.m_parents[static_cast<std::size_t>(armRoot[0])]; cur >= 0;
                     cur = rig.m_parents[static_cast<std::size_t>(cur)]) {
                    if (std::find(chain.begin(), chain.end(), cur) != chain.end()) {
                        armJunction = cur;
                        break;
                    }
                }
            }
            const bool      haveArms = armRoot[0] >= 0 && armRoot[1] >= 0;
            const bool      haveHips = thighRoot[0] >= 0 && thighRoot[1] >= 0;
            int             fittedSegments = 0;
            const glm::vec3 rootPos = bindOf(rig.m_pelvis);
            const glm::vec3 neckPos = bindOf(neckBase);
            const float     L0 = glm::length(neckPos - rootPos);
            glm::vec3       lat, fwd;
            const glm::vec3 up = L0 > 1e-6f ? (neckPos - rootPos) / L0 : glm::vec3(0.0f, 1.0f, 0.0f);
            const bool      frameOk = haveHips ? bindFrame(thighRoot[0], thighRoot[1], up, lat, fwd)
                                               : bindFrame(armRoot[0], armRoot[1], up, lat, fwd);
            if (reached && chain.size() >= 2 && (haveArms || haveHips) && L0 > 1e-6f && frameOk) {
                const float fallback = 1.2f * halfSpacing;
                // The axis: the root's point extended down to the hip sockets' level (the pelvis
                // flesh), up to the neck base.
                float extendRoot = 0.0f;
                if (haveHips) {
                    const float socketY = 0.5f * (bindOf(thighRoot[0]).y + bindOf(thighRoot[1]).y);
                    extendRoot = std::max(0.0f, rootPos.y - socketY);
                }
                const glm::vec3         A0 = rootPos - up * extendRoot;
                const float             Ltot = L0 + extendRoot;
                const std::vector<char> member = chainMembers(rig.m_pelvis, neckBase, false);
                const int               nb = std::max(4, std::min(16, static_cast<int>(Ltot / 0.05f + 0.5f)));
                std::vector<std::vector<float>> bu(static_cast<std::size_t>(nb)), bw(static_cast<std::size_t>(nb));
                for (const BodyMeshPoint& pt : *mesh) {
                    if (pt.bone < 0 || pt.bone >= static_cast<int>(n) || !member[static_cast<std::size_t>(pt.bone)]) {
                        continue;
                    }
                    const glm::vec3 rel = pt.pos - A0;
                    const float     axial = glm::dot(rel, up);
                    if (axial < -0.02f || axial > Ltot + 0.02f) {
                        continue;
                    }
                    const int       k = std::max(0, std::min(nb - 1, static_cast<int>(axial / Ltot * static_cast<float>(nb))));
                    const glm::vec3 radial = rel - up * axial;
                    bu[static_cast<std::size_t>(k)].push_back(std::abs(glm::dot(radial, lat)));
                    bw[static_cast<std::size_t>(k)].push_back(glm::dot(radial, fwd));
                }
                std::vector<float> width(static_cast<std::size_t>(nb)), front(static_cast<std::size_t>(nb)), back(static_cast<std::size_t>(nb));
                std::vector<char>  valid(static_cast<std::size_t>(nb), 0);
                int                validCount = 0;
                for (int k = 0; k < nb; ++k) {
                    const std::size_t ki = static_cast<std::size_t>(k);
                    if (static_cast<int>(bu[ki].size()) < kMinBandPoints) {
                        continue;
                    }
                    std::vector<float> w2 = bw[ki];
                    width[ki] = pct(bu[ki], kSidePct);
                    front[ki] = pct(bw[ki], kFrontPct);
                    back[ki] = pct(w2, kBackPct);
                    valid[ki] = 1;
                    ++validCount;
                }
                // An empty band (none expected) takes its nearest measured neighbour's section.
                for (int k = 0; k < nb && validCount > 0; ++k) {
                    if (valid[static_cast<std::size_t>(k)]) {
                        continue;
                    }
                    int best = -1;
                    for (int d = 1; d < nb && best < 0; ++d) {
                        if (k - d >= 0 && valid[static_cast<std::size_t>(k - d)]) best = k - d;
                        else if (k + d < nb && valid[static_cast<std::size_t>(k + d)]) best = k + d;
                    }
                    const std::size_t bi = static_cast<std::size_t>(best);
                    width[static_cast<std::size_t>(k)] = width[bi];
                    front[static_cast<std::size_t>(k)] = front[bi];
                    back[static_cast<std::size_t>(k)] = back[bi];
                }
                // The section at an axial position: interpolated between the band centres.
                const auto sample = [&](float axial, float& w, float& f, float& b) {
                    const float       x = axial / Ltot * static_cast<float>(nb) - 0.5f;
                    const int         k0 = std::max(0, std::min(nb - 1, static_cast<int>(std::floor(x))));
                    const int         k1 = std::max(0, std::min(nb - 1, k0 + 1));
                    const float       f01 = std::max(0.0f, std::min(1.0f, x - static_cast<float>(k0)));
                    const std::size_t i0 = static_cast<std::size_t>(k0), i1 = static_cast<std::size_t>(k1);
                    w = width[i0] + (width[i1] - width[i0]) * f01;
                    f = front[i0] + (front[i1] - front[i0]) * f01;
                    b = back[i0] + (back[i1] - back[i0]) * f01;
                };
                if (kTrace && validCount > 0) {
                    for (int k = 0; k < nb; ++k) {
                        const std::size_t ki = static_cast<std::size_t>(k);
                        std::fprintf(stderr, "[volume] torso band %d at %.0f mm: half-width %.0f front %.0f back %.0f (%zu points)\n", k,
                                     (static_cast<float>(k) + 0.5f) / static_cast<float>(nb) * Ltot * 1000.0f,
                                     width[ki] * 1000.0f, front[ki] * 1000.0f, back[ki] * 1000.0f, bu[ki].size());
                    }
                }
                bool useArms = false;
                for (std::size_t k = chain.size() - 1; k >= 1 && validCount > 0; --k) {
                    const int a = chain[k];
                    const int b = chain[k - 1];
                    if (a == armJunction) {
                        useArms = true;
                    }
                    const int   refL = (useArms && haveArms) || !haveHips ? armRoot[0] : thighRoot[0];
                    const int   refR = (useArms && haveArms) || !haveHips ? armRoot[1] : thighRoot[1];
                    const float extendA = a == rig.m_pelvis ? extendRoot : 0.0f;
                    const float axialA = glm::dot(bindOf(a) - A0, up) - extendA;
                    const float axialB = glm::dot(bindOf(b) - A0, up);
                    float       wA, fA, kA, wB, fB, kB;
                    sample(axialA, wA, fA, kA);
                    sample(axialB, wB, fB, kB);
                    const float depthA = glm::clamp(0.5f * (fA - kA), 0.5f * fallback, 2.5f * fallback);
                    const float depthB = glm::clamp(0.5f * (fB - kB), 0.5f * fallback, 2.5f * fallback);
                    const float centreA = 0.5f * (fA + kA);
                    const float centreB = 0.5f * (fB + kB);
                    const float sideA = glm::clamp(wA - depthA, 0.0f, 3.0f * halfSpacing);
                    const float sideB = glm::clamp(wB - depthB, 0.0f, 3.0f * halfSpacing);
                    VolumeSpec  centre;
                    centre.a = a;
                    centre.b = b;
                    centre.radius = depthA;
                    centre.radiusB = depthB;
                    centre.extendA = extendA;
                    centre.offA = glm::vec3(0.0f, 0.0f, centreA);
                    centre.offB = glm::vec3(0.0f, 0.0f, centreB);
                    centre.refL = refL;
                    centre.refR = refR;
                    centre.limbs = arms;
                    centre.riders = true;
                    // The fitted torso tests the UPPER ARM's segments (kVolumeAppliesJointSegment:
                    // the nodes from the elbow up to the arm root): laid against the ribs the
                    // upper arm is kept out by its own flesh, not only at its two joints. Not the
                    // forearm's and hand's, which sweep across the chest and the breasts (held to
                    // their flesh they stopped the cross-body reach: the base rig stepped after
                    // the cursor, the male's hand stalled 23cm short). The coarse cylinder could
                    // test no segment at all. IK_VOLUME_TORSO_NO_SEGMENTS=1 is the A/B probe.
                    if (!kTorsoNoSegments) {
                        centre.segmentNodes.assign(n, 0);
                        for (int side = 0; side < 2; ++side) {
                            for (int cur = elbow[side]; cur >= 0 && cur != upperArm[side] && cur != armRoot[side];
                                 cur = rig.m_parents[static_cast<std::size_t>(cur)]) {
                                centre.segmentNodes[static_cast<std::size_t>(cur)] = 1;
                            }
                        }
                    }
                    centre.what = "torso";
                    addVolume(centre);
                    ++fittedSegments;
                    if (sideA > 0.01f || sideB > 0.01f) {
                        for (int side = 0; side < 2; ++side) {
                            VolumeSpec  sv = centre;
                            const float sign = side == 0 ? 1.0f : -1.0f;
                            sv.offA.x = sign * sideA;
                            sv.offB.x = sign * sideB;
                            sv.what = side == 0 ? "torso-left" : "torso-right";
                            addVolume(sv);
                        }
                    }
                }
            }
            torsoFitted = fittedSegments > 0;
        }
        if (!torsoFitted && rig.m_pelvis >= 0 && neckBase >= 0) {
            VolumeSpec torsoSpec;
            torsoSpec.a = rig.m_pelvis;
            torsoSpec.b = neckBase;
            torsoSpec.radius = 1.2f * halfSpacing;
            torsoSpec.limbs = arms;
            torsoSpec.riders = true;
            torsoSpec.what = "torso";
            addVolume(torsoSpec);
        }
    }

    /// THE HEAD: the neck base through the head joint, the skull past it — the skull's radius
    /// from the head's own points, the crown from their axial extent.
    void head() {
        if (neckBase < 0 || headNode < 0) {
            return;
        }
        VolumeSpec spec;
        spec.a = neckBase;
        spec.b = headNode;
        spec.radius = 1.0f * halfSpacing;
        spec.extendB = 0.75f * halfSpacing;
        spec.limbs = arms;
        spec.riders = true;
        spec.what = "head";
        if (fitted) {
            const std::vector<char> member = chainMembers(neckBase, headNode, true);
            const glm::vec3         A0 = bindOf(neckBase);
            const glm::vec3         B0 = bindOf(headNode);
            const float             L = glm::length(B0 - A0);
            // The skull: the radial percentile over the head's own points, the crown from the
            // axial extent; the window is the head itself (the joint on), not the neck.
            std::vector<float> radial;
            std::vector<float> axialAll;
            if (L > 1e-6f) {
                const glm::vec3 up = (B0 - A0) / L;
                for (const BodyMeshPoint& pt : *mesh) {
                    if (pt.bone < 0 || pt.bone >= static_cast<int>(n) || !member[static_cast<std::size_t>(pt.bone)]) {
                        continue;
                    }
                    const glm::vec3 rel = pt.pos - A0;
                    const float     axial = glm::dot(rel, up);
                    if (axial < L) {
                        continue;
                    }
                    radial.push_back(glm::length(rel - up * axial));
                    axialAll.push_back(axial);
                }
            }
            if (static_cast<int>(radial.size()) >= kMinBandPoints) {
                const float r = glm::clamp(pct(radial, kHeadPct), 0.5f * halfSpacing, 2.0f * halfSpacing);
                const float crown = pct(axialAll, 0.98f);
                spec.radius = r;
                spec.extendB = glm::clamp(crown - r - L, 0.0f, 2.0f * halfSpacing);
            }
        }
        addVolume(spec);
    }

    /// THE THIGHS keep the OTHER leg out (legs never cross through each other): each a capsule
    /// of its inner surface.
    void thighs() {
        for (int side = 0; side < 2; ++side) {
            if (thighRoot[side] >= 0 && knee[side] >= 0) {
                VolumeSpec thigh;
                thigh.a = thighRoot[side];
                thigh.b = knee[side];
                thigh.limbs = std::vector<int>{2 + (1 - side)};
                thigh.segments = true;
                thigh.what = "thigh";
                glm::vec3        inner(0.0f);
                const glm::vec3* innerPtr = nullptr;
                if (thighRoot[1 - side] >= 0) {
                    inner = bindOf(thighRoot[1 - side]) - bindOf(thighRoot[side]);
                    innerPtr = &inner;
                }
                fitLimb(thigh, 0.9f * halfSpacing, innerPtr);
                addVolume(thigh);
            }
        }
    }

    /// LIMB AGAINST LIMB (the self-collision round 2): each SHIN keeps the other leg out (a
    /// foot dragged across went around the thigh and through the shin), each UPPER ARM and
    /// FOREARM keeps the other arm out (a hand dragged across the chest went through the
    /// other forearm).
    void limbPairs() {
        for (int side = 0; side < 2 && !kNoLimbVolumes; ++side) {
            if (knee[side] >= 0 && ankle[side] >= 0) {
                VolumeSpec shin;
                shin.a = knee[side];
                shin.b = ankle[side];
                shin.limbs = std::vector<int>{2 + (1 - side)};
                shin.segments = true;
                shin.what = "shin";
                glm::vec3        inner(0.0f);
                const glm::vec3* innerPtr = nullptr;
                if (thighRoot[side] >= 0 && thighRoot[1 - side] >= 0) {
                    inner = bindOf(thighRoot[1 - side]) - bindOf(thighRoot[side]);
                    innerPtr = &inner;
                }
                fitLimb(shin, 0.55f * halfSpacing, innerPtr);
                addVolume(shin);
            }
            if (upperArm[side] >= 0 && elbow[side] >= 0) {
                VolumeSpec upper;
                upper.a = upperArm[side];
                upper.b = elbow[side];
                upper.limbs = std::vector<int>{1 - side};
                upper.segments = true;
                upper.what = "upper arm";
                fitLimb(upper, 0.55f * halfSpacing, nullptr);
                addVolume(upper);
            }
            if (elbow[side] >= 0 && hand[side] >= 0) {
                // Not past the wrist: hands plant side by side in an all-fours and clasp each
                // other, and a capsule reaching into the palm held them 5cm apart.
                VolumeSpec fore;
                fore.a = elbow[side];
                fore.b = hand[side];
                fore.limbs = std::vector<int>{1 - side};
                fore.segments = true;
                fore.what = "forearm";
                fitLimb(fore, 0.45f * halfSpacing, nullptr);
                addVolume(fore);
            }
        }
    }

    /// Every node on a capsule's chain (its b end up to, excluding, its a end) carries that
    /// capsule's radius WHERE IT STANDS as the flesh radius of the segment ending at it — the
    /// volume rules keep the whole segment out with it (see IkRig::volumeSegmentRadius).
    void segmentRadii() {
        for (const BodyVolume& v : rig.m_bodyVolumes) {
            const VolumeAxis ax = volumeAxis(v, static_cast<int>(n), [&](int i) { return bindOf(i); });
            if (!ax.ok) {
                continue;
            }
            int guard = 0;
            for (int cur = v.b; cur >= 0 && cur != v.a && guard < 64; cur = rig.m_parents[static_cast<std::size_t>(cur)], ++guard) {
                const glm::vec3& P = rig.m_bindPos[static_cast<std::size_t>(cur)];
                const float      t = volumeAxisParam(ax, P);
                rig.m_volumeSegmentRadius[static_cast<std::size_t>(cur)] =
                    std::max(rig.m_volumeSegmentRadius[static_cast<std::size_t>(cur)], volumeRadiusAt(v, t));
            }
        }
    }

    /// The rest-overlap exemption for SEGMENTS: a tested joint whose segment already crosses a
    /// volume at bind is exempt from that volume, like a joint that sits inside one (the
    /// newest generation's collar runs through the chest, and its upper arm read 97mm inside
    /// the torso in every pose — the solver then fought the rest pose itself).
    void segmentRestExemption() {
        for (BodyVolume& v : rig.m_bodyVolumes) {
            const VolumeAxis ax = volumeAxis(v, static_cast<int>(n), [&](int i) { return bindOf(i); });
            if (!ax.ok) {
                continue;
            }
            for (std::size_t i = 0; i < n; ++i) {
                const int   par = rig.m_parents[i];
                const float segR = rig.m_volumeSegmentRadius[i];
                if (!volumeAppliesSegment(v, v.applies[i]) || par < 0 || segR <= 0.0f) {
                    continue;
                }
                float       sPar = 0.0f;
                float       tPar = 0.0f;
                const float dSeg = closestSegmentPoints(rig.m_bindPos[static_cast<std::size_t>(par)],
                                                        rig.m_bindPos[i], ax.A, ax.B, sPar, tPar);
                const float keepSeg = volumeRadiusAt(v, tPar) + std::max(kVolumeSegmentFlesh * segR, rig.m_volumeClearance[i]);
                if (dSeg < keepSeg) {
                    if (kTrace) {
                        std::fprintf(stderr, "[volume] the segment into %s crosses volume %d-%d at bind (%.1f < %.1f mm): exempt\n",
                                     names[i].c_str(), v.a, v.b, dSeg * 1000.0f, keepSeg * 1000.0f);
                    }
                    v.applies[i] = 0;
                }
            }
        }
    }
};

void IkRig::buildBodyVolumes(const std::vector<std::string>& names, const std::vector<BodyMeshPoint>* mesh) {
    const std::size_t n = m_bindPos.size();
    m_bodyVolumes.clear();
    m_volumeClearance.assign(n, kFleshRadius * m_sizeScale);
    m_volumeSegmentRadius.assign(n, 0.0f);
    m_riderHand.assign(n, -1);
    m_riderClearance = kRiderClearance * m_sizeScale;
    if (n == 0 || names.size() != n) {
        return;
    }
    VolumeFitter(*this, names, mesh).run();
}

void IkRig::maskOverlappingVolumes(const std::vector<glm::vec3>& positions) {
    const int n = static_cast<int>(positions.size());
    m_dragVolumes = m_bodyVolumes;
    if (static_cast<int>(m_volumeClearance.size()) != n) {
        return;
    }
    constexpr float kOverlapTol = 0.002f;
    for (BodyVolume& v : m_dragVolumes) {
        const VolumeAxis ax = volumeAxis(v, n, [&](int i) { return positions[static_cast<std::size_t>(i)]; });
        if (!ax.ok) {
            continue;
        }
        for (int i = 0; i < n; ++i) {
            const char applies = v.applies[static_cast<std::size_t>(i)];
            if (!applies) {
                continue;
            }
            const float clearance = applies == kVolumeAppliesRider ? m_riderClearance
                                                                    : m_volumeClearance[static_cast<std::size_t>(i)];
            const glm::vec3& P = positions[static_cast<std::size_t>(i)];
            const float t = volumeAxisParam(ax, P);
            const float d = glm::length(P - (ax.A + ax.ab * t));
            bool overlaps = d < volumeRadiusAt(v, t) + clearance - kOverlapTol;
            const int par = m_parents[static_cast<std::size_t>(i)];
            const float segR = m_volumeSegmentRadius[static_cast<std::size_t>(i)];
            if (!overlaps && volumeAppliesSegment(v, applies) && segR > 0.0f && par >= 0) {
                float sPar = 0.0f;
                float tPar = 0.0f;
                const float dSeg = closestSegmentPoints(positions[static_cast<std::size_t>(par)], P, ax.A, ax.B, sPar, tPar);
                overlaps = dSeg < volumeRadiusAt(v, tPar) + std::max(kVolumeSegmentFlesh * segR, clearance) - kOverlapTol;
            }
            if (overlaps) {
                v.applies[static_cast<std::size_t>(i)] = 0;
                if (kTrace) {
                    std::fprintf(stderr, "[volume] node %d overlaps volume %d-%d at drag start: exempt this drag\n",
                                i, v.a, v.b);
                }
            }
        }
    }
}

glm::vec3 IkRig::clampOutOfVolumes(int node, glm::vec3 target,
                                   const std::vector<glm::vec3>& positions) const {
    const int n = static_cast<int>(positions.size());
    if (node < 0 || node >= n || static_cast<int>(m_volumeClearance.size()) != n) {
        return target;
    }
    const auto pos = [&](int i) { return positions[static_cast<std::size_t>(i)]; };
    // The SEGMENT into the effector too (its parent where it stands now — the forearm laid
    // across the chest to a cursor just past the far shoulder cut the chest by 3cm while the
    // cursor itself sat outside the capsule, and the finisher held the hand there): the
    // target is moved out along the segment's closest point, over that point's parameter.
    const int par = m_parents[static_cast<std::size_t>(node)];
    const float segR = volumeSegmentRadius(node);
    for (int round = 0; round < 4 && par >= 0 && segR > 0.0f; ++round) {
        bool moved = false;
        for (const BodyVolume& v : dragVolumes()) {
            const VolumeAxis ax = volumeAxis(v, n, pos);
            if (!ax.ok || !volumeAppliesSegment(v, v.applies[static_cast<std::size_t>(node)])) {
                continue;
            }
            const glm::vec3& PP = positions[static_cast<std::size_t>(par)];
            float sPar = 0.0f;
            float tPar = 0.0f;
            const float dSeg = closestSegmentPoints(PP, target, ax.A, ax.B, sPar, tPar);
            const float keepSeg = volumeRadiusAt(v, tPar) + std::max(kVolumeSegmentFlesh * segR, m_volumeClearance[static_cast<std::size_t>(node)]) + 0.001f;
            if (dSeg >= keepSeg || sPar < 0.05f) {
                continue;
            }
            const glm::vec3 S = PP + (target - PP) * sPar;
            glm::vec3 nrm = S - (ax.A + ax.ab * tPar);
            if (glm::length(nrm) > 1e-5f) {
                nrm = glm::normalize(nrm);
            } else if (ax.ab2 > 1e-12f) {
                nrm = glm::cross(glm::normalize(ax.ab), target - PP);
                nrm = glm::length(nrm) > 1e-5f ? glm::normalize(nrm) : glm::vec3(1.0f, 0.0f, 0.0f);
            } else {
                nrm = glm::vec3(1.0f, 0.0f, 0.0f);
            }
            target += nrm * ((keepSeg - dSeg) / sPar);
            moved = true;
        }
        if (!moved) {
            break;
        }
    }
    const auto pointOut = [&](const BodyVolume& v, const VolumeAxis& ax, const glm::vec3& P, float clearance,
                              glm::vec3& push) {
        const float t = volumeAxisParam(ax, P);
        const glm::vec3 Q = ax.A + ax.ab * t;
        const float keep = volumeRadiusAt(v, t) + clearance + 0.001f;
        const glm::vec3 away = P - Q;
        const float d = glm::length(away);
        if (d >= keep) {
            return false;
        }
        glm::vec3 nrm(1.0f, 0.0f, 0.0f);
        if (d > 1e-5f) {
            nrm = away / d;
        } else if (ax.ab2 > 1e-12f) {
            nrm = glm::cross(ax.ab / std::sqrt(ax.ab2), glm::vec3(0.0f, 1.0f, 0.0f));
            nrm = glm::length(nrm) > 1e-5f ? glm::normalize(nrm) : glm::vec3(1.0f, 0.0f, 0.0f);
        }
        push = nrm * (keep - d);
        return true;
    };
    for (const BodyVolume& v : dragVolumes()) {
        const VolumeAxis ax = volumeAxis(v, n, pos);
        if (!ax.ok || !volumeAppliesJoint(v.applies[static_cast<std::size_t>(node)])) {
            continue;
        }
        glm::vec3 push;
        if (pointOut(v, ax, target, m_volumeClearance[static_cast<std::size_t>(node)], push)) {
            target += push;
        }
    }
    // The effector's RIDERS (a hand's fingers), carried along with it: where the hand would
    // stand at the target, each rider stands at its current offset from the hand, and the
    // target moves so the deepest of them clears the volume — a cursor pushed into the chest
    // asks for the hand at the fingertips' reach, not the wrist's.
    std::vector<int> riders;
    for (int i = 0; i < n; ++i) {
        if (riderHand(i) == node) {
            riders.push_back(i);
        }
    }
    for (int round = 0; round < 3 && !riders.empty(); ++round) {
        bool moved = false;
        for (const BodyVolume& v : dragVolumes()) {
            const VolumeAxis ax = volumeAxis(v, n, pos);
            if (!ax.ok) {
                continue;
            }
            for (const int r : riders) {
                if (v.applies[static_cast<std::size_t>(r)] != kVolumeAppliesRider) {
                    continue;
                }
                const glm::vec3 Pr = target + (positions[static_cast<std::size_t>(r)] - positions[static_cast<std::size_t>(node)]);
                glm::vec3 push;
                if (pointOut(v, ax, Pr, m_riderClearance, push)) {
                    target += push;
                    moved = true;
                }
            }
        }
        if (!moved) {
            break;
        }
    }
    return target;
}

} // namespace pose

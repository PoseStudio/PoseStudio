/**
 * @file bodyvolume.h
 * @brief The self-collision BODY VOLUMES: tapered capsules between joints (the torso's sections,
 *        the head, the thighs, shins, upper arms and forearms), with the per-node record of which
 *        joints, limb segments and finger riders each one keeps out, and the ONE reading of a
 *        capsule's axis in a pose that every test shares.
 *
 * Built and fitted to the figure's mesh by IkRig::buildBodyVolumes; read by the Armature's solve
 * (one-sided rows), the drag-target clamp, the FK collision stop and the harness. Qt-free (GLM).
 */

#ifndef BODYVOLUME_H
#define BODYVOLUME_H

#include <glm/glm.hpp>

#include <vector>

namespace pose {

/// A BODY VOLUME for self-collision (see IkRig::bodyVolumes): a capsule between the
/// current positions of joints @p a and @p b — @p b pushed @p extendB further along a->b (a
/// head's skull past the head joint) — of @p radius, applied to the joints flagged in
/// @p applies (the other limbs' joints: an arm against the torso, a foot against the other
/// thigh), never to the joints the volume is made of.
/// The fraction of a limb segment's own capsule radius that counts as its flesh when the
/// SEGMENT is tested against a volume (IkRig::volumeSegmentRadius): the joint tests
/// count 1.5-2cm of clearance, and the volumes were sized for that; the full radius on top of
/// them read an upper arm laid across the chest as 1cm inside (a capsule chest has no give).
constexpr float kVolumeSegmentFlesh = 0.6f;
/// A segment's row in the SOLVE comes in from each end of the segment over this share of its
/// length (from the 2% where the joints' own rows take over): cut in at full strength there, the
/// cost of a pose jumped as a closest point crossed the line (armatureiksolverows.cpp).
constexpr float kSegmentRowRamp = 0.12f;

/// How a body volume applies to a node (BodyVolume::applies): 0 = not tested. 1 = a body
/// joint, kept out by the solve's one-sided rows, the cursor clamp and the FK stop. 2 = a
/// RIDER of a hand (a finger, a thumb, a carpal: it keeps its local pose and follows the hand
/// rigidly), kept out the same way with kRiderClearance — the row sits on the finger itself,
/// so a hand pressed into the chest stops at its fingertips, not at its wrist.
constexpr char  kVolumeAppliesJoint = 1;
constexpr char  kVolumeAppliesRider = 2;
/// 3 = a solved joint whose SEGMENT is tested too, on a volume that does not test every
/// joint's segment (BodyVolume::segments): the upper arm against the torso — laid against the
/// ribs it is kept out by its own flesh — while the forearm and hand, which sweep across the
/// chest and the breasts, keep their joint tests (their segments held against the chest stopped
/// the cross-body reach: the base rig stepped after the cursor, the male's hand stalled).
constexpr char  kVolumeAppliesJointSegment = 3;
inline bool volumeAppliesJoint(char applies) {
    return applies == kVolumeAppliesJoint || applies == kVolumeAppliesJointSegment;
}
constexpr float kRiderClearance = 0.01f; ///< A finger's flesh (m, figure-scaled by the rig).

struct BodyVolume {
    int               a = -1;
    int               b = -1;
    float             radius = 0.0f;   ///< At the a end (m).
    float             radiusB = -1.0f; ///< At the b end; < 0 = the same as radius (a TAPERED capsule otherwise).
    float             extendA = 0.0f;  ///< The a end pushed this far back along b->a (the pelvis below the hip).
    float             extendB = 0.0f;  ///< The b end pushed this far on along a->b (a skull past the head joint).
    /// The axis ends' OFFSETS from the joints, in the volume's own frame — x along the lateral
    /// reference (refR -> refL), z forward (their cross with the axis), y unused: a chest
    /// capsule runs ahead of the spine, a side capsule beside it. Ignored without references.
    glm::vec3         offA{0.0f};
    glm::vec3         offB{0.0f};
    int               refL = -1;       ///< The lateral reference joints (the hip sockets, the arm roots).
    int               refR = -1;
    std::vector<char> applies;         ///< Per node: kVolumeAppliesJoint / kVolumeAppliesRider / 0.
    /// Whether the tested joints' SEGMENTS are kept out too (see volumeSegmentRadius): the
    /// LIMB capsules — two limbs cross at their middles. Not the torso and the head: their
    /// capsules are coarse, and a hanging forearm brushing the belly in a bent-over stance
    /// or an arm lowered past the head read as centimetres inside, which held the arms off
    /// the body (the all-fours hands never came down).
    bool              segments = false;
};

/// Whether @p v tests the SEGMENT into a joint that applies to it as @p applies.
inline bool volumeAppliesSegment(const BodyVolume& v, char applies) {
    return volumeAppliesJoint(applies) && (v.segments || applies == kVolumeAppliesJointSegment);
}

/// A body volume's axis in a given pose (see volumeAxis): the capsule runs from A to B.
struct VolumeAxis {
    glm::vec3 A{0.0f};
    glm::vec3 B{0.0f};
    glm::vec3 ab{0.0f};
    float     ab2 = 0.0f;
    bool      ok = false; ///< False = the volume's indices do not fit a rig of n nodes.
};

/// The axis of @p v with its joints at @p pos (a callable int -> glm::vec3), for a rig of @p n
/// nodes: the extensions and the frame offsets applied. Every volume test (the solve's rows,
/// the cursor clamp, the FK stop, the harness) reads the capsule through this, so they agree.
template <class PosFn>
inline VolumeAxis volumeAxis(const BodyVolume& v, int n, PosFn&& pos) {
    VolumeAxis ax;
    if (v.a < 0 || v.a >= n || v.b < 0 || v.b >= n || static_cast<int>(v.applies.size()) != n) {
        return ax;
    }
    glm::vec3 A = pos(v.a);
    glm::vec3 B = pos(v.b);
    const glm::vec3 ab0 = B - A;
    const float len = glm::length(ab0);
    const glm::vec3 up = len > 1e-6f ? ab0 / len : glm::vec3(0.0f, 1.0f, 0.0f);
    if (v.extendB > 0.0f) {
        B += up * v.extendB;
    }
    if (v.extendA > 0.0f) {
        A -= up * v.extendA;
    }
    if (v.refL >= 0 && v.refL < n && v.refR >= 0 && v.refR < n &&
        (v.offA != glm::vec3(0.0f) || v.offB != glm::vec3(0.0f))) {
        glm::vec3 lat = pos(v.refL) - pos(v.refR);
        lat -= up * glm::dot(lat, up);
        const float latLen = glm::length(lat);
        if (latLen > 1e-6f) {
            lat /= latLen;
            const glm::vec3 fwd = glm::cross(lat, up);
            A += lat * v.offA.x + fwd * v.offA.z;
            B += lat * v.offB.x + fwd * v.offB.z;
        }
    }
    ax.A = A;
    ax.B = B;
    ax.ab = B - A;
    ax.ab2 = glm::dot(ax.ab, ax.ab);
    ax.ok = true;
    return ax;
}

/// The capsule's radius at parameter @p t along its axis (0 = the A end, 1 = the B end).
/// The parameter (0..1) along axis @p ax of the point of the capsule's axis nearest @p P.
inline float volumeAxisParam(const VolumeAxis& ax, const glm::vec3& P) {
    return ax.ab2 > 1.0e-12f ? glm::clamp(glm::dot(P - ax.A, ax.ab) / ax.ab2, 0.0f, 1.0f) : 0.0f;
}

inline float volumeRadiusAt(const BodyVolume& v, float t) {
    if (v.radiusB < 0.0f) {
        return v.radius;
    }
    const float tc = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return v.radius + (v.radiusB - v.radius) * tc;
}

} // namespace pose

#endif // BODYVOLUME_H

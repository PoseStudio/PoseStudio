/**
 * @file armaturedials.cpp
 * @brief Armature's TRANSFORM DIALS: a joint's rotation read and set as three dials — Bend
 *        Forward, Bend Sideways, Twist — each on a 0-100 scale of its own limit (the Transform tab).
 *
 * A joint's pose is three Euler channels in its own orientation frame, in degrees, between limits
 * that differ from joint to joint and from one figure generation to the next. A dial says the same
 * thing in the joint's own terms: 0 is the rest pose, 100 is as far as the joint goes in its
 * natural direction, and a negative value bends it the other way in the same proportion until the
 * other limit stops it (a knee authored -11..155 degrees reads -7..100).
 *
 * WHICH channel is which dial, and which way is positive, is found by STRUCTURE — the ranges the
 * rig authors and the way the bone lies — never by bone names, so it holds for every figure
 * generation (checked on the eight reference rigs by the harness's `transform-dials` phase, which
 * also gates that the same value on a left and a right joint gives mirrored poses). See
 * Armature::jointDial in armature.h for the rules; the thresholds below are the whole tuning.
 */

#include "armature.h"
#include "ikmath.h" // axisIndexOf
#include "ikrig.h"
#include "rigroles.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace pose {

namespace {

/// An unlimited channel (the hips' own rotation) reads 100 at half a turn.
constexpr float kDialUnlimitedDeg = 180.0f;
/// BEND FORWARD is the wider of the two bend channels — unless the narrower spans this share of
/// the wider or more, where the ranges say nothing (a shoulder's 150 and 130 degrees, a chest's
/// 40 and 50 on the oldest generation) and the channel that swings the limb fore and aft is it.
constexpr float kDialNearEqualShare = 0.75f;
/// A channel whose short side is this share of its long side or less is ONE-SIDED — a fold: its
/// long side is the way the joint bends (a knee's 155 against 11, a collar's 55 against 10).
/// Between the measured cases: a thigh's 35 / 115 and a shoulder's 40 / 110 (0.30-0.36, their
/// long side forward either way) below it; a head's 20 / 40 and a neck's 12 / 27 (0.44-0.50,
/// whose LONG side is backward) above it, which bend forward by the way they lie instead.
constexpr float kDialOneSidedShare = 0.4f;
/// Two sides of a channel within this share of each other are EVEN (a spine joint's side bend).
constexpr float kDialEvenShare = 0.05f;
/// A side bend that moves what the joint carries less than this (of a unit) sideways is read by
/// its up-and-down instead (an arm lying along the shoulders' line is raised and lowered).
constexpr float kDialLateralMin = 0.2f;
/// The limb's direction is read off the joint's heaviest children where they stand this share of
/// the figure's height or more from it (a mid-foot bone sits millimetres from its ankle).
constexpr float kDialLimbMinShare = 0.02f;
/// A bone this share of the figure's height off the sagittal plane is a side's (armature.cpp's
/// mirror pairing reads the same measure).
constexpr float kDialOffPlaneShare = 0.003f;
/// The SWEEP's lever (jointDialSweep) is the rigid segment's length — unless the next joint
/// stands closer than this share of the figure's height, where this share of it stands in.
constexpr float kDialSweepMinShare = 0.04f;
constexpr float kDialSweepDefaultShare = 0.08f;

} // namespace

int Armature::dialJoint(int bone) const {
    if (bone < 0 || bone >= static_cast<int>(m_bones.size())) {
        return -1;
    }
    int joint = bone;
    while (isTwistBone(joint) && m_bones[static_cast<std::size_t>(joint)].parent >= 0) {
        joint = m_bones[static_cast<std::size_t>(joint)].parent;
    }
    return joint;
}

glm::vec3 Armature::limbDirection(int bone) const {
    const std::size_t b = static_cast<std::size_t>(bone);
    const Bone&       src = m_bones[b];
    const auto bindPos = [this](int i) { return -glm::vec3(m_bones[static_cast<std::size_t>(i)].inverseBind[3]); };
    const auto subtreeSize = [this](int root) {
        int              size = 0;
        std::vector<int> stack{root};
        while (!stack.empty()) {
            const int node = stack.back();
            stack.pop_back();
            ++size;
            stack.insert(stack.end(), m_children[static_cast<std::size_t>(node)].begin(),
                         m_children[static_cast<std::size_t>(node)].end());
        }
        return size;
    };

    // The bone's LENGTH AXIS is the format's own statement: the rotation order's first axis — the
    // twist, applied innermost; the STORED order's last letter (lengthAxisLetter) — in the bone's
    // orientation frame. (Read off the joints instead it is wrong wherever a bone's
    // children do not lie along it: the toes fan sideways from the ball, a mid-foot bone sits
    // millimetres from its ankle, the chest's heaviest children are the collars.) What the
    // joints say is only which WAY along it is distal.
    const int       lengthAxis = std::max(0, axisIndexOf(lengthAxisLetter(src)));
    const glm::vec3 axis = glm::normalize(glm::vec3(src.orient[lengthAxis]));
    const glm::vec3 origin = bindPos(bone);

    float lowest = origin.y, highest = origin.y;
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const float y = bindPos(static_cast<int>(i)).y;
        lowest = std::min(lowest, y);
        highest = std::max(highest, y);
    }
    const float minLength = kDialLimbMinShare * std::max(highest - lowest, 1.0e-3f);

    // The HEAD lies the way it stands on the neck: its children are the face rig's, in front of it.
    bool faceBelow = false;
    for (const int c : m_children[b]) {
        faceBelow = faceBelow || (static_cast<std::size_t>(c) < m_boneClass.size() &&
                                  static_cast<BoneClass>(m_boneClass[static_cast<std::size_t>(c)]) == BoneClass::Face);
    }

    // Toward the children that carry the most — a mirrored pair together (the thighs under the
    // pelvis, the collars under the chest: their mean is on the midline) — walking down past
    // children that sit right at the joint.
    float along = 0.0f;
    if (!faceBelow) {
        for (int at = bone; !m_children[static_cast<std::size_t>(at)].empty();) {
            const std::vector<int>& kids = m_children[static_cast<std::size_t>(at)];
            int       best = 0;
            int       first = -1;
            int       count = 0;
            glm::vec3 sum(0.0f);
            for (const int c : kids) {
                const int size = subtreeSize(c);
                if (size > best) {
                    best = size;
                    first = c;
                    count = 0;
                    sum = glm::vec3(0.0f);
                }
                if (size == best) {
                    sum += bindPos(c);
                    ++count;
                }
            }
            const glm::vec3 toward = sum / static_cast<float>(count) - origin;
            if (glm::length(toward) >= minLength) {
                along = glm::dot(axis, glm::normalize(toward));
                break;
            }
            at = first;
        }
    }
    if (std::abs(along) < 0.3f) {
        // No clear answer there (a leaf, the head, children beside the bone's line): everything the
        // joint carries, or for a leaf the way it lies from its parent.
        glm::vec3 sum(0.0f);
        int       count = 0;
        if (!faceBelow) {
            std::vector<int> stack(m_children[b].begin(), m_children[b].end());
            while (!stack.empty()) {
                const int node = stack.back();
                stack.pop_back();
                sum += bindPos(node) - origin;
                ++count;
                stack.insert(stack.end(), m_children[static_cast<std::size_t>(node)].begin(),
                             m_children[static_cast<std::size_t>(node)].end());
            }
        }
        const glm::vec3 toward = count > 0 ? sum / static_cast<float>(count) : glm::vec3(src.localBind[3]);
        if (std::abs(glm::dot(axis, toward)) > 1.0e-6f) {
            along = glm::dot(axis, toward);
        } else if (count == 0 && src.parent >= 0) {
            // A zero-length LEAF at its parent's joint (a rigging add-on's palm bones, each a
            // leaf beside the finger it stands for): nothing of its own says which way is
            // distal, and read as "+axis" the right side's — whose mirrored frame's length axis
            // points proximally — swung the other way for the same dial (4 degrees apart,
            // 2026-11-03). It lies the way its parent's segment lies.
            along = glm::dot(axis, limbDirection(src.parent));
        }
    }
    return along >= 0.0f ? axis : -axis;
}

JointDial Armature::jointDial(int bone, JointDialKind kind) const {
    JointDial dial;
    // A dial reads the joint's ROLE (a finger's Bend is its flexion, below), and the rig that
    // carries the roles is a cache of the bind skeleton (see aliasedBoneIndex): a dial read
    // before the first drag builds it, as the alias layer does, so the same joint never reads
    // one dial without the rig and another with it (2026-11-03; a body mesh arriving later
    // rebuilds it, setBodyMesh).
    if (!m_ikRig && hasSkeleton()) {
        const_cast<Armature*>(this)->prepareIkRig();
    }
    const int joint = dialJoint(bone);
    if (joint < 0) {
        return dial;
    }
    const Bone& b = m_bones[static_cast<std::size_t>(joint)];

    // A channel's limits (an unlimited channel reads half a turn either way), and the dial made
    // of one: positive toward @p sign's limit — or, where that side has no room, the other.
    const auto limits = [this](int which, int axis, float& lo, float& hi) {
        const Bone& bn = m_bones[static_cast<std::size_t>(which)];
        lo = bn.rotLimited[axis] ? bn.rotMin[axis] : -kDialUnlimitedDeg;
        hi = bn.rotLimited[axis] ? bn.rotMax[axis] : kDialUnlimitedDeg;
    };
    const auto make = [&](int which, int axis, float sign) {
        JointDial out;
        float     lo = 0.0f, hi = 0.0f;
        limits(which, axis, lo, hi);
        if (hi - lo < kLockedRangeDeg) {
            return out; // locked: no dial
        }
        float full = sign > 0.0f ? hi : lo;
        if (std::abs(full) < 0.5f * kLockedRangeDeg) {
            full = sign > 0.0f ? lo : hi;
        }
        const float a = lo / full * 100.0f;
        const float c = hi / full * 100.0f;
        out.bone = which;
        out.axis = axis;
        out.fullDeg = full;
        out.minValue = std::min(a, c);
        out.maxValue = std::max(a, c);
        return out;
    };
    // Which side of the body a bone is on (0: the midline), by its bind position.
    const auto sideOf = [this](int which) {
        float lowest = 0.0f, highest = 0.0f;
        for (std::size_t i = 0; i < m_bones.size(); ++i) {
            const float y = -m_bones[i].inverseBind[3].y;
            lowest = i == 0 ? y : std::min(lowest, y);
            highest = i == 0 ? y : std::max(highest, y);
        }
        const float offPlane = kDialOffPlaneShare * std::max(highest - lowest, 1.0e-3f);
        const float x = -m_bones[static_cast<std::size_t>(which)].inverseBind[3].x;
        return x > offPlane ? 1 : (x < -offPlane ? -1 : 0); // +1 = her left (+X), -1 = her right
    };
    // A channel's longer side: +1 / -1, or 0 when the two are even.
    const auto longerSide = [&](int which, int axis) {
        float lo = 0.0f, hi = 0.0f;
        limits(which, axis, lo, hi);
        const float big = std::max(std::abs(lo), std::abs(hi));
        if (std::abs(std::abs(hi) - std::abs(lo)) <= kDialEvenShare * std::max(big, 1.0e-6f)) {
            return 0.0f;
        }
        return std::abs(hi) > std::abs(lo) ? 1.0f : -1.0f;
    };

    const int twistAxis = std::max(0, axisIndexOf(lengthAxisLetter(b)));

    if (kind == JointDialKind::Twist) {
        // The joint's own twist channel — or, where that is locked, its limb's twist bone's (the
        // limb continues through it; one generation hangs leaf twist helpers beside the next joint).
        int   twistBone = joint;
        int   axis = twistAxis;
        float lo = 0.0f, hi = 0.0f;
        limits(joint, twistAxis, lo, hi);
        if (hi - lo < kLockedRangeDeg) {
            twistBone = -1;
            const int main = mainChild(joint);
            if (main >= 0 && isTwistBone(main)) {
                twistBone = main;
            } else {
                for (const int c : m_children[static_cast<std::size_t>(joint)]) {
                    if (isTwistBone(c)) {
                        twistBone = c;
                        break;
                    }
                }
            }
            if (twistBone < 0) {
                return dial;
            }
            const Bone& t = m_bones[static_cast<std::size_t>(twistBone)];
            for (int a = 0; a < 3; ++a) {
                if (!(t.rotLimited[a] && (t.rotMax[a] - t.rotMin[a]) < kLockedRangeDeg)) {
                    axis = a; // (a twist bone has exactly one free channel)
                }
            }
        }
        // The longer side; on an even channel a centre or left bone's positive, mirrored on the
        // right — the sagittal mirror of a pose is (x, -y, -z) per channel (Armature::mirrorPose).
        float sign = longerSide(twistBone, axis);
        if (sign == 0.0f) {
            sign = (sideOf(twistBone) < 0 && axis != 0) ? -1.0f : 1.0f;
        }
        return make(twistBone, axis, sign);
    }

    // The two bend channels, and how a positive turn of each moves what the joint carries: the
    // channel's axis crossed with the way the limb lies (both at bind, where the bone's frame is
    // its orientation).
    const int       o0 = (twistAxis + 1) % 3;
    const int       o1 = (twistAxis + 2) % 3;
    const glm::vec3 limb = limbDirection(joint);
    const auto sweep = [&](int axis) { return glm::cross(glm::normalize(glm::vec3(b.orient[axis])), limb); };
    const auto range = [&](int axis) {
        float lo = 0.0f, hi = 0.0f;
        limits(joint, axis, lo, hi);
        return hi - lo;
    };
    const float r0 = range(o0);
    const float r1 = range(o1);
    int forward = o0;
    if (std::min(r0, r1) >= kDialNearEqualShare * std::max(r0, r1)) {
        forward = std::abs(sweep(o0).z) >= std::abs(sweep(o1).z) ? o0 : o1; // the one that swings fore and aft
    } else {
        forward = r0 >= r1 ? o0 : o1; // the wider one
    }
    const int sideways = forward == o0 ? o1 : o0;

    if (kind == JointDialKind::BendForward) {
        float lo = 0.0f, hi = 0.0f;
        limits(joint, forward, lo, hi);
        const float big = std::max(std::abs(lo), std::abs(hi));
        const float small = std::min(std::abs(lo), std::abs(hi));
        float sign = 1.0f;
        // A FINGER's bend dial is its long side — the flexion, on every finger joint the reference
        // authors (a knuckle -90..50, a thumb's base -26..40): the geometric rule below reads the
        // way a joint carries its limb forward or down, and a finger hanging in an A-pose carries
        // its tip neither — the thumb's reads one way on one rig and the other on the next
        // (2026-11-03: a character-creation rig's thumb Bend dial EXTENDED the thumb once its
        // frame was the palm's). The toes keep the geometric rule: a toe points forward and curls
        // DOWN, which the rule reads, and the reference's toe joints author their long side on
        // either sign.
        const bool finger = m_ikRig && m_ikRig->roles().built() && m_ikRig->roles().role(joint) == RigRole::Finger;
        if (finger || small <= kDialOneSidedShare * big) {
            sign = std::abs(hi) >= std::abs(lo) ? 1.0f : -1.0f; // a fold: its long side
        } else {
            const glm::vec3 v = sweep(forward);
            const bool      even = std::abs(hi) - std::abs(lo) <= kDialEvenShare * std::max(big, 1.0e-6f) &&
                                   std::abs(lo) - std::abs(hi) <= kDialEvenShare * std::max(big, 1.0e-6f); // (the side bend's own test)
            if (even && std::abs(v.x) > std::abs(v.y) && std::abs(v.x) > std::abs(v.z)) {
                // An EVEN channel whose swing is SIDEWAYS (a rib helper's, a palm bone's: the
                // bend dial of a joint that carries nothing fore and aft): outward for a side's
                // bone, toward her left for a centre one — the side-bend rule below. Read as
                // "forward" it was +1 on both sides, and on a rig conformed from an FBX file,
                // whose two sides carry near-identical frames rather than mirror images, the same
                // local sign is an UNMIRRORED turn: the Blender export's rib helpers turned 10
                // degrees apart for the same dial, a rigging add-on's palms 4 (2026-11-03). The
                // reference generations have no such channel, and read as before. EVEN only: a
                // finger's knuckle hanging in an A-pose swings sideways too (its sweep's lateral
                // part a hair over its downward one on the main family, well over on a biped
                // export) and is no even channel (-90..50) — read outward, its Bend dial
                // EXTENDED the finger on every A-pose rig for the day the rule stood unqualified.
                const float outward = sideOf(joint) < 0 ? -v.x : v.x;
                sign = outward >= 0.0f ? 1.0f : -1.0f;
            } else {
                sign = std::abs(v.z) >= std::abs(v.y) ? (v.z >= 0.0f ? 1.0f : -1.0f)  // forward
                                                      : (v.y <= 0.0f ? 1.0f : -1.0f); // ... or down
            }
        }
        return make(joint, forward, sign);
    }

    // Bend Sideways: the longer side; on an even channel OUTWARD for a side's bone, toward her
    // left for a centre one — or, for a limb that a side bend moves up and down, up.
    float sign = longerSide(joint, sideways);
    if (sign == 0.0f) {
        const glm::vec3 v = sweep(sideways);
        if (std::abs(v.x) > kDialLateralMin) {
            const float outward = sideOf(joint) < 0 ? -v.x : v.x;
            sign = outward >= 0.0f ? 1.0f : -1.0f;
        } else {
            sign = v.y >= 0.0f ? 1.0f : -1.0f;
        }
    }
    return make(joint, sideways, sign);
}

float Armature::jointDialValue(const JointDial& dial) const {
    if (!dial.valid() || dial.bone >= static_cast<int>(m_boneEuler.size()) || dial.fullDeg == 0.0f) {
        return 0.0f;
    }
    return m_boneEuler[static_cast<std::size_t>(dial.bone)][dial.axis] / dial.fullDeg * 100.0f;
}

bool Armature::setJointDial(const JointDial& dial, float value) {
    if (!dial.valid() || dial.bone >= static_cast<int>(m_boneEuler.size())) {
        return false;
    }
    const float target = std::clamp(value, dial.minValue, dial.maxValue) / 100.0f * dial.fullDeg;
    const float before = m_boneEuler[static_cast<std::size_t>(dial.bone)][dial.axis];
    if (std::abs(target - before) < 1.0e-5f) {
        return false;
    }
    glm::vec3 delta(0.0f);
    delta[dial.axis] = target - before;
    nudgeBone(dial.bone, delta); // the limits clamp; the body volumes stop it (the FK collision stop)
    return m_boneEuler[static_cast<std::size_t>(dial.bone)][dial.axis] != before;
}

bool Armature::jointDialSweep(const JointDial& dial, JointDialSweep& sweep) const {
    if (!dial.valid() || dial.bone >= static_cast<int>(m_bones.size()) ||
        dial.bone >= static_cast<int>(m_poseGlobal.size()) || dial.bone >= static_cast<int>(m_boneWorldPos.size())) {
        return false;
    }
    const std::size_t b = static_cast<std::size_t>(dial.bone);
    const Bone&       bone = m_bones[b];
    const auto bindPos = [this](int i) { return -glm::vec3(m_bones[static_cast<std::size_t>(i)].inverseBind[3]); };

    // The channel's axis as it stands: the rotations composed OUTSIDE it (the letters before its
    // own in the rotation order, as eulerMatrix composes them) carry it; the bone's orientation
    // frame and its parent's posed rotation put it in the model's space.
    glm::mat3 prefix(1.0f);
    for (const char ch : bone.rotationOrder) {
        const int axis = axisIndexOf(ch);
        if (axis < 0) {
            continue;
        }
        if (axis == dial.axis) {
            break;
        }
        glm::vec3 e(0.0f);
        e[axis] = 1.0f;
        prefix = prefix * glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(m_boneEuler[b][axis]), e));
    }
    glm::vec3 unit(0.0f);
    unit[dial.axis] = 1.0f;
    const glm::mat3 toWorld(m_transform);
    const glm::mat3 parentRot =
        bone.parent >= 0 ? glm::mat3(m_poseGlobal[static_cast<std::size_t>(bone.parent)]) : glm::mat3(1.0f);
    const glm::vec3 axisWorld = toWorld * parentRot * glm::mat3(bone.orient) * prefix * unit;

    // The lever: the way the bone lies (every bind is translation-only, so the bone's posed
    // rotation IS its rotation away from the bind) over its rigid segment's length — to the next
    // real joint past the limb's twist bones, or a share of the figure's height where that joint
    // sits at this one (a mid-foot bone millimetres from its ankle) or there is none (a leaf).
    float lowest = 0.0f, highest = 0.0f;
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const float y = bindPos(static_cast<int>(i)).y;
        lowest = i == 0 ? y : std::min(lowest, y);
        highest = i == 0 ? y : std::max(highest, y);
    }
    const float height = std::max(highest - lowest, 1.0e-3f);
    int farJoint = mainChild(dial.bone);
    while (farJoint >= 0 && isTwistBone(farJoint)) {
        farJoint = mainChild(farJoint);
    }
    float length = farJoint >= 0 ? glm::length(bindPos(farJoint) - bindPos(dial.bone)) : 0.0f;
    if (length < kDialSweepMinShare * height) {
        length = kDialSweepDefaultShare * height;
    }
    const glm::vec3 lever = toWorld * glm::mat3(m_poseGlobal[b]) * limbDirection(dial.bone) * length;

    sweep.tip = m_boneWorldPos[b] + lever;
    sweep.degreesPerUnit = std::abs(dial.fullDeg) / 100.0f;
    // (Toward the dial's positive side: the channel's angle grows with the dial where fullDeg > 0.)
    sweep.tipPerDegree = glm::radians(dial.fullDeg >= 0.0f ? 1.0f : -1.0f) * glm::cross(axisWorld, lever);
    return true;
}

} // namespace pose

// ============================================================================================
// THE FIGURE'S PLACE AND HEADING (2026-11-05; the user: "we have to be able to easily position the
// character (up down left right) and twist the entire character"). Both live in the POSE: the
// place is the root's pose translation — what the figure move (Ctrl+drag of the hips) and the
// Ground button write, what a pose file carries as `@trans:` — and the heading is the root's own
// rotation, which the hips' unlimited channels carry exactly. A turn is applied as a WORLD yaw
// about the vertical through the root joint, so a lying figure spins on the floor rather than
// about her spine, and the feet keep their heights.
// ============================================================================================

namespace pose {

int Armature::figureRoot() const {
    if (m_bones.empty()) {
        return -1;
    }
    if (m_ikRig) {
        return m_ikRig->rootNode();
    }
    // Without a rig: the rig's own rule — the first bone with two children or more below the
    // anatomical root (a figure node at the origin with the hip as its lone child).
    int node = -1;
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        if (m_bones[i].parent < 0) {
            node = static_cast<int>(i);
            break;
        }
    }
    for (int guard = 0; node >= 0 && guard < 64; ++guard) {
        int child = -1, children = 0;
        for (std::size_t i = 0; i < m_bones.size(); ++i) {
            if (m_bones[i].parent == node) {
                ++children;
                child = static_cast<int>(i);
            }
        }
        if (children != 1) {
            return node;
        }
        node = child;
    }
    return node;
}

glm::vec3 Armature::figurePosition() const {
    const int root = figureRoot();
    if (root < 0 || static_cast<std::size_t>(root) >= m_boneTranslation.size()) {
        return glm::vec3(0.0f);
    }
    // The root's parent frame -> model -> world (the inverse of shiftPoseY's path).
    const Bone& bone = m_bones[static_cast<std::size_t>(root)];
    glm::vec3   t = m_boneTranslation[static_cast<std::size_t>(root)];
    if (bone.parent >= 0 && static_cast<std::size_t>(bone.parent) < m_poseGlobal.size()) {
        t = glm::mat3(m_poseGlobal[static_cast<std::size_t>(bone.parent)]) * t;
    }
    return glm::mat3(m_transform) * t;
}

void Armature::setFigurePosition(const glm::vec3& world) {
    const int root = figureRoot();
    if (root < 0 || static_cast<std::size_t>(root) >= m_boneTranslation.size()) {
        return;
    }
    const Bone& bone = m_bones[static_cast<std::size_t>(root)];
    glm::vec3   t = glm::inverse(glm::mat3(m_transform)) * world;
    if (bone.parent >= 0 && static_cast<std::size_t>(bone.parent) < m_poseGlobal.size()) {
        t = glm::transpose(glm::mat3(m_poseGlobal[static_cast<std::size_t>(bone.parent)])) * t;
    }
    if (t == m_boneTranslation[static_cast<std::size_t>(root)]) {
        return;
    }
    m_boneTranslation[static_cast<std::size_t>(root)] = t;
    recomposePoseLocal(static_cast<std::size_t>(root));
    computeSkinMatrices();
}

glm::vec3 Armature::figureRotationDeg() const {
    const int root = figureRoot();
    if (root < 0 || static_cast<std::size_t>(root) >= m_poseGlobal.size()) {
        return glm::vec3(0.0f);
    }
    // The root's world rotation (binds are translation-only, so poseGlobal's rotation part is the
    // pose's), read as yaw about the world's vertical, then pitch about the figure's own lateral
    // axis, then roll about her own forward axis ("YXZ": leftmost outermost). The tilt is shown
    // with her left side DOWN positive, which is a negative roll about +z.
    const glm::mat3 world = glm::mat3(m_transform) * glm::mat3(m_poseGlobal[static_cast<std::size_t>(root)]);
    const glm::vec3 e = eulerFromMatrix(world, "YXZ"); // (e.x pitch, e.y yaw, e.z roll)
    return glm::vec3(e.y, e.x, -e.z);
}

void Armature::setFigureRotationDeg(const glm::vec3& turnTipTilt) {
    const int root = figureRoot();
    if (root < 0 || static_cast<std::size_t>(root) >= m_bones.size()) {
        return;
    }
    const glm::vec3 now = figureRotationDeg();
    if (glm::length(now - turnTipTilt) < 1e-4f) {
        return;
    }
    // The wanted world rotation W' from the three angles; the root's local R' from it: with W =
    // T * P * OR * R * OR^-1 (T the model transform's rotation, P the parent's — the figure node,
    // unrotated), R' = OR^-1 * P^-1 * T^-1 * W' * OR, read back into the root's channels.
    Bone&           bone = m_bones[static_cast<std::size_t>(root)];
    const glm::mat3 orient(bone.orient);
    const glm::mat3 invOrient(bone.invOrient);
    const glm::mat3 target(eulerMatrix(glm::vec3(turnTipTilt.y, turnTipTilt.x, -turnTipTilt.z), "YXZ"));
    glm::mat3       parentRot(1.0f);
    if (bone.parent >= 0 && static_cast<std::size_t>(bone.parent) < m_poseGlobal.size()) {
        parentRot = glm::mat3(m_poseGlobal[static_cast<std::size_t>(bone.parent)]);
    }
    const glm::mat3 next = invOrient * glm::transpose(parentRot) * glm::inverse(glm::mat3(m_transform)) * target * orient;
    m_boneEuler[static_cast<std::size_t>(root)] = eulerFromMatrix(next, bone.rotationOrder);
    clampBoneEuler(root);
    recomposePoseLocal(static_cast<std::size_t>(root));
    computeSkinMatrices();
}

} // namespace pose

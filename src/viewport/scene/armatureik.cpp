/**
 * @file armatureik.cpp
 * @brief Armature's full-body-IK drag LIFECYCLE — the engine side of the FBIK system (scene/ik/):
 *        building the per-figure IkRig and the joint solver's skeleton, beginIkDrag (contacts,
 *        user pins, the grab offset of a promoted grab, the posture reference), dragIkTo (the
 *        model-space target: grab offset, floor and body-volume clamps — then the tick's solve),
 *        settleIkTick / endIkDrag, the FK collision stop's measure, and the user-pin API.
 *
 * The per-tick SOLVE — the drag's tasks and stiffness model handed to scene/ik/jointsolver.h and
 * applied in full, with the rig's per-tick policy around it — is armatureiksolve.cpp and its
 * stage files (armatureiksolvedrag / posture / rows .cpp, armatureikpost.cpp the post-steps);
 * the digit drag is armatureikdigit.cpp, the figure move armatureikfigure.cpp, the landing bounce
 * armaturelanding.cpp. Those TUs are the only place the engine and scene/ik/ meet. Vulkan-free
 * like the rest of the Armature, so the IK harness runs this exact loop.
 */

#include "armature.h"
#include "balancecontroller.h"  // the centre of mass the weight shift is measured from

#include "ikmath.h"
#include "ikrig.h"             // the drag's policy: contacts, pins, volumes, stepping, lift-off
#include "jointsolver.h"       // complete type for the unique_ptr

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace pose {

namespace {

// POSESTUDIO_IK_TRACE=1: the per-drag (beginIkDrag) diagnostics (armatureiksolve.cpp prints the
// per-tick ones).
const bool kIkTrace = std::getenv("POSESTUDIO_IK_TRACE") != nullptr;


// IK_DRAG_STATE_TRACE=1: the DETERMINISM probe — a fingerprint of the pose at every press and
// every tick, so two drags meant to be identical can be compared line by line (the first use
// found a zero Euler composing to the bind only to a rounding: Armature::recomposePoseLocal).
const bool kDragStateTrace = std::getenv("IK_DRAG_STATE_TRACE") != nullptr;

/// A cheap order-sensitive fingerprint of a pose's channels (x + 3y + 7z, summed over bones).
double poseFingerprint(const std::vector<glm::vec3>& perBone) {
    double sum = 0.0;
    for (const glm::vec3& v : perBone) {
        sum += v.x * 1.0 + v.y * 3.0 + v.z * 7.0;
    }
    return sum;
}

/// The bone names flagged in a per-bone mask, space-separated, for a trace line.
void printFlaggedBones(const std::vector<char>& mask, const std::vector<std::string>& names) {
    for (std::size_t i = 0; i < mask.size() && i < names.size(); ++i) {
        if (mask[i]) {
            std::fprintf(stderr, " %s", names[i].c_str());
        }
    }
}

} // namespace

bool Armature::ensureIkRig() {
    if (m_ikRig) {
        return true;
    }
    if (m_bones.empty()) {
        return false;
    }
    {
        // First use on this figure: build the rig (body tree, masses, clearances, body volumes)
        // from the bind skeleton, plus the model-space bind joint positions (the foot contact
        // model finds the ball of a foot by bind height).
        std::vector<IkRigBone> rigBones(m_bones.size());
        std::vector<glm::vec3> bindPos(m_bones.size(), glm::vec3(0.0f));
        for (std::size_t i = 0; i < m_bones.size(); ++i) {
            const Bone& bone = m_bones[i];
            const glm::vec3 parentBind =
                bone.parent >= 0 ? bindPos[static_cast<std::size_t>(bone.parent)] : glm::vec3(0.0f);
            bindPos[i] = parentBind + glm::vec3(bone.localBind[3]);
            IkRigBone& rigBone = rigBones[i];
            rigBone.name = m_boneNames[i];
            rigBone.parent = bone.parent;
            rigBone.bindPos = bindPos[i];
            rigBone.helper = isTwistBone(static_cast<int>(i)); // (a twist helper is no contact: IkRig::helperNode)
        }
        m_ikRig = std::make_unique<IkRig>();
        m_ikRig->build(rigBones, m_bodyMesh.empty() ? nullptr : &m_bodyMesh);
        m_ikBindPos = std::move(bindPos);
        buildJointSolver();
        classifyBones(); // (the face rig and the digits: found with the rig, by structure)
    }
    return true;
}

float Armature::fkVolumeDepth(int rotatedBone) const {
    if (!m_ikRig || rotatedBone < 0 || rotatedBone >= static_cast<int>(m_bones.size())) {
        return 0.0f;
    }
    const int n = static_cast<int>(m_bones.size());
    // The rotated bone's subtree (the solver's O(1) ancestor test), on a buffer kept across calls:
    // the hang's outward bisection asks this of an arm several times a tick.
    std::vector<char>& inSub = m_fkInSubBuf;
    inSub.assign(static_cast<std::size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        inSub[static_cast<std::size_t>(i)] = boneUnder(i, rotatedBone) ? 1 : 0;
    }
    const auto pos = [&](int i) { return glm::vec3(m_poseGlobal[static_cast<std::size_t>(i)][3]); };
    float worst = 0.0f;
    static const bool kDepthTrace = std::getenv("IK_FK_DEPTH_TRACE") != nullptr; // which pair stops an FK turn
    for (const BodyVolume& v : m_ikRig->bodyVolumes()) {
        const VolumeAxis ax = volumeAxis(v, n, pos);
        if (!ax.ok) {
            continue;
        }
        const bool volumeMoves = inSub[static_cast<std::size_t>(v.a)] || inSub[static_cast<std::size_t>(v.b)];
        for (int i = 0; i < n; ++i) {
            const char applies = v.applies[static_cast<std::size_t>(i)];
            if (!applies || (inSub[static_cast<std::size_t>(i)] != 0) == volumeMoves) {
                continue; // no relative motion: this rotation cannot change the pair
            }
            const glm::vec3 P = pos(i);
            const float t = volumeAxisParam(ax, P);
            const float d = glm::length(P - (ax.A + ax.ab * t));
            if (applies == kVolumeAppliesRider) {
                worst = std::max(worst, volumeRadiusAt(v, t) + m_ikRig->riderClearance() - d);
                continue;
            }
            float pair = volumeRadiusAt(v, t) + m_ikRig->volumeClearance(i) - d;
            // The segment into the joint (two limbs cross at their middles).
            const float segR = m_ikRig->volumeSegmentRadius(i);
            const int par = m_bones[static_cast<std::size_t>(i)].parent;
            if (volumeAppliesSegment(v, applies) && segR > 0.0f && par >= 0) {
                float sPar = 0.0f;
                float tPar = 0.0f;
                const float dSeg = closestSegmentPoints(pos(par), P, ax.A, ax.B, sPar, tPar);
                pair = std::max(pair, volumeRadiusAt(v, tPar) + std::max(kVolumeSegmentFlesh * segR, m_ikRig->volumeClearance(i)) - dSeg);
            }
            if (kDepthTrace && pair > 1.0e-3f) {
                std::fprintf(stderr, "[fk-depth] rotated %s: %s inside volume %s-%s by %.1f mm", m_boneNames[static_cast<std::size_t>(rotatedBone)].c_str(),
                             m_boneNames[static_cast<std::size_t>(i)].c_str(), m_boneNames[static_cast<std::size_t>(v.a)].c_str(),
                             m_boneNames[static_cast<std::size_t>(v.b)].c_str(), pair * 1000.0f);
                std::fputc(10, stderr);
            }
            worst = std::max(worst, pair);
        }
    }
    return worst;
}

/// A segment grabbed nearer its far joint than this share of its length drags the FAR joint (the
/// knee for a thigh, the elbow for an upper arm, the hand for a forearm), nearer its near joint
/// the near one. Well short of the middle: what a hand laid on a thigh or an upper arm means to
/// move is the limb, and only a grab right by the socket means the socket (the pelvis, the
/// shoulder); and the far joint's target is the cursor's carried out along the segment, by
/// 1 / share — under a third of the way along that lever turns a twitch into a swing.
constexpr float kSegmentGrabNear = 0.35f;

bool Armature::isTwistBone(int bone) const {
    if (bone < 0 || bone >= static_cast<int>(m_bones.size())) {
        return false;
    }
    // Two locked axes and the free one ALONG the bone: a hinge (an elbow, on the generations
    // that lock its other two channels) has two locked axes too, its free one across the bone.
    const Bone& b = m_bones[static_cast<std::size_t>(bone)];
    int locked = 0;
    int freeAxis = -1;
    for (int a = 0; a < 3; ++a) {
        if (b.rotLimited[a] && (b.rotMax[a] - b.rotMin[a]) < kLockedRangeDeg) {
            ++locked;
        } else {
            freeAxis = a;
        }
    }
    if (locked != 2 || freeAxis < 0) {
        return false;
    }
    glm::vec3 along(0.0f);
    float longest = 0.0f;
    for (const int c : m_children[static_cast<std::size_t>(bone)]) {
        const glm::vec3 offset(m_bones[static_cast<std::size_t>(c)].localBind[3]);
        if (glm::length(offset) > longest) {
            longest = glm::length(offset);
            along = offset;
        }
    }
    if (longest < 1.0e-4f) {
        along = glm::vec3(b.localBind[3]); // (a leaf helper: the way it lies from its parent)
        longest = glm::length(along);
    }
    return longest > 1.0e-4f && std::abs(glm::dot(glm::vec3(b.orient[freeAxis]), along / longest)) > 0.8f;
}

bool Armature::isFoldChannel(int b, int axis) const {
    const Bone& bone = m_bones[static_cast<std::size_t>(b)];
    const float range = bone.rotMax[axis] - bone.rotMin[axis];
    return bone.rotLimited[axis] && range >= kFoldRangeDeg && bone.rotMin[axis] < 0.0f && bone.rotMax[axis] > 0.0f &&
           std::min(-bone.rotMin[axis], bone.rotMax[axis]) <= kFoldNarrowFraction * std::max(-bone.rotMin[axis], bone.rotMax[axis]);
}

int Armature::foldAxisOf(int b) const {
    if (b < 0 || b >= static_cast<int>(m_bones.size())) {
        return -1;
    }
    for (int a = 0; a < 3; ++a) {
        if (isFoldChannel(b, a)) {
            return a;
        }
    }
    return -1;
}

int Armature::freeChannelCount(int b) const {
    const Bone& bone = m_bones[static_cast<std::size_t>(b)];
    int         count = 0;
    for (int a = 0; a < 3; ++a) {
        count += (bone.rotLimited[a] && bone.rotMax[a] - bone.rotMin[a] < kLockedRangeDeg) ? 0 : 1;
    }
    return count;
}

bool Armature::boneUnder(int node, int ancestor) const {
    if (node < 0 || ancestor < 0 || node >= static_cast<int>(m_bones.size())) {
        return false;
    }
    if (m_jointSolver) {
        return m_jointSolver->isAncestorOrSelf(ancestor, node);
    }
    for (int cur = node; cur >= 0; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
        if (cur == ancestor) {
            return true;
        }
    }
    return false;
}

int Armature::armSocketOf(int hand) const {
    if (!m_ikRig || hand < 0 || static_cast<std::size_t>(hand) >= m_bones.size()) {
        return -1;
    }
    const int junction = m_ikRig->limbJunction(hand);
    int cur = hand;
    int below = -1; // the bone below `cur` on the path
    while (cur >= 0 && m_bones[static_cast<std::size_t>(cur)].parent != junction) {
        below = cur;
        cur = m_bones[static_cast<std::size_t>(cur)].parent;
    }
    // (`cur` is the collar — the junction's child; `below` the bone hanging from it: the upper arm)
    return below >= 0 ? below : m_bones[static_cast<std::size_t>(hand)].parent;
}

float Armature::chainLength(int from, int upTo) const {
    float length = 0.0f;
    for (int cur = from; cur >= 0 && cur != upTo; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
        length += glm::length(glm::vec3(m_bones[static_cast<std::size_t>(cur)].localBind[3]));
    }
    return length;
}

int Armature::mainChild(int bone) const {
    if (bone < 0 || bone >= static_cast<int>(m_children.size())) {
        return -1;
    }
    // The child with the biggest subtree (children follow parents in the bone order).
    int best = -1;
    int bestSize = 0;
    for (const int c : m_children[static_cast<std::size_t>(bone)]) {
        int size = 0;
        std::vector<int> stack{c};
        while (!stack.empty()) {
            const int node = stack.back();
            stack.pop_back();
            ++size;
            stack.insert(stack.end(), m_children[static_cast<std::size_t>(node)].begin(),
                         m_children[static_cast<std::size_t>(node)].end());
        }
        if (size > bestSize) {
            bestSize = size;
            best = c;
        }
    }
    return best;
}

bool Armature::sameRigidSegment(int a, int b) const {
    if (a == b) {
        return a >= 0;
    }
    int nearA = -1, farA = -1, nearB = -1, farB = -1;
    return rigidSegment(a, nearA, farA) && rigidSegment(b, nearB, farB) && nearA == nearB && farA == farB;
}

bool Armature::rigidSegment(int bone, int& nearJoint, int& farJoint) const {
    nearJoint = bone;
    while (isTwistBone(nearJoint) && m_bones[static_cast<std::size_t>(nearJoint)].parent >= 0) {
        nearJoint = m_bones[static_cast<std::size_t>(nearJoint)].parent;
    }
    farJoint = mainChild(bone);
    while (farJoint >= 0 && isTwistBone(farJoint)) {
        farJoint = mainChild(farJoint);
    }
    if (farJoint < 0 && nearJoint != bone) {
        // (A LEAF twist helper beside the limb's next joint, as one generation hangs them: the
        // segment is its parent's.)
        for (int next = mainChild(nearJoint); next >= 0; next = mainChild(next)) {
            if (!isTwistBone(next)) {
                farJoint = next;
                break;
            }
        }
    }
    return farJoint >= 0 && farJoint != nearJoint;
}

bool Armature::footLyingExempt(int node) const {
    if (node < 0 || static_cast<std::size_t>(node) >= m_bones.size() || m_jsFootFlat.size() != m_bones.size() ||
        m_jsBall.size() != m_bones.size()) {
        return false;
    }
    for (std::size_t f = 0; f < m_bones.size(); ++f) {
        if (m_jsFootFlat[f] <= 0.5f || m_jsBall[f] < 0) {
            continue;
        }
        // (the chain from the ball's parent up to the foot bone, inclusive)
        for (int cur = m_bones[static_cast<std::size_t>(m_jsBall[f])].parent; cur >= 0; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            if (cur == node) {
                return true;
            }
            if (cur == static_cast<int>(f)) {
                break;
            }
        }
    }
    return false;
}

void Armature::setIkGrabPoint(int bone, const glm::vec3& worldPoint) {
    bone = posingBone(bone); // (a point of the face is a point of the HEAD: the face rig is not posed)
    if (bone < 0 || bone >= static_cast<int>(m_bones.size())) {
        m_ikGrabBone = -1;
        return;
    }
    m_ikGrabBone = bone;
    m_ikGrabLocal = glm::vec3(glm::inverse(m_transform * m_poseGlobal[static_cast<std::size_t>(bone)]) *
                              glm::vec4(worldPoint, 1.0f));
}

void Armature::setIkGrabOnSegment(int bone, float share) {
    int nearJoint = -1;
    int farJoint = -1;
    if (bone < 0 || bone >= static_cast<int>(m_bones.size()) || !rigidSegment(bone, nearJoint, farJoint)) {
        m_ikGrabBone = -1;
        return;
    }
    const glm::vec3 from(m_poseGlobal[static_cast<std::size_t>(nearJoint)][3]);
    const glm::vec3 to(m_poseGlobal[static_cast<std::size_t>(farJoint)][3]);
    const glm::vec3 point = glm::mix(from, to, glm::clamp(share, 0.0f, 1.0f));
    m_ikGrabBone = bone;
    m_ikGrabLocal = glm::vec3(glm::inverse(m_poseGlobal[static_cast<std::size_t>(bone)]) * glm::vec4(point, 1.0f));
}

glm::vec3 Armature::ikGrabPointWorld() const {
    if (m_ikGrabBone >= 0 && m_ikGrabBone == m_selectedBone && m_ikGrabBone < static_cast<int>(m_bones.size())) {
        return glm::vec3(m_transform * m_poseGlobal[static_cast<std::size_t>(m_ikGrabBone)] * glm::vec4(m_ikGrabLocal, 1.0f));
    }
    if (m_selectedBone >= 0 && m_selectedBone < static_cast<int>(m_boneWorldPos.size())) {
        return m_boneWorldPos[static_cast<std::size_t>(m_selectedBone)];
    }
    return glm::vec3(0.0f);
}

// --- THE GROUP (see armature.h) ------------------------------------------------------------

bool Armature::inGroup(int bone) const {
    return std::find(m_groupBones.begin(), m_groupBones.end(), bone) != m_groupBones.end();
}

void Armature::clearGroup() {
    if (!m_groupBones.empty()) {
        m_groupBones.clear();
        ++m_groupVersion;
    }
}

bool Armature::toggleGroupBone(int bone) {
    bone = posingBone(bone); // (a point of the face is the HEAD's)
    if (bone < 0 || bone >= static_cast<int>(m_bones.size()) || bone == m_selectedBone ||
        boneClass(bone) == BoneClass::Digit) {
        return false;
    }
    const auto it = std::find(m_groupBones.begin(), m_groupBones.end(), bone);
    if (it != m_groupBones.end()) {
        m_groupBones.erase(it);
        ++m_groupVersion;
        return false;
    }
    m_groupBones.push_back(bone);
    ++m_groupVersion;
    return true;
}

bool Armature::makeGroupPrimary(int bone) {
    bone = posingBone(bone);
    if (bone < 0 || bone >= static_cast<int>(m_bones.size()) || boneClass(bone) == BoneClass::Digit) {
        return false;
    }
    if (bone == m_selectedBone) {
        return true;
    }
    const auto it = std::find(m_groupBones.begin(), m_groupBones.end(), bone);
    if (it != m_groupBones.end()) {
        m_groupBones.erase(it);
    }
    if (m_selectedBone >= 0 && m_selectedBone < static_cast<int>(m_bones.size()) && !inGroup(m_selectedBone)) {
        m_groupBones.insert(m_groupBones.begin(), m_selectedBone);
    }
    m_selectedBone = bone;
    m_ikGrabBone = -1;
    ++m_groupVersion;
    return true;
}

bool Armature::removeFromGroup(int bone) {
    bone = posingBone(bone);
    if (bone >= 0 && bone == m_selectedBone) {
        m_selectedBone = m_groupBones.empty() ? -1 : m_groupBones.front();
        if (!m_groupBones.empty()) {
            m_groupBones.erase(m_groupBones.begin());
        }
        m_ikGrabBone = -1;
        ++m_groupVersion;
        return true;
    }
    const auto it = std::find(m_groupBones.begin(), m_groupBones.end(), bone);
    if (it == m_groupBones.end()) {
        return false;
    }
    m_groupBones.erase(it);
    ++m_groupVersion;
    return true;
}

/// beginIkDrag's FIRST reset, before the grab offset is captured: the drag's seat and kneel
/// readings (constants of the drag), the contacts' and the flat-foot model's per-drag state.
void Armature::resetContactState() {
    m_jsFloorSeatStart = m_ikRig->floorSeat(); // (a constant of the drag: never a mode read off the pose it changes)
    m_jsSeatNode = m_ikRig->floorSeatNode();
    m_jsKneelStart = m_ikRig->onKnees();
    m_jsKneelSeatY = m_jsKneelStart ? m_ikRig->kneelSeatHeight() : -1.0f;
    m_jsKneelSeat = m_jsKneelStart ? m_ikRig->kneelSeat() : glm::vec3(0.0f, -1.0f, 0.0f);
    m_jsHandTipValid.assign(m_bones.size(), 0);
    m_jsContactGone.assign(m_bones.size(), 0.0f);
    m_jsContactRiseBase.assign(m_bones.size(), -1.0f);
    m_jsContactRebased.assign(m_bones.size(), 0);
    m_jsHandUnload.assign(m_bones.size(), 0.0f);
    // A KNEELING FOOT LIES FLAT: the share persists across drags (a kneel let go of is still a
    // kneel); the drag-local flags reset. (Whether a foot IS flat is re-read at the first tick from
    // the pins and the pose: a Reset Pose leaves no planted knee, and the share goes at once.)
    if (m_jsFootFlat.size() != m_bones.size()) {
        m_jsFootFlat.assign(m_bones.size(), 0.0f);
        m_jsFootFlatAxis.assign(m_bones.size(), -2);
        m_jsFootFlatLimit.assign(m_bones.size(), 0.0f);
        m_jsFootFlatTuck.assign(m_bones.size(), 0.0f);
        m_jsFootFlatRef.assign(m_bones.size(), glm::vec3(0.0f));
        m_jsFlatBallOffset.assign(m_bones.size(), glm::vec3(0.0f));
        m_jsFlatHeelOffset.assign(m_bones.size(), glm::vec3(0.0f));
        m_jsFootKnee.assign(m_bones.size(), -1);
        m_jsFlatLyingY.assign(m_bones.size(), 0.0f);
        m_jsFlatRollStartY.assign(m_bones.size(), 0.0f);
        m_jsFlatRollStartRef.assign(m_bones.size(), 0.0f);
        m_jsToeFlatRef.assign(m_bones.size(), glm::vec3(0.0f));
    }
    m_jsToeFlatValid.assign(m_bones.size(), 0);
    m_jsToeFlatShare.assign(m_bones.size(), 0.0f);
    m_jsFootFlatRefValid.assign(m_bones.size(), 0);
    m_jsFootFlatSeen.assign(m_bones.size(), 0);
    m_jsFootWasFlat.assign(m_bones.size(), 0);
    m_jsFootFlatAtStart.assign(m_bones.size(), 0);
    m_jsFootUnrolled.assign(m_bones.size(), 0);
    m_jsBallLanded.assign(m_bones.size(), 0);
    m_jsFootUnrollProgress.assign(m_bones.size(), 1.0f);
    m_jsKneeHeldDown.assign(m_bones.size(), 0);
    m_jsKneeHold.assign(m_bones.size(), 1.0f);
    m_jsKneeUnrollBaseY.assign(m_bones.size(), 0.0f);
    m_jsKneeUnrollBaseValid.assign(m_bones.size(), 0);
    m_jsKneeUnrollExtra.assign(m_bones.size(), 0.0f);
    m_jsKneeHeldDownPrev.assign(m_bones.size(), 0);
    m_jsFlatRiseBase.assign(m_bones.size(), 0.0f);
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        m_jsFootWasFlat[i] = m_jsFootFlatAtStart[i] = m_jsFootFlat[i] > 0.5f ? 1 : 0;
    }
    m_jsContactUnloaded.assign(m_bones.size(), 0);
    m_jsContactLanding.assign(m_bones.size(), 0);
    m_jsKneelTilt = 1.0;
    m_jsSeatRolls = false;
}

/// THE POSTURE REFERENCE and the drag-start readings the solve measures from: this moment's pose,
/// each pin's held orientation, every stage's per-drag state reset, the root's upright Euler and
/// heading, the balance ramp.
void Armature::captureDragStart(const std::vector<glm::vec3>& positions, float groundOffsetY) {
    // The POSTURE REFERENCE of the drag's solve — this moment's pose (every channel is pulled
    // back toward it, which is what makes the solved pose a function of the targets) — and each
    // pin's world orientation for its hold (see m_ikFlatNodes in armature.h).
    m_ikStartEuler = m_boneEuler;
    m_jsStartTranslation = m_boneTranslation;
    captureFlatHold(false);
    m_jsPinTargetValid.assign(m_bones.size(), 0);
    m_jsPinsEasing = false;
    m_jsSettling = false;
    m_jsHoldValid = false;
    m_jsFollowValid = false;
    m_jsRootYield = 0.0f;
    m_jsReachDown = 0.0f;
    m_jsReachHinge = 0.0f;
    m_jsReachTravel = 0.0f;
    m_jsRootRise = 0.0f;
    m_jsContactRise = 0.0f;
    m_jsKneelYield = 0.0f;
    m_jsKneelHold = 0.0f;
    m_jsKneelSeatShift = glm::vec3(0.0f);
    m_jsKneelUpright = 0.0f;
    m_jsKneelUpRise = 0.0f;
    m_jsLowestTargetY = 1.0e9f;
    m_jsRiseProgress = 0.0f;
    m_jsTrunkHomeShare = 0.0f;
    {
        const std::size_t root = static_cast<std::size_t>(m_ikRig->rootNode());
        m_jsRootRiseRoom = (m_ikBindPos[root].y + groundOffsetY) - positions[root].y;
        // The root UPRIGHT: its world rotation now, reduced to its heading (read off whichever
        // of its lateral and fore axes lies flatter — a root pitched 90 degrees has no fore
        // heading, one rolled 90 no lateral), as an Euler pose on this rotation's own branch.
        const Bone& bone = m_bones[root];
        const glm::mat3 world(m_poseGlobal[root]);
        glm::vec3 lateral = world * glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 fore = world * glm::vec3(0.0f, 0.0f, 1.0f);
        lateral.y = 0.0f;
        fore.y = 0.0f;
        const float yaw = glm::length(lateral) >= glm::length(fore) ? std::atan2(-lateral.z, lateral.x)
                                                                     : std::atan2(fore.x, fore.z);
        const glm::mat3 upright(glm::mat4_cast(glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f))));
        m_jsRootHeading = yaw;
        m_jsStartGrabFromRoot = positions[static_cast<std::size_t>(m_ikRig->dragEffector())] - positions[root];
        m_jsEffectorStartRot = glm::mat3(m_poseGlobal[static_cast<std::size_t>(m_ikRig->dragEffector())]);
        m_jsPrevGoalValid = false;
        m_jsTrunkInSolve = false;
        m_jsHingeRelease = 0.0f;
        m_jsTrunkFollow = glm::vec2(0.0f);
        m_jsUnfoldShift = glm::vec2(0.0f);
        m_jsTrunkWalk = 0.0f;
        m_jsTrunkCounter = 0.0f;
        m_jsWasSuspended = false;
        m_jsWasSwinging = false;
        m_jsLimbSocket = -1;
        m_jsTrunkHinge = 0.0f;
        m_jsHangArms.clear();
        m_jsHangArmsFound = false;
        m_jsPostStepEuler.assign(m_bones.size(), glm::vec3(0.0f));
        m_jsPostStepTurned.assign(m_bones.size(), 0);
        m_jsHeadChain.clear();
        m_jsHeadFree.clear();
        m_jsHeadBase = -1;
        m_jsHeadFound = false;
        m_jsLiftEasing = false;
        m_jsLiftDeficit = glm::vec3(0.0f);
        m_jsHangDeficit = glm::vec3(0.0f);
        m_jsLeanMoment = -1.0f;
        m_jsStartPos.resize(m_bones.size());
        m_jsStartRot.resize(m_bones.size());
        for (std::size_t i = 0; i < m_bones.size(); ++i) {
            m_jsStartPos[i] = glm::vec3(m_poseGlobal[i][3]);
            m_jsStartRot[i] = glm::mat3(m_poseGlobal[i]);
        }
        m_jsEffectorStartPos = glm::vec3(m_poseGlobal[static_cast<std::size_t>(m_ikRig->dragEffector())][3]);
        const glm::mat3 parentRot = bone.parent >= 0 ? glm::mat3(m_poseGlobal[static_cast<std::size_t>(bone.parent)])
                                                     : glm::mat3(1.0f);
        const glm::mat3 local = glm::mat3(bone.invOrient) * glm::transpose(parentRot) * upright * glm::mat3(bone.orient);
        m_jsRootUprightEuler = eulerFromMatrix(local, bone.rotationOrder);
        for (int a = 0; a < 3; ++a) {
            while (m_jsRootUprightEuler[a] - m_boneEuler[root][a] > 180.0f) {
                m_jsRootUprightEuler[a] -= 360.0f;
            }
            while (m_jsRootUprightEuler[a] - m_boneEuler[root][a] < -180.0f) {
                m_jsRootUprightEuler[a] += 360.0f;
            }
        }
    }
    m_jsHullKey = -1.0f;
    m_jsLastSupport.clear();
    m_jsGrowFrom.clear();
    m_jsGrown = 0.0f;
    m_jsBalanceSlack = 0.0f;
    m_jsBalanceBase = -1.0f;
}

/// THE WEIGHT SHIFT (ikPostureModel): a foot lifted off a TWO-FOOTED stance takes the hips over
/// the standing foot. Read here, once, as a constant of the drag: from the centre of mass as the
/// drag found it to the standing foot's footprint centre — for a drag that begins on exactly two
/// planted feet, one of which the drag can lift (the dragged foot itself, or the foot under a
/// dragged knee), and nothing else on the floor: a kneeling figure's knee drawn up stands on the
/// other KNEE, a body with a hand on the floor is propped, a seated one is held up by its seat.
void Armature::findWeightShift() {
    m_jsWeightShift = glm::vec2(0.0f);
    m_jsWeightLiftStart = -1.0f;
    m_jsWeightLift = 0.0f;
    m_jsWeightEased = glm::vec2(0.0f);
    m_jsWeightEasing = false;
    const IkRig& rig = *m_ikRig;
    const int    effector = rig.dragEffector();
    const bool effectorIsFoot = effector != rig.rootNode() && rig.isFootClass(effector);
    int  standing = -1;
    int  standingCount = 0;
    int  liftable = effectorIsFoot ? 1 : 0;
    bool propped = rig.onKnees() || rig.floorSeat() || rig.seatPin() >= 0 || rig.suspended();
    for (std::size_t p = 0; p < rig.pins().size() && !propped; ++p) {
        const int node = rig.pins()[p].node;
        if (node < 0) {
            continue;
        }
        if (rig.pinIsLive(p) || !rig.isFootClass(node)) {
            propped = true; // a knee or a hand on the floor, a pinned hand: not a two-footed stance
        } else if (rig.pinUnderEffector(p)) {
            ++liftable; // the foot under a dragged knee
        } else {
            standing = static_cast<int>(p);
            ++standingCount;
        }
    }
    if (!propped && standingCount == 1 && liftable == 1) {
        const std::vector<glm::vec2> footprint = rig.pinFootprintPoints(static_cast<std::size_t>(standing));
        glm::vec2 centre(rig.pins()[static_cast<std::size_t>(standing)].target.x,
                         rig.pins()[static_cast<std::size_t>(standing)].target.z);
        if (!footprint.empty()) {
            // The MIDFOOT: halfway from the ankle to the footprint's centroid. A body on one
            // leg stands over its midfoot, between the heel and the ball; the footprint's
            // points are mostly the toes', and on a rig whose toes splay outward the
            // centroid alone sat 4cm outside the ankle.
            glm::vec2 centroid(0.0f);
            for (const glm::vec2& v : footprint) {
                centroid += v;
            }
            centroid /= static_cast<float>(footprint.size());
            centre = 0.5f * (centre + centroid);
        }
        std::vector<int> parents(m_bones.size());
        for (std::size_t i = 0; i < m_bones.size(); ++i) {
            parents[i] = m_bones[i].parent;
        }
        const glm::vec3 com = BalanceController::centerOfMass(m_jsStartPos, parents, rig.masses());
        m_jsWeightShift = centre - glm::vec2(com.x, com.z);
        static const bool kWeightTrace = std::getenv("IK_JS_WEIGHT_TRACE") != nullptr;
        if (kWeightTrace) {
            const IkEffector& pin = rig.pins()[static_cast<std::size_t>(standing)];
            std::fprintf(stderr, "[weight] standing %s target(%.4f %.4f %.4f) footprint %zu centre(%.4f %.4f) com(%.4f %.4f %.4f) shift(%.4f %.4f)\n",
                         m_boneNames[static_cast<std::size_t>(pin.node)].c_str(), pin.target.x, pin.target.y, pin.target.z,
                         footprint.size(), centre.x, centre.y, com.x, com.y, com.z, m_jsWeightShift.x, m_jsWeightShift.y);
            for (const glm::vec2& v : footprint) {
                std::fprintf(stderr, "[weight]   footprint point (%.4f %.4f)\n", v.x, v.y);
            }
        }
    }
}

/// The foot model's and the hand's per-drag state (the balls, the toe mates, the eased targets, the
/// hand slide and lay), reset for the drag.
void Armature::resetFootAndHandState() {
    m_jsBall.assign(m_bones.size(), -2);
    m_jsToeMates.assign(m_bones.size(), {});
    m_jsBallTargetValid.assign(m_bones.size(), 0);
    m_jsKneeUnderValid = 0;
    m_jsLyingHangYValid = 0;
    m_jsFootAllowValid = 0;
    m_jsTargetDown = 0.0f;
    m_jsProneLie = 0.0f;
    m_jsAkimboBone = -1;
    m_jsAkimboShare = 0.0f;
    m_jsAkimboEase = 0.0f;
    m_jsDangle = 0.0f;
    m_jsHandSlideEase = 0.0f;
    m_jsHandLayValid = false;
    m_jsHandLayEase = 0.0f;
    m_jsRawTargetValid = false;
}

/// THE TIPTOE (dragIkTick): a hand pulled up beyond its reach lifts the heels. Read here, once,
/// whether this drag is one that can: a LIMB's joint — not the root, not a trunk joint, not a
/// foot's — dragged from a stance of exactly two planted feet with nothing else on the floor:
/// no live contact (a hand or a knee down is a propped body), no user pin (a pinned foot
/// cannot pitch, a pinned hand holds the body), not on her knees or her seat, not suspended.
void Armature::findTiptoeDrag() {
    m_jsTiptoeDrag = false;
    m_jsTiptoeRoom = -1.0f;
    m_jsTiptoeD = 0.0f;
    m_jsTiptoeH0 = 0.0f;
    m_jsTiptoe = 0.0f;
    m_jsTiptoeTheta = 0.0f;
    const IkRig& rig = *m_ikRig;
    const int    effector = rig.dragEffector();
    bool can = effector >= 0 && effector != rig.rootNode() && !rig.isFootClass(effector) && !rig.effectorIsTrunk() &&
               !(rig.onKnees() || rig.floorSeat() || rig.seatPin() >= 0 || rig.suspended());
    int standing = 0;
    for (std::size_t p = 0; p < rig.pins().size() && can; ++p) {
        const int node = rig.pins()[p].node;
        if (node < 0) {
            continue;
        }
        if (rig.pinIsLive(p) || rig.pinIsUser(p) || !rig.isFootClass(node) || rig.pinUnderEffector(p)) {
            can = false;
        } else {
            ++standing;
        }
    }
    m_jsTiptoeDrag = can && standing == 2;
}

bool Armature::beginIkDrag(IkScope scope) {
    m_ikScope = IkScope::Body; // (set once the rig has the drag: a failed begin leaves no scope behind)
    if (m_selectedBone < 0 || m_selectedBone >= static_cast<int>(m_bones.size()) ||
        m_bones.empty()) {
        return false;
    }
    if (!ensureIkRig()) {
        return false;
    }
    // A finger's or a toe's joint: THE DIGIT DRAG (armatureikdigit.cpp) — the digit alone answers
    // the cursor, whatever the scope; the rig begins no drag, so nothing is planted, stepped or
    // balanced, and no other bone is an unknown.
    m_digitDrag = false;
    m_figureMove = false;
    if (boneClass(m_selectedBone) == BoneClass::Digit) {
        return beginDigitDrag();
    }
    // IkScope::Part (the app's Ctrl+drag) is resolved by what the rig's drag turns out to have hold
    // of, below: the HIPS — the solve root, which a grab of the pelvis girdle is promoted to — ->
    // the figure is moved as she is posed (Figure); anything else, a spine joint included since
    // 2026-10-28 (CTRL ON THE TORSO, below) -> the grabbed chain alone (Chain).
    const bool part = scope == IkScope::Part;
    if (part) {
        scope = IkScope::Chain;
    }
    // Current model-space joint positions, plus the ground contacts (detected by WORLD height —
    // the model transform may have grounded/translated the figure). Thresholds scale with the
    // figure's size (a child's standing ankle sits proportionally lower).
    const float sizeScale = m_ikRig->sizeScale();
    std::vector<glm::vec3> positions(m_bones.size());
    std::vector<int> contacts;
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        positions[i] = glm::vec3(m_poseGlobal[i][3]);
        if (m_boneWorldPos[i].y < IkRig::kContactHeight * sizeScale) { // the feet and toes standing, a knee kneeling, a hand on the floor
            contacts.push_back(static_cast<int>(i));
        }
    }
    if (contacts.empty()) {
        // Nothing under the threshold (residuals left the figure hovering, or it was posed
        // airborne): anchor at whatever is LOWEST — combined with the rig's ground-healing pin
        // heights, a slightly-floating figure is pulled back onto the floor by its next drag
        // instead of drifting further with each one.
        float minY = 1e30f;
        for (std::size_t i = 0; i < m_bones.size(); ++i) {
            minY = std::min(minY, m_boneWorldPos[i].y);
        }
        for (std::size_t i = 0; i < m_bones.size(); ++i) {
            if (m_boneWorldPos[i].y < minY + 0.06f * sizeScale) {
                contacts.push_back(static_cast<int>(i));
            }
        }
    }
    m_ikSettleTicks = 0;
    m_ikGrabOffset = glm::vec3(0.0f); // reset BEFORE the rig call — a failed begin must not
                                      // leave the previous drag's promotion offset behind
    // The rig solves in MODEL space; the world floor (y = 0) sits at model height -ty once the
    // transform translates the figure vertically (the Ground button). Every bind-height ground
    // reference in the rig shifts by this, or pins would heal to a floor that no longer matches
    // the visible one. (The transform is translation-only today — nothing rotates models.)
    const float groundOffsetY = -m_transform[3][1];
    m_ikInvTransform = glm::inverse(m_transform); // fixed for the drag (see armature.h)
    std::vector<int> userPins;
    for (std::size_t i = 0; i < m_bonePinned.size() && i < m_bones.size(); ++i) {
        if (m_bonePinned[i]) {
            userPins.push_back(static_cast<int>(i));
        }
    }
    // WHAT the drag takes hold of (see setIkGrabPoint): the nearest real joint of the rigid
    // segment the grab point lies on. (No grab point: the selected joint itself, as ever.)
    int grabNode = m_selectedBone;
    m_ikSegmentNear = m_ikSegmentFar = -1;
    m_ikSegmentShare = 0.0f;
    m_ikGrabFarJoint = false;
    // (A TWIST bone's own joint is a point of its segment's body too — mid-thigh, mid-forearm —
    // however it was picked: dragged as a joint in its own right, a mid-thigh twist bone rested
    // 5-15cm short of its cursor, the planted foot below it holding the knee it has no say over.)
    if (isTwistBone(m_selectedBone) && !(m_ikGrabBone == m_selectedBone && glm::length(m_ikGrabLocal) > 1.0e-4f)) {
        m_ikGrabBone = m_selectedBone;
        m_ikGrabLocal = glm::vec3(0.0f);
    }
    const bool grabbedBody = m_ikGrabBone == m_selectedBone &&
                             (glm::length(m_ikGrabLocal) > 1.0e-4f || isTwistBone(m_selectedBone));
    // (The HEAD is no segment: what hangs below it is the face rig, whose joints are not the IK's —
    // a point of the head or the face is dragged by the head's own joint, its offset measured. Nor
    // is the HAND or the FOOT, 2026-10-28: what hangs below is its carpals and digits — a point of
    // the palm is dragged by the hand's own joint, its offset measured; ending at a carpal, the
    // "segment" had a palm click near that end drag the carpal by dilation.)
    const bool segment = grabbedBody && rigidSegment(m_selectedBone, m_ikSegmentNear, m_ikSegmentFar) &&
                         boneClass(m_ikSegmentFar) == BoneClass::Body;
    if (!segment) {
        m_ikSegmentNear = m_ikSegmentFar = -1;
    }
    if (segment) {
        const glm::vec3 point(m_poseGlobal[static_cast<std::size_t>(m_selectedBone)] * glm::vec4(m_ikGrabLocal, 1.0f));
        const glm::vec3& from = positions[static_cast<std::size_t>(m_ikSegmentNear)];
        const glm::vec3 along = positions[static_cast<std::size_t>(m_ikSegmentFar)] - from;
        const float length2 = glm::dot(along, along);
        m_ikSegmentShare = length2 > 1.0e-8f ? glm::clamp(glm::dot(point - from, along) / length2, 0.0f, 1.0f) : 0.0f;
        m_ikGrabFarJoint = m_ikSegmentShare >= kSegmentGrabNear;
        grabNode = m_ikGrabFarJoint ? m_ikSegmentFar : m_ikSegmentNear;
    }
    syncRigRotations(); // (the floor clearances are read along DOWN in each bone's frame)
    // (A limb the user takes hold of is the user's from here on: its contacts are no accidents.)
    if (m_incidentalContact.size() != m_bones.size()) {
        m_incidentalContact.assign(m_bones.size(), 0);
    }
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        if (!m_incidentalContact[i]) {
            continue;
        }
        const int junction = m_ikRig->limbJunction(static_cast<int>(i));
        for (int cur = grabNode; cur >= 0 && m_incidentalContact[i]; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            if (cur == junction) {
                break;
            }
            for (int up = static_cast<int>(i); up >= 0 && up != junction; up = m_bones[static_cast<std::size_t>(up)].parent) {
                if (up == cur) {
                    m_incidentalContact[i] = 0;
                    break;
                }
            }
        }
    }
    m_ikRig->setIncidentalContacts(m_incidentalContact);
    if (kDragStateTrace) {
        std::fprintf(stderr, "[drag-state] incidental:");
        printFlaggedBones(m_incidentalContact, m_boneNames);
        std::fprintf(stderr, " | hangArms %zu found %d | postStepTurned:", m_jsHangArms.size(), m_jsHangArmsFound ? 1 : 0);
        printFlaggedBones(m_jsPostStepTurned, m_boneNames);
        std::fputc(10, stderr);
        std::fprintf(stderr, "[drag-state] pose checksum %.9f trans %.9f transform y %.6f grab %d far %d seg %d/%d",
                     poseFingerprint(m_boneEuler), poseFingerprint(m_boneTranslation), m_transform[3][1], grabNode,
                     m_ikGrabFarJoint ? 1 : 0, m_ikSegmentNear, m_ikSegmentFar);
        std::fputc(10, stderr);
    }
    // THE GROUP (see toggleGroupBone): the secondaries go to the rig with the primary — their own
    // contacts lifted, their limbs active and priced as the dragged one — for a whole-body drag
    // only: a Ctrl+drag's scoped chain or figure move, and the landing bounce's own pelvis drag,
    // are the primary's alone.
    std::vector<int> secondaries;
    if (scope == IkScope::Body) {
        for (const int member : m_groupBones) {
            if (member >= 0 && member < static_cast<int>(m_bones.size()) && member != grabNode &&
                boneClass(member) == BoneClass::Body) {
                secondaries.push_back(member);
            }
        }
    }
    // (The rig reads the scope BEFORE the drag begins: under a SCOPED trunk drag the user's pins
    // below the grabbed joint HOLD — IkRig::beginDrag's pin rule — where a plain trunk drag leaves
    // them out. A failed begin, and the figure move's endDrag, clear it.)
    m_ikRig->setScoped(scope == IkScope::Chain); // (no intent, no suspension, no step under a scoped drag)
    if (!m_ikRig->beginDrag(grabNode, positions, contacts, groundOffsetY, &userPins,
                            secondaries.empty() ? nullptr : &secondaries)) {
        m_ikRig->setScoped(false);
        return false;
    }
    static const bool kNoFigureMove = std::getenv("POSESTUDIO_NO_FIGURE_MOVE") != nullptr; // A/B probe: the scoped chain, as before 2026-09-28
    // CTRL ON THE TORSO (2026-10-28; POSESTUDIO_CTRL_TRUNK_MOVES is the A/B — the figure move for
    // every grab of the body, as from 2026-09-28): the figure is MOVED by the HIPS alone — the solve
    // root, which a grab of the pelvis girdle is promoted to; a spine joint under Ctrl bends the
    // TRUNK ALONE, the scoped chain from the grabbed joint down to, not including, the root — the
    // pelvis and the legs stay, the arms and the head ride (the user: "ctrl+click dragging the
    // upper chest should continue to IK pose through the torso, but stop before the legs").
    static const bool kTrunkMoves = std::getenv("POSESTUDIO_CTRL_TRUNK_MOVES") != nullptr;
    const int  dragEffector = m_ikRig->dragEffector();
    const bool bodyMove = kTrunkMoves ? isBodyGrab(dragEffector) : dragEffector == m_ikRig->rootNode();
    if (part && !kNoFigureMove && bodyMove) {
        m_ikRig->endDrag(); // (the rig has no part in a figure move: nothing is planted)
        return beginFigureMove();
    }
    m_ikScope = scope;
    {
        // THE GROUP's drag state (updateGroupTargets): where each secondary effector stood at the
        // press, and the primary's grab point — the cursor's displacement is measured from it.
        const std::vector<int>& secs = m_ikRig->secondaryEffectors();
        m_jsGroupStartPos.resize(secs.size());
        for (std::size_t i = 0; i < secs.size(); ++i) {
            m_jsGroupStartPos[i] = positions[static_cast<std::size_t>(secs[i])];
        }
        m_jsGroupTargets.assign(secs.size(), glm::vec3(0.0f));
        m_jsGroupTargetValid.assign(secs.size(), 0);
        m_jsGroupCursorStart = glm::vec3(m_ikInvTransform * glm::vec4(ikGrabPointWorld(), 1.0f));
        m_jsGroupCursorValid = false;
        m_jsGroupFollowValid = false;
        if (kIkTrace && !secs.empty()) {
            std::fprintf(stderr, "[ik] group: %zu secondaries:", secs.size());
            for (const int sec : secs) {
                std::fprintf(stderr, " %s", m_boneNames[static_cast<std::size_t>(sec)].c_str());
            }
            std::fputc(10, stderr);
        }
    }
    if (kDragStateTrace) { // (the pins the rig planted: node, target, kind)
        const std::vector<IkEffector>& pins = m_ikRig->pins();
        for (std::size_t p = 0; p < pins.size(); ++p) {
            const int node = pins[p].node;
            std::fprintf(stderr, "[drag-state] pin %s target(%.4f %.4f %.4f) live=%d user=%d hard=%d effector=%s", node >= 0 ? m_boneNames[static_cast<std::size_t>(node)].c_str() : "?",
                         pins[p].target.x, pins[p].target.y, pins[p].target.z, m_ikRig->pinIsLive(p) ? 1 : 0, m_ikRig->pinIsUser(p) ? 1 : 0,
                         pins[p].hard ? 1 : 0, m_boneNames[static_cast<std::size_t>(m_ikRig->dragEffector())].c_str());
            std::fputc(10, stderr);
        }
    }
    resetContactState();
    // (The far joint stands only if the rig kept it: a toe or a fingertip it promotes onward is
    // handled by the measured offset below like any other promotion.)
    // (... and only for a LIMB's joint: a trunk segment does not pivot on its near joint, the
    // body goes along — a chest asked 1 / share as far as the cursor went overshoots by as much,
    // and the next tick asks it back. A trunk joint keeps the fixed offset below.)
    m_ikGrabFarJoint = m_ikGrabFarJoint && m_ikRig->dragEffector() == m_ikSegmentFar && !m_ikRig->effectorIsTrunk();
    // Token-mass grabs (a finger, a toe, a face bone) are PROMOTED by the rig to the limb's
    // first real-mass joint (the hand, the foot, the head) — a finger pull is an arm gesture.
    // The drag targets from the window track the GRABBED joint, so they are shifted by the
    // model-space grab offset before reaching the rig — captured here, then carried through the
    // effector's rotation since drag start on every tick for LIMB effectors (see dragIkTo: a
    // constant offset misses a twisting hand's fingertip by up to twice its length) and held
    // fixed for a trunk effector.
    m_ikGrabRotStart = glm::mat3(m_poseGlobal[static_cast<std::size_t>(m_ikRig->dragEffector())]);
    if (m_ikRig->dragEffector() != m_selectedBone) {
        m_ikGrabOffset =
            glm::vec3(m_poseGlobal[static_cast<std::size_t>(m_selectedBone)][3]) -
            glm::vec3(m_poseGlobal[static_cast<std::size_t>(m_ikRig->dragEffector())][3]);
    }
    // A grabbed BODY point: its offset from the effector is read off the pose on every tick (it
    // is rigid with the grabbed bone, which turns as the limb or the spine does; the loop this
    // closes has a gain of the offset over the lever that turns it — a fifth at the most).
    m_ikGrabMeasured = grabbedBody;
    // ... and so is a small bone's JOINT promoted to a TRUNK joint: a pectoral bone, which is what a
    // click on a figure's chest finds as often as the chest itself (its joint stands in front of the
    // spine, nearest the pixel), or a face bone on the head. Held FIXED, the offset did not turn with
    // a chest that LEANS as it is dragged: pulled 18cm sideways by a pectoral, the grabbed point
    // ended 40-47mm from the cursor and trailed it by 32-44mm all the way, on every rig (by the
    // chest itself: 0.04 and 3); measured, 18 and 9 (what is left is the lean's: a point beside the
    // spine drops as the chest rolls, and a standing body cannot be asked up). The fixed offset was
    // the old position-space solver's, which passed cursor noise on fourteen-fold and trembled a
    // grabbed head through this loop; here an eye grab rests 0.95mm from its cursor where it rested
    // 9.9, trails it by 1.3mm where it trailed 9.7, and under +-2mm of cursor noise wobbles 15mm
    // where it wobbled 14. IK_GRAB_TRUNK_FIXED / IK_GRAB_HEAD_FIXED are the A/B levers.
    static const bool kTrunkFixed = std::getenv("IK_GRAB_TRUNK_FIXED") != nullptr;
    static const bool kHeadFixed = std::getenv("IK_GRAB_HEAD_FIXED") != nullptr;
    if (!kTrunkFixed && !grabbedBody && m_ikRig->dragEffector() != m_selectedBone && m_ikRig->effectorIsTrunk() &&
        !m_ikRig->girdleGrab() && !(kHeadFixed && m_ikRig->dragEffector() == m_ikRig->headNode())) {
        m_ikGrabBone = m_selectedBone;
        m_ikGrabLocal = glm::vec3(0.0f);
        m_ikGrabMeasured = true;
    }
    m_ikSegmentPrevValid = false;
    m_ikSegmentTranslating = 0.0f;
    if (kIkTrace) {
        std::fprintf(stderr, "[ik] beginDrag effector=%s solved=%s pins:",
                     m_boneNames[static_cast<std::size_t>(m_selectedBone)].c_str(),
                     m_boneNames[static_cast<std::size_t>(m_ikRig->dragEffector())].c_str());
        for (const IkEffector& pin : m_ikRig->pins()) {
            std::fprintf(stderr, " %s(y=%.3f w=%.2f)", m_boneNames[static_cast<std::size_t>(pin.node)].c_str(), pin.target.y, pin.weight);
        }
        std::fprintf(stderr, " contacts=%zu\n", contacts.size());
        std::fflush(stderr);
    }
    captureDragStart(positions, groundOffsetY);
    findWeightShift();
    resetFootAndHandState();
    findTiptoeDrag();
    if (m_ikScope == IkScope::Chain) {
        // A SCOPED drag moves the chain alone: no weight shift under a lifted foot, no tiptoe.
        m_jsWeightShift = glm::vec2(0.0f);
        m_jsTiptoeDrag = false;
    }
    m_jsSoleEaseValid = false;
    m_jsSoleEase = 1.0f;
    m_jsBallOffset.assign(m_bones.size(), glm::vec3(0.0f));
    return true;
}

bool Armature::dragIkTo(const glm::vec3& targetWorld) {
    if (m_digitDrag) {
        return dragDigitTo(targetWorld); // (a finger's or a toe's joint: the digit alone)
    }
    if (m_figureMove) {
        return dragFigureTo(targetWorld); // (Ctrl + the body itself: the figure, moved as she is posed)
    }
    if (!m_ikRig || !m_ikRig->dragActive() || m_bones.empty()) {
        return false;
    }
    if (kDragStateTrace) {
        std::fprintf(stderr, "[drag-tick] target(%.6f %.6f %.6f) pose %.9f", targetWorld.x, targetWorld.y, targetWorld.z,
                     poseFingerprint(m_boneEuler));
        std::fputc(10, stderr);
    }
    // The solve works in model space. The grab offset re-targets a promoted drag (finger ->
    // hand etc., see beginIkDrag).
    // The promoted grab's offset rides the effector's rotation since drag start — for LIMB
    // effectors. A finger on a twisting hand needs it (a constant offset misses the
    // fingertip). A small bone promoted to a TRUNK joint — a pectoral to the chest, a face bone to
    // the head — has its offset MEASURED off the pose every tick instead (m_ikGrabMeasured, set in
    // beginIkDrag; the fixed offset kept here is only the IK_GRAB_TRUNK_FIXED / _HEAD_FIXED probes').
    const glm::mat3 grabRot =
        m_ikRig->effectorIsTrunk()
            ? glm::mat3(1.0f)
            : glm::mat3(m_poseGlobal[static_cast<std::size_t>(m_ikRig->dragEffector())]) *
                  glm::transpose(m_ikGrabRotStart);
    glm::vec3 grabOffset = grabRot * m_ikGrabOffset;
    const glm::vec3 cursor(m_ikInvTransform * glm::vec4(targetWorld, 1.0f));
    m_jsGroupCursorRaw = cursor; // (THE GROUP's secondaries ride its displacement: updateGroupTargets)
    m_jsGroupCursorValid = true;
    bool mapped = false;
    glm::vec3 mappedTarget(0.0f);
    if (m_ikGrabFarJoint && m_ikSegmentNear >= 0 && m_ikSegmentFar == m_ikRig->dragEffector() &&
        m_selectedBone >= 0 && m_selectedBone < static_cast<int>(m_bones.size())) {
        // The grab point lies on a rigid segment, `share` of the way from its near joint to the
        // far one the solve drags: the far joint's target is the cursor's place carried OUT from
        // the near joint by 1 / share — a dilation about the socket. With the far joint on that
        // target the point is on the cursor exactly; and a cursor that leaves the point's own
        // sphere (it drags in a plane: a thigh pulled up in a side view is a target INSIDE it)
        // becomes a far-joint target as far, in proportion, inside or outside ITS sphere — so
        // what the knee's or the elbow's own drag does with such a target (the height kept, the
        // swing found for it) it does for the grabbed point too: a knee taken to the cursor's
        // height / share above the socket puts the point at the cursor's height. (Exact for a
        // near joint that stays put — a socket mostly does — and read off the pose afresh every
        // tick for one that does not.)
        // ... as far as the near joint STAYS PUT. Let r be the share of the far joint's travel
        // that is the whole limb translating (the near joint going along: the body following an
        // out-of-reach pull, a figure hanging from the grabbed arm). Per tick the dilation leaves
        // -r (1 - share) / share of the point's miss — nothing at r = 0, a growing oscillation
        // once the body goes with the limb (a forearm grabbed a third of the way along and pulled
        // up until she hangs from it: -1.9 a tick) — and the plain measured offset (the far
        // joint asked to the cursor plus where it now stands from the point) leaves
        // (1 - r)(1 - share): exact at r = 1, slow at r = 0 and blind to the proportion above.
        // Blended by lambda = share (1 - r) / (share (1 - r) + r) the two cancel; r is read off
        // the two joints' travel over the last tick, smoothed.
        const glm::vec3 from(m_poseGlobal[static_cast<std::size_t>(m_ikSegmentNear)][3]);
        const glm::vec3 farPos(m_poseGlobal[static_cast<std::size_t>(m_ikSegmentFar)][3]);
        const glm::vec3 point(m_poseGlobal[static_cast<std::size_t>(m_selectedBone)] * glm::vec4(m_ikGrabLocal, 1.0f));
        if (m_ikSegmentPrevValid) {
            const float farWent = glm::length(farPos - m_ikSegmentPrevFar);
            if (farWent > 1.0e-3f) {
                const float along = glm::clamp(glm::length(from - m_ikSegmentPrevNear) / farWent, 0.0f, 1.0f);
                m_ikSegmentTranslating = glm::mix(m_ikSegmentTranslating, along, 0.5f);
            }
        }
        m_ikSegmentPrevNear = from;
        m_ikSegmentPrevFar = farPos;
        m_ikSegmentPrevValid = true;
        const float share = m_ikSegmentShare; // (>= kSegmentGrabNear: that is what made this a far-joint grab)
        const float swings = share * (1.0f - m_ikSegmentTranslating);
        const float trust = swings / std::max(swings + m_ikSegmentTranslating, 1.0e-4f);
        // (The dilation is for a cursor ON or INSIDE the point's sphere. Outside it the excess
        // passes on as it is, not 1 / share as large: a forearm grabbed 40% of the way along and
        // raised 90cm asked the hand 2.25m up, and she was lifted off the floor by it.)
        const glm::vec3 reach = cursor - from;
        const float out = glm::length(reach);
        const float length = glm::length(farPos - from);
        glm::vec3 dilated = from + reach / share;
        if (out > share * length && out > 1.0e-5f) {
            dilated = from + reach * ((length + out - share * length) / out);
        }
        mappedTarget = glm::mix(cursor + (farPos - point), dilated, trust);
        mapped = true;
    } else if (m_ikGrabMeasured && m_selectedBone >= 0 && m_selectedBone < static_cast<int>(m_bones.size())) {
        grabOffset = glm::vec3(m_poseGlobal[static_cast<std::size_t>(m_selectedBone)] * glm::vec4(m_ikGrabLocal, 1.0f)) -
                     glm::vec3(m_poseGlobal[static_cast<std::size_t>(m_ikRig->dragEffector())][3]);
    } else if (m_ikRig->girdleGrab() && m_selectedBone >= 0 && m_selectedBone < static_cast<int>(m_bones.size())) {
        // A joint of the pelvis girdle promoted to the root (IkRig::girdleGrab): the offset is
        // MEASURED, on the pose as it stands. The root's rotation is one of a pelvis drag's
        // unknowns (a sway rolls it, a kneel pitches it 40 degrees) and the pelvis bone and the
        // first spine bone turn under one, so no frame captured at the press carries the offset;
        // measured, the grabbed joint is on the cursor exactly wherever the pose rests (the
        // offset's own dependence on the target is a few percent: a lever of 2-15cm).
        grabOffset = glm::vec3(m_poseGlobal[static_cast<std::size_t>(m_selectedBone)][3]) -
                     glm::vec3(m_poseGlobal[static_cast<std::size_t>(m_ikRig->dragEffector())][3]);
    }
    glm::vec3 target = mapped ? mappedTarget : cursor - grabOffset;
    if (kDragStateTrace) {
        const glm::vec3 eff(m_poseGlobal[static_cast<std::size_t>(m_ikRig->dragEffector())][3]);
        std::fprintf(stderr, "[drag-map] cursor(%.4f %.4f %.4f) offset(%.4f %.4f %.4f) target(%.4f %.4f %.4f) effector(%.4f %.4f %.4f) measured=%d mapped=%d",
                     cursor.x, cursor.y, cursor.z, grabOffset.x, grabOffset.y, grabOffset.z, target.x, target.y, target.z, eff.x, eff.y, eff.z,
                     m_ikGrabMeasured ? 1 : 0, mapped ? 1 : 0);
        std::fputc(10, stderr);
    }
    // The floor bounds the cursor too: the grabbed joint cannot be asked below its clearance.
    target.y = std::max(target.y, -m_transform[3][1] +
                                      m_ikRig->floorClearance(m_ikRig->dragEffector()));
    // The body volumes bound it too (see IkRig::bodyVolumes): a cursor inside the torso asks
    // for the hand at the chest's surface, not through it. (The hand lay reads the target as it
    // was before: a cursor inside the body is a hand on it.)
    m_jsRawTarget = target;
    m_jsRawTargetValid = true;
    if (!m_ikRig->bodyVolumes().empty()) {
        std::vector<glm::vec3> current(m_bones.size());
        for (std::size_t i = 0; i < m_bones.size(); ++i) {
            current[i] = glm::vec3(m_poseGlobal[i][3]);
        }
        target = m_ikRig->clampOutOfVolumes(m_ikRig->dragEffector(), target, current);
    }
    return dragIkTick(target); // the solve: converged and applied in full (armatureiksolve.cpp)
}

bool Armature::settleIkTick() {
    if (m_digitDrag || m_figureMove) {
        return false; // (nothing to land: neither has a contact pin, and the pose is the solve's)
    }
    if (!m_ikRig || !m_ikRig->dragActive() || m_bones.empty() || m_ikRig->pins().empty()) {
        return false;
    }
    return settleIkSolveTick();
}

void Armature::syncRigRotations() {
    if (!m_ikRig) {
        return;
    }
    std::vector<glm::mat3>& rotations = m_jsRotationsBuf; // (kept across ticks: no allocation)
    rotations.resize(m_bones.size());
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        rotations[i] = glm::mat3(m_poseGlobal[i]);
    }
    m_ikRig->setBoneRotations(rotations);
}

void Armature::endIkDrag() {
    if (m_ikRig) {
        m_ikRig->endDrag();
    }
    m_ikScope = IkScope::Body;
    m_digitDrag = false;
    m_figureMove = false;
}

bool Armature::togglePinSelectedBone() {
    if (m_selectedBone < 0 || m_selectedBone >= static_cast<int>(m_bones.size())) {
        return false;
    }
    if (m_bonePinned.size() != m_bones.size()) {
        m_bonePinned.assign(m_bones.size(), 0);
    }
    char& flag = m_bonePinned[static_cast<std::size_t>(m_selectedBone)];
    flag = flag ? 0 : 1;
    return flag != 0;
}

bool Armature::hasPinnedBones() const {
    for (const char p : m_bonePinned) {
        if (p) {
            return true;
        }
    }
    return false;
}

void Armature::unpinAllBones() {
    std::fill(m_bonePinned.begin(), m_bonePinned.end(), 0);
}

std::vector<int> Armature::activeContactPins() const {
    std::vector<int> out;
    if (m_ikRig && m_ikRig->dragActive() && !m_landing.active) { // (a landing bounce is no drag of the user's: no markers)
        const std::vector<IkEffector>& pins = m_ikRig->pins();
        for (std::size_t p = 0; p < pins.size(); ++p) {
            if (!m_ikRig->pinIsUser(p)) {
                out.push_back(pins[p].node);
            }
        }
    }
    return out;
}

void Armature::captureFlatHold(bool restOrientation) {
    m_ikFlatNodes.clear();
    m_ikFlatRot.clear();
    m_ikFlatOwner.clear();
    if (!m_ikRig) {
        return;
    }
    const std::vector<IkEffector>& pins = m_ikRig->pins();
    for (std::size_t p = 0; p < pins.size(); ++p) {
        const int node = pins[p].node;
        if (node < 0 || node >= static_cast<int>(m_bones.size())) {
            continue;
        }
        if (m_ikRig->pinIsLive(p)) {
            continue; // a floor contact the pose made holds its place, not its orientation —
                      // exactly like one the drag brings down itself (IkRig::seedPoseContacts)
        }
        const bool user = m_ikRig->pinIsUser(p);
        // The rest pose's world rotation is the identity: a bone's bind is translation-only
        // (its orientation cancels at rest), so "flat on the floor" is the identity frame.
        const auto rotOf = [&](int b) {
            return (restOrientation && !user) ? glm::mat3(1.0f)
                                              : glm::mat3(m_poseGlobal[static_cast<std::size_t>(b)]);
        };
        m_ikFlatNodes.push_back(node);
        m_ikFlatRot.push_back(rotOf(node));
        m_ikFlatOwner.push_back(node);
        if (user) {
            continue; // a user pin's subtree keeps its own local pose (the pin is held 6-DoF)
        }
        // A contact's riders — its whole subtree, the toes — hold their world orientation
        // too (see m_ikFlatOwner): the sole may pitch, the toes stay flat on the floor.
        std::vector<int> stack(m_children[static_cast<std::size_t>(node)].begin(),
                               m_children[static_cast<std::size_t>(node)].end());
        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
            m_ikFlatNodes.push_back(c);
            m_ikFlatRot.push_back(rotOf(c));
            m_ikFlatOwner.push_back(node);
            stack.insert(stack.end(), m_children[static_cast<std::size_t>(c)].begin(),
                         m_children[static_cast<std::size_t>(c)].end());
        }
    }
}

} // namespace pose

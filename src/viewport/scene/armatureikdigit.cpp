/**
 * @file armatureikdigit.cpp
 * @brief The BONE CLASSES — what each bone is to the posing UI (BoneClass: the body's joints, the
 *        face rig, the digits) — and THE DIGIT DRAG: a finger or a toe dragged moves its own digit
 *        alone, up to where it joins the hand or the foot.
 *
 * The face rig's bones are for expressions: nothing selects one (a click on the face selects the
 * HEAD, Armature::posingBone) and none is ever an unknown of a solve. A digit's joint IS selectable,
 * and its drag is a solve of its own — the digit's few channels against one cursor row, the
 * authored limits and the floor — that the IK rig has no part in: nothing is planted, balanced or
 * stepped, and no bone outside the digit moves. (Until 2026-09-28 a grab of either was PROMOTED to
 * the limb's real end joint — a finger pull was an arm gesture, an eye grab a head drag — see
 * IkRig::beginDrag, whose promotion stays for the small bones that are neither: a pectoral on the
 * chest.) A CARPAL — what is below a wrist or an ankle and is neither the limb end nor a digit:
 * the carpals, a hand's prop anchor, the mid-foot, the ball, the heel — is the hand's or the foot's
 * own flesh (2026-10-28): a click on one selects the HAND or the FOOT, as a click on the face
 * selects the head, and it lights with it. (Until then it was a Body bone: selectable, the hand's
 * flesh split among four carpals and a click on the palm as often a carpal's as the hand's — "the
 * hand node can be difficult to select, and posing carpals in IK mode is not practical".) Qt-free
 * (std + GLM).
 */

#include "armature.h"
#include "armatureiksolvetuning.h"

#include "ikrig.h"
#include "jointsolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pose {

void Armature::classifyBones() {
    const std::size_t n = m_bones.size();
    m_boneClass.assign(n, static_cast<char>(BoneClass::Body));
    static const bool kAllBody = std::getenv("POSESTUDIO_POSE_ALL_BONES") != nullptr; // A/B probe
    if (!m_ikRig || kAllBody) {
        return;
    }
    const IkRig& rig = *m_ikRig;
    const int    head = rig.headNode();
    const int    root = rig.rootNode();
    ensureHandMaps();
    const float footHeight = kDigitFootHeight * rig.sizeScale();
    // (Parents precede children: one pass carries "below the head", "of the body", "foot-class"
    // and the LIMB END a bone hangs below — its wrist, or its ankle: the foot's most proximal
    // foot-class joint — with that hand's or foot's REACH, its farthest joint at bind.)
    std::vector<char>  underHead(n, 0);
    std::vector<char>  ofBody(n, 0);
    std::vector<char>  footClass(n, 0);
    std::vector<int>   limbEnd(n, -1);
    std::vector<float> reach(n, 0.0f);
    int faces = 0;
    int digits = 0;
    int carpals = 0;
    for (std::size_t b = 0; b < n; ++b) {
        const int  p = m_bones[b].parent;
        const bool hasParent = p >= 0 && static_cast<std::size_t>(p) < b;
        underHead[b] = hasParent && (p == head || underHead[static_cast<std::size_t>(p)]);
        ofBody[b] = static_cast<int>(b) == root || (hasParent && ofBody[static_cast<std::size_t>(p)]);
        footClass[b] = ofBody[b] && static_cast<int>(b) != root && b < m_ikBindPos.size() && m_ikBindPos[b].y < footHeight;
        if (head >= 0 && underHead[b]) {
            m_boneClass[b] = static_cast<char>(BoneClass::Face);
            ++faces;
            continue;
        }
        if (m_jsWristOf.size() == n && m_jsWristOf[b] >= 0 && m_jsWristOf[b] != static_cast<int>(b)) {
            limbEnd[b] = m_jsWristOf[b];
        } else if (footClass[b] && hasParent && footClass[static_cast<std::size_t>(p)]) {
            limbEnd[b] = limbEnd[static_cast<std::size_t>(p)] >= 0 ? limbEnd[static_cast<std::size_t>(p)] : p;
        }
        if (limbEnd[b] >= 0) {
            const std::size_t end = static_cast<std::size_t>(limbEnd[b]);
            reach[end] = std::max(reach[end], glm::length(m_ikBindPos[b] - m_ikBindPos[end]));
        }
    }
    // A DIGIT's joint, children first (a bone that carries more than one digit is no digit):
    //   - it BENDS: its widest LIMITED channel spans kDigitMinRangeDeg or more (a carpal's spans
    //     7-20; a helper bone that turns freely or full circle on every channel — a hand's prop
    //     anchor, authored -180..180 — is no joint);
    //   - it stands OUT in the hand or the foot: at least kDigitMinReachShare of the hand's or
    //     the foot's reach from the wrist or the ankle (a thumb's base stands 0.15-0.17 of it out;
    //     a heel bone or a mid-foot bone at the ankle 0.02-0.05, with 45-60 degrees of range);
    //   - it carries ONE digit at most: the bone the toes fan from (the ball of the foot, 100
    //     degrees of range and five children) is the foot's, as a carpal with two fingers is the
    //     hand's — a grab there is the limb's.
    for (std::size_t k = n; k-- > 0;) {
        if (limbEnd[k] < 0 || m_boneClass[k] != static_cast<char>(BoneClass::Body)) {
            continue;
        }
        const Bone&       bone = m_bones[k];
        const std::size_t end = static_cast<std::size_t>(limbEnd[k]);
        float             widest = 0.0f;
        for (int a = 0; a < 3; ++a) {
            const float range = bone.rotMax[a] - bone.rotMin[a];
            widest = std::max(widest, bone.rotLimited[a] && range < static_cast<float>(kFreeRangeDeg) ? range : 0.0f);
        }
        int carried = 0;
        for (const int c : m_children[k]) {
            carried += m_boneClass[static_cast<std::size_t>(c)] == static_cast<char>(BoneClass::Digit) ? 1 : 0;
        }
        if (widest >= kDigitMinRangeDeg && carried <= 1 &&
            glm::length(m_ikBindPos[k] - m_ikBindPos[end]) >= kDigitMinReachShare * reach[end]) {
            m_boneClass[k] = static_cast<char>(BoneClass::Digit);
            ++digits;
        }
    }
    // A CARPAL: what is left below a wrist or an ankle — the carpals, a hand's prop anchor, the
    // mid-foot, the ball, the heel — is the hand's or the foot's own flesh, not a joint to pose: a
    // click on one selects the limb end (posingBone), and it lights with it.
    m_boneLimbEnd = limbEnd;
    for (std::size_t b = 0; b < n; ++b) {
        if (limbEnd[b] >= 0 && m_boneClass[b] == static_cast<char>(BoneClass::Body)) {
            m_boneClass[b] = static_cast<char>(BoneClass::Carpal);
            ++carpals;
        }
    }
    static const bool kTrace = std::getenv("POSESTUDIO_BONE_CLASS_TRACE") != nullptr;
    if (kTrace) {
        std::fprintf(stderr, "[bone-class] head %s: %d face-rig bones; %d digit joints:",
                     head >= 0 ? m_boneNames[static_cast<std::size_t>(head)].c_str() : "(none)", faces, digits);
        for (std::size_t b = 0; b < n; ++b) {
            if (m_boneClass[b] == static_cast<char>(BoneClass::Digit)) {
                std::fprintf(stderr, " %s", m_boneNames[b].c_str());
            }
        }
        std::fputc(10, stderr);
        // (... and what below a wrist or an ankle is NOT a digit: the hand's and the foot's own —
        // the CARPALS, each selecting its limb end.)
        std::fprintf(stderr, "[bone-class] %d carpals, of the hands and feet themselves:", carpals);
        for (std::size_t b = 0; b < n; ++b) {
            if (m_boneClass[b] == static_cast<char>(BoneClass::Carpal)) {
                std::fprintf(stderr, " %s->%s", m_boneNames[b].c_str(),
                             limbEnd[b] >= 0 ? m_boneNames[static_cast<std::size_t>(limbEnd[b])].c_str() : "?");
            }
        }
        std::fputc(10, stderr);
    }
}

BoneClass Armature::boneClass(int bone) {
    if (bone < 0 || bone >= static_cast<int>(m_bones.size()) || !ensureIkRig()) {
        return BoneClass::Body;
    }
    if (m_boneClass.size() != m_bones.size()) {
        classifyBones();
    }
    return static_cast<BoneClass>(m_boneClass[static_cast<std::size_t>(bone)]);
}

int Armature::posingBone(int bone) {
    const BoneClass cls = boneClass(bone);
    if (cls == BoneClass::Face) {
        return m_ikRig->headNode();
    }
    if (cls == BoneClass::Carpal && static_cast<std::size_t>(bone) < m_boneLimbEnd.size() && m_boneLimbEnd[static_cast<std::size_t>(bone)] >= 0) {
        return m_boneLimbEnd[static_cast<std::size_t>(bone)]; // (the hand, the foot)
    }
    return bone;
}

int Armature::knownPosingBone(int bone) const {
    if (!m_ikRig || bone < 0 || static_cast<std::size_t>(bone) >= m_boneClass.size()) {
        return bone;
    }
    const char cls = m_boneClass[static_cast<std::size_t>(bone)];
    if (cls == static_cast<char>(BoneClass::Face)) {
        return m_ikRig->headNode();
    }
    if (cls == static_cast<char>(BoneClass::Carpal) && static_cast<std::size_t>(bone) < m_boneLimbEnd.size() &&
        m_boneLimbEnd[static_cast<std::size_t>(bone)] >= 0) {
        return m_boneLimbEnd[static_cast<std::size_t>(bone)];
    }
    return bone;
}

bool Armature::highlightedWithSelection(int bone) const {
    if (bone < 0) {
        return false;
    }
    // THE GROUP's secondaries light with the selection, each with its highlight twin (a bend bone's
    // twist child and vice versa: the whole upper arm for a grabbed shoulder) — and a secondary
    // head lights its face rig as the selected head does.
    for (const int member : m_groupBones) {
        if (member == bone || (member >= 0 && static_cast<std::size_t>(member) < m_highlightTwin.size() &&
                               m_highlightTwin[static_cast<std::size_t>(member)] == bone)) {
            return true;
        }
    }
    if (!m_ikRig || static_cast<std::size_t>(bone) >= m_boneClass.size()) {
        return false;
    }
    // A CARPAL lights with its hand or foot (the palm is the carpals' flesh).
    if (m_boneClass[static_cast<std::size_t>(bone)] == static_cast<char>(BoneClass::Carpal)) {
        const int end = static_cast<std::size_t>(bone) < m_boneLimbEnd.size() ? m_boneLimbEnd[static_cast<std::size_t>(bone)] : -1;
        return end >= 0 && ((highlightBone() >= 0 && highlightBone() == end) || inGroup(end));
    }
    if (m_boneClass[static_cast<std::size_t>(bone)] != static_cast<char>(BoneClass::Face)) {
        return false;
    }
    if (highlightBone() >= 0 && highlightBone() == m_ikRig->headNode()) {
        return true;
    }
    return inGroup(m_ikRig->headNode());
}

void Armature::localChannelDofs(int b, std::vector<JointSolverDof>& out) const {
    const Bone& bone = m_bones[static_cast<std::size_t>(b)];
    for (int a = 0; a < 3; ++a) {
        const float range = bone.rotMax[a] - bone.rotMin[a];
        if (bone.rotLimited[a] && range < kLockedRangeDeg) {
            continue;
        }
        const double rangeDeg = bone.rotLimited[a] ? std::max(static_cast<double>(range), kMinRangeDeg) : kFreeRangeDeg;
        const double rangeRad = rangeDeg * 3.14159265358979323846 / 180.0;
        JointSolverDof dof;
        dof.bone = b;
        dof.axis = a;
        dof.stiffness = kLimbStiffness / (rangeRad * rangeRad);
        dof.reference = static_cast<double>(m_ikStartEuler[static_cast<std::size_t>(b)][a]);
        bool hinge = true;
        for (int o = 0; o < 3 && hinge; ++o) {
            if (o == a || m_jsTwistAxis[static_cast<std::size_t>(b)] == o) {
                continue;
            }
            const float otherRange = bone.rotLimited[o] ? bone.rotMax[o] - bone.rotMin[o] : 360.0f;
            hinge = otherRange <= kFoldHingeOtherDeg;
        }
        if (hinge && bone.rotLimited[a] && range >= kFoldRangeDeg && bone.rotMin[a] < 0.0f && bone.rotMax[a] > 0.0f &&
            std::min(-bone.rotMin[a], bone.rotMax[a]) <= kFoldNarrowFraction * std::max(-bone.rotMin[a], bone.rotMax[a])) {
            dof.foldSign = bone.rotMax[a] > -bone.rotMin[a] ? 1 : -1;
            if (dof.foldSign > 0) {
                dof.minDeg = std::min(m_boneEuler[static_cast<std::size_t>(b)][a], 0.0f);
            } else {
                dof.maxDeg = std::max(m_boneEuler[static_cast<std::size_t>(b)][a], 0.0f);
            }
        }
        out.push_back(dof);
    }
}

bool Armature::beginDigitDrag() {
    const int bone = m_selectedBone;
    m_digitBone = bone;
    m_digitLocal = m_ikGrabBone == bone ? m_ikGrabLocal : glm::vec3(0.0f);
    // THE DIGIT: the grabbed bone — when the grabbed point is off its joint, so that its own
    // channels turn the point — and the digit's bones above it, up to where the digit joins the
    // hand or the foot: the first bone that is no digit's joint (a carpal, the wrist, the ball of
    // the foot the toes fan from — a big toe dragged bends the big toe, not every toe at the ball).
    m_digitChain.clear();
    if (glm::length(m_digitLocal) > 1.0e-4f) {
        m_digitChain.push_back(bone);
    }
    for (int cur = m_bones[static_cast<std::size_t>(bone)].parent; cur >= 0 && boneClass(cur) == BoneClass::Digit;
         cur = m_bones[static_cast<std::size_t>(cur)].parent) {
        m_digitChain.push_back(cur);
    }
    std::reverse(m_digitChain.begin(), m_digitChain.end()); // parents first
    syncRigRotations(); // (the floor clearances are read along DOWN in each bone's frame)
    m_ikInvTransform = glm::inverse(m_transform); // fixed for the drag (see armature.h)
    m_ikGrabOffset = glm::vec3(0.0f);
    m_ikStartEuler = m_boneEuler; // the posture reference: this moment's pose
    m_jsFollowValid = false;
    m_digitDrag = true;
    static const bool kTrace = std::getenv("POSESTUDIO_IK_TRACE") != nullptr;
    if (kTrace) {
        std::fprintf(stderr, "[ik] beginDrag DIGIT %s, point (%.4f %.4f %.4f) in its frame, chain:",
                     m_boneNames[static_cast<std::size_t>(bone)].c_str(), m_digitLocal.x, m_digitLocal.y, m_digitLocal.z);
        for (const int b : m_digitChain) {
            std::fprintf(stderr, " %s", m_boneNames[static_cast<std::size_t>(b)].c_str());
        }
        std::fputc(10, stderr);
    }
    return true;
}

bool Armature::dragDigitTo(const glm::vec3& targetWorld) {
    const std::size_t n = m_bones.size();
    if (!m_ikRig || !m_jointSolver || m_digitBone < 0 || static_cast<std::size_t>(m_digitBone) >= n) {
        return false;
    }
    const float floorModel = -m_transform[3][1]; // the floor's height in model space
    glm::vec3   cursor(m_ikInvTransform * glm::vec4(targetWorld, 1.0f));
    cursor.y = std::max(cursor.y, floorModel); // (the floor bounds the cursor too)
    const glm::vec3 target = followIkTarget(cursor);
    const bool      following = glm::length(cursor - target) > 1.0e-5f;
    if (m_digitChain.empty()) {
        return false; // (a digit's base joint grabbed AT the joint: nothing of the digit moves it)
    }

    // The unknowns: the digit's unlocked channels (localChannelDofs).
    JointSolver::Problem problem;
    for (const int b : m_digitChain) {
        localChannelDofs(b, problem.dofs);
    }
    if (problem.dofs.empty()) {
        return false;
    }
    // NO POSTURE EASING (the body solve's, ikSpineCoupling; IK_DIGIT_EASE brings it back): a digit
    // has next to nothing redundant to ease, and the easing's creeping references cost a POP — a
    // finger held 57cm beyond its reach rested with a channel a hair inside its limit, the steps
    // shrank tick by tick until the active set's hair rule let that channel go, and the finger
    // turned 87 degrees in one tick, 27mm at its joint, a third of a second into a still hold (one
    // of the eight rigs). Without it the pose is the target's alone and the hold is still at once.
    static const bool kEase = std::getenv("IK_DIGIT_EASE") != nullptr; // A/B probe
    const double kappa = kEase ? kDampingLevels[ikDamping()].kappa : 0.0;
    if (kappa > 0.0) {
        for (JointSolverDof& dof : problem.dofs) {
            const double previous = static_cast<double>(m_boneEuler[static_cast<std::size_t>(dof.bone)][dof.axis]);
            dof.reference = (dof.reference + kappa * previous) / (1.0 + kappa);
            dof.stiffness *= 1.0 + kappa;
        }
    }

    // The cursor: the grabbed POINT held at the target, with the body solve's bounded pull — a
    // cursor the digit cannot reach draws it to its limits and no further.
    JointPositionTask grab;
    grab.bone = m_digitBone;
    grab.offset = glm::dvec3(m_digitLocal);
    grab.target = glm::dvec3(target);
    grab.weight = kEffectorWeight;
    grab.huber = kEffectorPull / (2.0 * kEffectorWeight);
    problem.positions.push_back(grab);

    // The floor: every joint the digit's channels move stays over its clearance.
    const int top = m_digitChain.front();
    for (std::size_t b = static_cast<std::size_t>(top) + 1; b < n; ++b) {
        if (!m_jointSolver->isAncestorOrSelf(top, static_cast<int>(b))) {
            continue;
        }
        JointPlaneTask floor;
        floor.bone = static_cast<int>(b);
        floor.planePoint = glm::dvec3(0.0, static_cast<double>(floorModel + m_ikRig->floorClearance(static_cast<int>(b))), 0.0);
        floor.normal = glm::dvec3(0.0, 1.0, 0.0);
        floor.weight = kPlaneWeight;
        problem.planes.push_back(floor);
    }

    JointSolver::Pose pose;
    pose.eulerDeg.resize(n);
    pose.translation.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        pose.eulerDeg[i] = glm::dvec3(m_boneEuler[i]);
        pose.translation[i] = glm::dvec3(m_boneTranslation[i]);
    }
    JointSolver::Settings settings;
    settings.maxIterations = 16;
    JointSolver::Result result = m_jointSolver->solve(pose, problem, settings);

    // THE BOUND RETRY (2026-09-30; IK_DIGIT_NO_BOUND_RETRY is the A/B): a channel the solve left AT
    // its authored limit is tried a few degrees back inside, and the cheaper pose kept — the fold
    // retry's idea, for a limit. The solver freezes a channel at a limit whose STEP points out of
    // the box, and with a neighbour still a hair short of its own limit that step can point out
    // while the channel's own gradient points in: a finger pulled along its own line folds into a
    // zigzag (its base extended, its middle joint curled to the limit to keep the grabbed joint on
    // the line), and pulled on beyond its reach it stayed in it until the base joint reached ITS
    // limit to the last hundredth of a degree — a third of a second into a still hold on one rig,
    // when the middle joint swung 87 degrees in one tick, 27mm at the grabbed joint. Retried, it
    // leaves the zigzag in the tick the straighter pose becomes the cheaper one, during the move.
    // (One extra small solve per channel at a limit; the retry must beat the pose by a real margin,
    // so two descents that land in the same minimum never trade places.)
    static const bool kNoBoundRetry = std::getenv("IK_DIGIT_NO_BOUND_RETRY") != nullptr; // A/B probe
    if (!kNoBoundRetry) {
        constexpr double kBoundKickDeg = 5.0;
        for (const JointSolverDof& dof : problem.dofs) {
            const std::size_t b = static_cast<std::size_t>(dof.bone);
            const Bone&       bone = m_bones[b];
            if (dof.axis >= 3 || !bone.rotLimited[dof.axis]) {
                continue;
            }
            const double lo = static_cast<double>(bone.rotMin[dof.axis]);
            const double hi = static_cast<double>(bone.rotMax[dof.axis]);
            const double v = pose.eulerDeg[b][dof.axis];
            const bool   atLo = v <= lo + 1.0e-3;
            const bool   atHi = v >= hi - 1.0e-3;
            if (atLo == atHi) {
                continue; // (inside its range — or a range too narrow to tell)
            }
            JointSolver::Pose kicked = pose;
            kicked.eulerDeg[b][dof.axis] = atLo ? std::min(lo + kBoundKickDeg, hi) : std::max(hi - kBoundKickDeg, lo);
            const JointSolver::Result retry = m_jointSolver->solve(kicked, problem, settings);
            if (retry.cost < result.cost * (1.0 - 1.0e-3) - 1.0e-9) {
                pose = kicked;
                result = retry;
            }
        }
    }

    // Applied in full.
    float changed = 0.0f;
    for (const JointSolverDof& dof : problem.dofs) {
        const std::size_t b = static_cast<std::size_t>(dof.bone);
        const float       v = static_cast<float>(pose.eulerDeg[b][dof.axis]);
        changed = std::max(changed, std::abs(v - m_boneEuler[b][dof.axis]));
        m_boneEuler[b][dof.axis] = v;
    }
    const bool moved = changed > 1e-5f;
    if (moved) {
        for (const int b : m_digitChain) {
            clampBoneEuler(b);
            recomposePoseLocal(static_cast<std::size_t>(b));
        }
        computeSkinMatrices();
    }
    static const bool kTrace = std::getenv("POSESTUDIO_IK_TRACE") != nullptr;
    if (kTrace) {
        const glm::vec3 point(m_poseGlobal[static_cast<std::size_t>(m_digitBone)] * glm::vec4(m_digitLocal, 1.0f));
        std::fprintf(stderr, "[js-digit] dofs=%zu it=%d conv=%d cost=%.6g miss=%.2fmm moved=%.4f\n", problem.dofs.size(),
                     result.iterations, result.converged ? 1 : 0, result.cost, glm::length(point - target) * 1000.0f, changed);
    }
    return moved || following;
}

} // namespace pose

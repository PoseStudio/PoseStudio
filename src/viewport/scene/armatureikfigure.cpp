/**
 * @file armatureikfigure.cpp
 * @brief THE FIGURE MOVE (IkScope::Figure): the app's Ctrl+drag of the body itself — the hips, the
 *        abdomen, the chest — moves the whole figure as she is posed, lifted off the floor or
 *        carried across it, and the user's pins hold.
 *
 * The user: "when ctrl + pulling anywhere on the body should disable any ik pinning to the floor,
 * lifting the figure in its current position. This should not affect user added pins" (2026-09-28).
 * Until then a Ctrl+drag of the trunk was the scoped chain's (IkScope::Chain: the chest bent the
 * spine over a still pelvis, the hips took the legs over planted feet), and there was no way to pick
 * a posed figure up and put her somewhere else.
 *
 * Like the digit drag it is a solve of its own that the IK rig has no part in: the rig begins no
 * drag, so NO floor contact is planted — not the feet, not a knee or a hand the pose has on the
 * floor — nothing balances, steps or lifts off, and no post-step turns an arm or the head. What
 * moves is the solve root's POSE TRANSLATION (the figure's place: what a hip walk writes, what a
 * pose file carries as its `@trans:` row), so the move is an ordinary pose edit: one undo step.
 * No joint turns — except in a limb the user PINNED: its chain, from the pinned joint up to where
 * the limb joins the body, gives so that the joint stays where it is pinned, held as hard as under
 * any drag; when the limb runs out, the pin stops the body. A pin on the trunk holds the figure
 * where she is. The FLOOR still stops her on the way down: the joints that ride the body may come
 * down to their clearances and no further (a bound on the target: the body is one rigid piece).
 * Qt-free (std + GLM).
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

bool Armature::isBodyGrab(int effector) const {
    if (!m_ikRig || effector < 0 || static_cast<std::size_t>(effector) >= m_bones.size()) {
        return false;
    }
    const IkRig& rig = *m_ikRig;
    const int    root = rig.rootNode();
    if (effector == root) {
        return true; // (the hips — and a joint of the pelvis girdle, which the rig promotes to them)
    }
    // A bone of the SPINE: what it hangs from, going up, is the root itself — a limb's, the neck's
    // and the head's junction is the bone they branch from — and it is no leg's joint (on a rig
    // whose thighs hang straight off the hips a leg's junction is the root too).
    const bool footClass = rig.isFootClass(effector);
    return rig.limbJunction(effector) == root && !rig.isLegJointAboveFoot(effector) && !footClass;
}

bool Armature::beginFigureMove() {
    const std::size_t n = m_bones.size();
    const IkRig&      rig = *m_ikRig;
    const int         root = rig.rootNode();
    m_moveBone = m_selectedBone;
    m_moveLocal = m_ikGrabBone == m_selectedBone ? m_ikGrabLocal : glm::vec3(0.0f);
    m_movePins.clear();
    m_moveLimbs.clear();
    m_moveHeld = false;
    const float floorModel = -m_transform[3][1]; // the floor's height in model space
    m_moveFloor.assign(n, -1.0e9f);
    for (std::size_t b = 0; b < n; ++b) {
        if (m_jointSolver->isAncestorOrSelf(root, static_cast<int>(b))) { // (a joint of the body: it rides)
            m_moveFloor[b] = std::min(floorModel + rig.floorClearance(static_cast<int>(b)), m_poseGlobal[b][3][1]);
        }
    }
    // THE USER'S PINS HOLD. A pin on the grabbed bone sits out (dragging a pinned joint moves the
    // pin, as ever); a pin on the body itself holds the whole figure; a pin in a limb is served by
    // that limb's chain, whose joints go where the pin holds them — not where the body goes.
    std::vector<char> limb(n, 0);
    for (std::size_t b = 0; b < m_bonePinned.size() && b < n; ++b) {
        if (!m_bonePinned[b] || static_cast<int>(b) == m_moveBone || !m_jointSolver->isAncestorOrSelf(root, static_cast<int>(b))) {
            continue;
        }
        if (isBodyGrab(static_cast<int>(b)) || rig.isGirdleJoint(static_cast<int>(b))) {
            m_moveHeld = true;
            continue;
        }
        m_movePins.push_back({static_cast<int>(b), glm::vec3(m_poseGlobal[b][3]), glm::mat3(m_poseGlobal[b])});
        const int junction = rig.limbJunction(static_cast<int>(b));
        int       top = static_cast<int>(b);
        for (int cur = static_cast<int>(b); cur >= 0 && cur != junction && cur != root; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            limb[static_cast<std::size_t>(cur)] = 1;
            top = cur;
        }
        for (std::size_t j = static_cast<std::size_t>(top); j < n; ++j) {
            if (m_jointSolver->isAncestorOrSelf(top, static_cast<int>(j))) {
                m_moveFloor[j] = -1.0e9f; // (a joint of the pinned limb: the floor's bound on the BODY is not its business)
            }
        }
    }
    for (std::size_t b = 0; b < n; ++b) {
        if (limb[b]) {
            m_moveLimbs.push_back(static_cast<int>(b)); // (parents first: the bone order)
        }
    }
    // THE HANGING FEET COME DOWN FLAT (armaturehangfeet.cpp; beginIkDrag found them): carried down,
    // feet a lift left hanging go flat as the floor nears (dragFigureTo) — so what stops her is
    // where their SOLES will stand: the ankle at its clearance, the joints below it out of the
    // bound. Not a foot of a pinned limb: that one goes where its pin holds it.
    for (std::size_t f = m_hangFeet.size(); f-- > 0;) {
        const HangFoot& foot = m_hangFeet[f];
        bool            pinned = limb[static_cast<std::size_t>(foot.ankle)] != 0;
        for (const int b : foot.subtree) {
            pinned = pinned || (static_cast<std::size_t>(b) < m_bonePinned.size() && m_bonePinned[static_cast<std::size_t>(b)]);
        }
        if (pinned) {
            m_hangFeet.erase(m_hangFeet.begin() + static_cast<std::ptrdiff_t>(f));
            continue;
        }
        for (const int b : foot.subtree) {
            if (b != foot.ankle) {
                m_moveFloor[static_cast<std::size_t>(b)] = -1.0e9f;
            }
        }
    }
    syncRigRotations();
    m_ikInvTransform = glm::inverse(m_transform); // fixed for the drag (see armature.h)
    m_ikGrabOffset = glm::vec3(0.0f);
    m_ikStartEuler = m_boneEuler; // the posture reference: this moment's pose
    m_jsStartTranslation = m_boneTranslation;
    m_jsFollowValid = false;
    m_figureMove = true;
    m_ikScope = IkScope::Figure;
    static const bool kTrace = std::getenv("POSESTUDIO_IK_TRACE") != nullptr;
    if (kTrace) {
        std::fprintf(stderr, "[ik] beginDrag FIGURE by %s, point (%.4f %.4f %.4f) in its frame; %zu pin(s) held by %zu limb bone(s)%s\n",
                     m_boneNames[static_cast<std::size_t>(m_moveBone)].c_str(), m_moveLocal.x, m_moveLocal.y, m_moveLocal.z,
                     m_movePins.size(), m_moveLimbs.size(), m_moveHeld ? "; a pin on the body holds her where she is" : "");
    }
    return true;
}

bool Armature::dragFigureTo(const glm::vec3& targetWorld) {
    const std::size_t n = m_bones.size();
    if (!m_ikRig || !m_jointSolver || m_moveBone < 0 || static_cast<std::size_t>(m_moveBone) >= n) {
        return false;
    }
    const int       root = m_ikRig->rootNode();
    const glm::vec3 cursor(m_ikInvTransform * glm::vec4(targetWorld, 1.0f));
    glm::vec3       target = followIkTarget(cursor);
    const bool      following = glm::length(cursor - target) > 1.0e-5f;
    if (m_moveHeld || root < 0) {
        return false; // (pinned by the body: she stays)
    }
    // THE FLOOR stops her on the way down: the body is one rigid piece, so how far it may come
    // down is the least room any joint that rides it has left over the height it may come down to.
    const glm::vec3 grabbed(m_poseGlobal[static_cast<std::size_t>(m_moveBone)] * glm::vec4(m_moveLocal, 1.0f));
    float           room = 1.0e9f;
    for (std::size_t b = 0; b < n && b < m_moveFloor.size(); ++b) {
        if (m_moveFloor[b] > -1.0e8f) {
            room = std::min(room, m_poseGlobal[b][3][1] - m_moveFloor[b]);
        }
    }
    target.y = std::max(target.y, grabbed.y - std::max(room, 0.0f));

    // The unknowns: the root's pose translation — free: the pelvis drag's own price, against which
    // the cursor's pull is exact — and the pinned limbs' channels.
    JointSolver::Problem problem;
    for (int a = 3; a < 6; ++a) {
        JointSolverDof dof;
        dof.bone = root;
        dof.axis = a;
        dof.stiffness = 1.0;
        dof.reference = static_cast<double>(m_boneTranslation[static_cast<std::size_t>(root)][a - 3]); // (where she is: a move has no home)
        problem.dofs.push_back(dof);
    }
    for (const int b : m_moveLimbs) {
        localChannelDofs(b, problem.dofs);
    }
    JointPositionTask grab;
    grab.bone = m_moveBone;
    grab.offset = glm::dvec3(m_moveLocal);
    grab.target = glm::dvec3(target);
    grab.weight = kEffectorWeight;
    grab.huber = kEffectorPull * kRootDragPullScale / (2.0 * kEffectorWeight);
    problem.positions.push_back(grab);
    for (const HeldPin& pin : m_movePins) {
        JointPositionTask place;
        place.bone = pin.bone;
        place.target = glm::dvec3(pin.place);
        place.weight = kPinWeight * kUserPinScale;
        problem.positions.push_back(place);
        JointOrientationTask hold;
        hold.bone = pin.bone;
        hold.target = glm::dmat3(pin.rotation);
        hold.weight = kPinWeight * kUserPinScale * kPinOrientScale * kPinOrientScale;
        problem.orientations.push_back(hold);
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
    const JointSolver::Result result = m_jointSolver->solve(pose, problem, settings);

    // Applied in full.
    float changed = 0.0f;
    for (const JointSolverDof& dof : problem.dofs) {
        const std::size_t b = static_cast<std::size_t>(dof.bone);
        if (dof.axis < 3) {
            const float v = static_cast<float>(pose.eulerDeg[b][dof.axis]);
            changed = std::max(changed, std::abs(v - m_boneEuler[b][dof.axis]));
            m_boneEuler[b][dof.axis] = v;
        } else {
            const float v = static_cast<float>(pose.translation[b][dof.axis - 3]);
            changed = std::max(changed, std::abs(v - m_boneTranslation[b][dof.axis - 3]) * 1000.0f);
            m_boneTranslation[b][dof.axis - 3] = v;
        }
    }
    const bool moved = changed > 1e-5f;
    if (moved) {
        recomposePoseLocal(static_cast<std::size_t>(root));
        for (const int b : m_moveLimbs) {
            clampBoneEuler(b);
            recomposePoseLocal(static_cast<std::size_t>(b));
        }
        computeSkinMatrices();
    }
    // THE SOFT BODY RIDES THE MOVE (2026-11-06): the chest and the hips go at the cursor's speed —
    // the sharpest jolt a user makes — and the breasts and the buttocks feel its acceleration as
    // under any drag (armaturebreasts.cpp, armaturebuttocks.cpp); the settle ticks them on alone
    // once the figure stops (settleIkTick). Until this day the move never ticked them: a figure
    // carried by Ctrl rode dead still. Off, tickSoftBody returns at once.
    m_jsSoftBodyMoving = tickSoftBody(1.0f / 60.0f);
    // (THE HANGING FEET: no more hanging than the floor allows — flat as she is set down.)
    const bool feetMoved = poseHangFeet(1.0f, 0.0f);
    static const bool kTrace = std::getenv("POSESTUDIO_IK_TRACE") != nullptr;
    if (kTrace) {
        const glm::vec3 point(m_poseGlobal[static_cast<std::size_t>(m_moveBone)] * glm::vec4(m_moveLocal, 1.0f));
        std::fprintf(stderr, "[js-figure] dofs=%zu it=%d conv=%d cost=%.6g miss=%.2fmm room=%.1fmm moved=%.4f\n", problem.dofs.size(),
                     result.iterations, result.converged ? 1 : 0, result.cost, glm::length(point - target) * 1000.0f,
                     room < 1.0e8f ? room * 1000.0f : -1.0f, changed);
    }
    return moved || following || feetMoved || m_jsSoftBodyMoving; // (a bounce under a still cursor is a frame to draw)
}

} // namespace pose

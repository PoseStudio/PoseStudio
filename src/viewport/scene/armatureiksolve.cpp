/**
 * @file armatureiksolve.cpp
 * @brief The Armature's per-tick IK SOLVE (scene/ik/jointsolver.h) — the entry points and the map.
 *
 * One tick of a drag is dragIkTick: the damped target follower, the rig's intents (IkRig), then
 * solveIk, then the live contacts, landings and the step policy on the solved pose, and the two
 * POST-STEPS (armatureikpost.cpp: the idle arms' hang, the head's righting). solveIk itself is a
 * sequence of STAGES over one IkSolveScratch (armatureiksolvestate.h), each a member function:
 *
 *   armatureiksolvedrag.cpp     what the drag IS and which channels answer it —
 *     ikChooseUnknowns          the unknowns (active paths, pins, toes, the root's rotation), the balls
 *     ikTrunkPolicy             the trunk follow / unfold, the hip sway, the hip hinge's balance
 *     ikStiffnessClasses        dragged limb / pin-serving limb / trunk, the twist-priced bones
 *     ikFootDrag                the SLIDE of a dragged foot
 *     ikKneeDrag                a knee's goal over its planted foot, or the foot letting go
 *     ikElbowDrag               an elbow's goal on its reach, the hand that stays
 *   armatureiksolveposture.cpp  the posture model that prices them —
 *     ikPostureModel            every unknown's stiffness and reference, the root's home
 *     ikSpineCoupling           the spine as one, the neck as its top, the posture easing
 *   armatureiksolverows.cpp     the rows the solver satisfies —
 *     ikPinRowsBegin/ikPinRows/ikPinRowsEnd   the pins: user pins, the foot contact model,
 *                               live contacts, landed hands, the knee-drag foot's release
 *     ikCursorRows              the cursor, the rising trunk, the slide, the hand slide, suspension
 *     ikBalanceRow              balance over the support polygon
 *     ikPlaneRows               the floor's and the body volumes' one-sided rows
 *   armatureiksolve.cpp (here)  ikSolve, ikApply, ikSolveTraces — and the tick around them.
 *
 * The tuning constants live in armatureiksolvetuning.h. What the rig supplies is the drag's
 * POLICY — which joints are planted and where their pins are (ground healing, steps, slides,
 * landings), which node is solved for a token-mass grab, the mass model and the support polygon,
 * the body volumes, when the figure lifts off; what happens here is how a pose is found.
 *
 * The unknowns are the unlocked Euler channels of every bone that moves an ACTIVE node (the
 * paths joining the effector and the pins to the solve root), the pins' own channels (their
 * held orientation), the toes under a standing foot, and the solve root's translation — plus,
 * under a pelvis drag or while the body rises, the root's rotation; everything else keeps its
 * local pose and rides.
 *
 * The POSTURE term is the stiffness model: each channel is pulled toward its drag-start value
 * at a cost of (deflection / its authored range)^2 — a joint with a wide range is supple, one
 * with a narrow range is stiff, which is the rig's own statement of what moves easily — times a
 * class factor: the dragged limb 1, the limbs that serve a pin less (they articulate to hold
 * it), the trunk more (the body follows a reach reluctantly), and the root's translation
 * priced per metre. It makes the pose a FUNCTION of the targets: nothing to tremble at a still
 * cursor, the body back where it started when the cursor returns, no damping needed for
 * stability — the slight DAMPING there is (ikDamping) is for feel, and is built so that every
 * constraint stays exact on every tick. Qt-free (std + GLM).
 */

#include "armature.h"
#include "softbodysettings.h"
#include "armatureiksolvestate.h"
#include "armatureiksolvetuning.h"

#include "balancecontroller.h"
#include "ikmath.h"
#include "ikrig.h"
#include "jointsolver.h"

#include <glm/gtc/quaternion.hpp>

#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <tuple>

namespace pose {

namespace {

/// The process-wide damping level (Armature::ikDamping): -1 until first read.
std::atomic<int> g_ikDamping{-1};

} // namespace

int Armature::ikDamping() {
    int level = g_ikDamping.load();
    if (level < 0) {
        const char* env = std::getenv("IK_JS_DAMPING");
        level = env != nullptr ? std::atoi(env) : kIkDampingDefault;
        level = std::clamp(level, 0, kIkDampingLevels - 1);
        g_ikDamping.store(level);
    }
    return level;
}

void Armature::buildJointSolver() {
    std::vector<JointSolverBone> bones(m_bones.size());
    m_jsTwistAxis.assign(m_bones.size(), -1);
    m_jsSpineChain.clear(); // (a new skeleton: searched again at the next solve — ensureSpineChain)
    m_jsSpineChainChest = -1;
    m_jsWristOf.clear();    // (... and the hands: ensureHandMaps)
    m_jsHandTip.clear();
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const Bone& src = m_bones[i];
        JointSolverBone& dst = bones[i];
        dst.parent = src.parent;
        dst.bindOffset = glm::dvec3(glm::vec3(src.localBind[3]));
        dst.orient = glm::dmat3(glm::mat3(src.orient));
        dst.rotationOrder = src.rotationOrder;
        dst.minDeg = glm::dvec3(src.rotMin);
        dst.maxDeg = glm::dvec3(src.rotMax);
        dst.limited = src.rotLimited;
        // The channel most parallel to the segment toward the bone's longest child: twist.
        float longest = 0.02f;
        glm::vec3 segment(0.0f);
        for (const int c : m_children[i]) {
            const glm::vec3 offset(m_bones[static_cast<std::size_t>(c)].localBind[3]);
            if (glm::length(offset) > longest) {
                longest = glm::length(offset);
                segment = offset / longest;
            }
        }
        if (glm::dot(segment, segment) > 0.5f) {
            float best = 0.8f;
            for (int a = 0; a < 3; ++a) {
                const float along = std::abs(glm::dot(glm::vec3(src.orient[a]), segment));
                if (along > best) {
                    best = along;
                    m_jsTwistAxis[i] = a;
                }
            }
        }
    }
    m_jointSolver = std::make_unique<JointSolver>();
    m_jointSolver->setSkeleton(std::move(bones));
}

bool Armature::solveIk(const glm::vec3* dragTarget, const glm::vec3* holdTarget) {
    IkRig& rig = *m_ikRig;
    const std::size_t n = m_bones.size();
    const int root = rig.rootNode();
    const int effector = rig.dragEffector();
    if (!m_jointSolver || root < 0 || effector < 0) {
        return false;
    }
    IkSolveScratch s{rig, n, root, effector, *m_jointSolver, rig.pins(), rig.activeNodes(), rig.secondaryEffectors(),
                     dragTarget, holdTarget};
    s.lateral = glm::vec3(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
    s.fore = glm::vec3(std::sin(m_jsRootHeading), 0.0f, std::cos(m_jsRootHeading));

    // The stages, in order — each a member function over the scratch (armatureiksolvestate.h):
    // what the drag IS and which channels answer it, the posture model that prices them, the
    // rows the solver satisfies, the solve, and the pose applied in full.
    ikChooseUnknowns(s);
    ikTrunkPolicy(s);
    ikStiffnessClasses(s);
    ikFootDrag(s);
    ikKneeDrag(s);
    ikElbowDrag(s);
    ikPostureModel(s);
    ikSpineCoupling(s);
    ikPinRowsBegin(s);
    for (std::size_t p = 0; p < s.pins.size(); ++p) {
        ikPinRows(s, p);
    }
    ikPinRowsEnd(s);
    ikCursorRows(s);
    ikBalanceRow(s);
    ikPlaneRows(s);
    ikSolve(s);
    ikApply(s);
    ikSolveTraces(s);
    return s.moved;
}

/// The SOLVE: the pose as it stands, the settings, and the continuation over a cursor jump.
void Armature::ikSolve(IkSolveScratch& s) {
    const std::size_t n = s.n;
    const int effector = s.effector;
    const JointSolver& solver = s.solver;
    const glm::vec3* const dragTarget = s.dragTarget;
    bool& kneeDrag = s.kneeDrag;
    glm::vec3& kneeGoal = s.kneeGoal;
    bool& elbowDrag = s.elbowDrag;
    glm::vec3& elbowGoal = s.elbowGoal;
    JointSolver::Problem& problem = s.problem;
    std::size_t& goalTask = s.goalTask;

    // --- Solve ----------------------------------------------------------------------------
    JointSolver::Pose& pose = s.pose;
    pose.eulerDeg.resize(n);
    pose.translation.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        pose.eulerDeg[i] = glm::dvec3(m_boneEuler[i]);
        pose.translation[i] = glm::dvec3(m_boneTranslation[i]);
    }
    // (IK_JS_START_COST_TRACE=<n>: the position rows that BEGIN a solve costing more than n —
    // weight x miss^2 on the pose as the last tick left it: which row a tick's rows have moved away
    // from the pose, where IK_JS_COST_TRACE shows what a solve ENDS fighting.)
    static const char* kStartCost = std::getenv("IK_JS_START_COST_TRACE");
    if (kStartCost != nullptr) {
        static const double kFrom = std::max(1.0, std::atof(kStartCost));
        for (const JointPositionTask& task : problem.positions) {
            const std::size_t tb = static_cast<std::size_t>(task.bone);
            const glm::dvec3  at = glm::dvec3(glm::vec3(m_poseGlobal[tb][3])) + glm::dmat3(glm::mat3(m_poseGlobal[tb])) * task.offset;
            const glm::dvec3  e = (task.target - at) * task.axisScale;
            const double      cost = task.weight * glm::dot(e, e);
            if (cost > kFrom) {
                std::fprintf(stderr, "[js-start] position %s: (%.1f %.1f %.1f) mm off, weight %.4g, cost %.1f", m_boneNames[tb].c_str(), e.x * 1000.0,
                             e.y * 1000.0, e.z * 1000.0, task.weight, cost);
                std::fputc(10, stderr);
            }
        }
    }
    JointSolver::Settings settings;
    JointSolver::Result& result = s.result;
    s.iterations = 0;
    int& iterations = s.iterations;
    if (goalTask != static_cast<std::size_t>(-1) && dragTarget != nullptr) {
        // Continuation: a cursor jump is walked in kTargetSubstep pieces from where the
        // effector IS, each solved a few iterations deep, the last to convergence.
        const glm::vec3 from(m_poseGlobal[static_cast<std::size_t>(effector)][3]);
        const glm::vec3 to = kneeDrag ? kneeGoal : elbowDrag ? elbowGoal : *dragTarget; // (a knee's, an elbow's: taken onto their reach)
        // (The walk is for a target that JUMPED, so it is as long as the target's own move since
        // the last tick, not as the miss: a target held out of reach is a miss that stays, and
        // walked afresh every tick it was twelve solves a tick, each ending in ten rejected
        // dampings — 18ms of tick work, over the tick's whole budget, for a hand held half a
        // metre beyond its reach with the mouse still.)
        const float dist = glm::length(to - from);
        const float jumped = m_jsPrevGoalValid ? glm::length(to - m_jsPrevGoal) : dist;
        m_jsPrevGoal = to;
        m_jsPrevGoalValid = true;
        // (Less a hundredth of a piece: under a STILL cursor the follower's last micron of travel
        // made "jumped" 1e-7 and the ceiling TWO pieces — a solve to a target halfway back from a
        // cursor held out of reach, every tick, and the real one from wherever that left the pose:
        // on one rig a folded-over crouch answered with a 2-tick limit cycle, 45mm at the face.)
        static const bool kTaskTrace = std::getenv("IK_JS_TASK_TRACE") != nullptr; // the problem's rows by task, before the solve
        if (kTaskTrace) {
            for (std::size_t i = 0; i < problem.positions.size(); ++i) {
                const JointPositionTask& pt = problem.positions[i];
                std::fprintf(stderr, "[js-task] position %zu bone %d (%s) weight %.3g huber %.3g axes(%.0f %.0f %.0f)", i, pt.bone,
                             pt.bone >= 0 && static_cast<std::size_t>(pt.bone) < m_boneNames.size() ? m_boneNames[static_cast<std::size_t>(pt.bone)].c_str() : "?",
                             pt.weight, pt.huber, pt.axisScale.x, pt.axisScale.y, pt.axisScale.z);
                std::fputc(10, stderr);
            }
            for (std::size_t i = 0; i < problem.orientations.size(); ++i) {
                const JointOrientationTask& ot = problem.orientations[i];
                std::fprintf(stderr, "[js-task] orientation %zu bone %d (%s) weight %.3g perAxis %d axisWeights(%.3g %.3g %.3g)", i, ot.bone,
                             ot.bone >= 0 && static_cast<std::size_t>(ot.bone) < m_boneNames.size() ? m_boneNames[static_cast<std::size_t>(ot.bone)].c_str() : "?",
                             ot.weight, ot.perAxis ? 1 : 0, ot.axisWeights.x, ot.axisWeights.y, ot.axisWeights.z);
                std::fputc(10, stderr);
            }
        }
        const int pieces = std::clamp(
            static_cast<int>(std::ceil(std::min(dist, jumped + kTargetSubstep) / kTargetSubstep - 0.01f)), 1, 12);
        for (int k = 1; k <= pieces; ++k) {
            const glm::vec3 t = glm::mix(from, to, static_cast<float>(k) / static_cast<float>(pieces));
            problem.positions[goalTask].target = glm::dvec3(t);
            static const int kIterations = std::max(4, static_cast<int>(envOr("IK_JS_ITERATIONS", 16.0)));
            settings.maxIterations = k == pieces ? kIterations : 4;
            result = solver.solve(pose, problem, settings);
            iterations += result.iterations;
        }
    } else {
        settings.maxIterations = 16;
        result = solver.solve(pose, problem, settings);
        iterations = result.iterations;
    }
}

/// APPLY the solved pose in full: every channel and the root's translation, then the skin matrices.
void Armature::ikApply(IkSolveScratch& s) {
    const std::size_t n = s.n;
    const int root = s.root;
    std::vector<char>& dofBone = s.dofBone;
    JointSolver::Problem& problem = s.problem;
    JointSolver::Pose& pose = s.pose;

    // --- Apply, in full -------------------------------------------------------------------
    s.changed = 0.0f;
    float& changed = s.changed;
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
    s.moved = changed > 1e-5f;
    const bool& moved = s.moved;
    if (moved) {
        for (std::size_t b = 0; b < n; ++b) {
            if (dofBone[b] || static_cast<int>(b) == root) {
                clampBoneEuler(static_cast<int>(b));
                recomposePoseLocal(b);
            }
        }
        computeSkinMatrices();
    }
}

/// The solve's TRACES (POSESTUDIO_IK_TRACE, IK_JS_JACOBIAN_CHECK, IK_JS_FAR_ROWS, IK_JS_COST_TRACE, IK_JS_POSE_TRACE): probes only, no effect on the pose.
void Armature::ikSolveTraces(IkSolveScratch& s) {
    const int effector = s.effector;
    const JointSolver& solver = s.solver;
    const std::vector<IkEffector>& pins = s.pins;
    JointSolver::Problem& problem = s.problem;
    const glm::vec3*& goal = s.goal;
    JointSolver::Pose& pose = s.pose;
    JointSolver::Result& result = s.result;
    int& iterations = s.iterations;
    float& changed = s.changed;

    static const bool kTrace = std::getenv("POSESTUDIO_IK_TRACE") != nullptr;
    if (kTrace) {
        float pinErr = 0.0f;
        for (const IkEffector& pin : pins) {
            const std::size_t node = static_cast<std::size_t>(pin.node);
            pinErr = std::max(pinErr, glm::length(glm::vec3(m_poseGlobal[node][3]) - m_jsPinTarget[node]));
        }
        const float miss =
            goal != nullptr
                ? glm::length(glm::vec3(m_poseGlobal[static_cast<std::size_t>(effector)][3]) - *goal)
                : 0.0f;
        std::fprintf(stderr,
                     "[js] dofs=%zu rows=%zu it=%d conv=%d cost=%.6g miss=%.2fmm pin=%.3fmm moved=%.4f\n",
                     problem.dofs.size(),
                     problem.positions.size() * 3 + problem.orientations.size() * 3 + 1, iterations,
                     result.converged ? 1 : 0, result.cost, miss * 1000.0f, pinErr * 1000.0f, changed);
    }
    static const bool kJacobianCheck = std::getenv("IK_JS_JACOBIAN_CHECK") != nullptr;
    static const bool kJacobianCheckAll = kJacobianCheck && std::string(std::getenv("IK_JS_JACOBIAN_CHECK")) == "all";
    if (kJacobianCheck && (!result.converged || kJacobianCheckAll)) {
        // A solve that ran out of iterations or stalled: is a row's Jacobian wrong HERE?
        JointSolver::Problem fixedPlanes = problem;
        fixedPlanes.planeSource = nullptr;
        int row = -1, dofIndex = -1;
        const double worst = solver.jacobianError(pose, fixedPlanes, &row, &dofIndex);
        if (dofIndex >= 0) {
            const JointSolverDof& d = problem.dofs[static_cast<std::size_t>(dofIndex)];
            std::fprintf(stderr, "[js-jac] worst Jacobian error %.4g at row %d (pos rows %zu, ori rows %zu), dof %s axis %d",
                         worst, row, problem.positions.size() * 3, problem.orientations.size() * 3,
                         m_boneNames[static_cast<std::size_t>(d.bone)].c_str(), d.axis);
            std::fputc(10, stderr);
            if (row >= 0 && static_cast<std::size_t>(row) < problem.positions.size() * 3) {
                const JointPositionTask& task = problem.positions[static_cast<std::size_t>(row) / 3];
                std::fprintf(stderr, "[js-jac]   task on %s: weight %.4g huber %.4g axisScale (%.2f %.2f %.2f) offset (%.3f %.3f %.3f)",
                             m_boneNames[static_cast<std::size_t>(task.bone)].c_str(), task.weight, task.huber, task.axisScale.x,
                             task.axisScale.y, task.axisScale.z, task.offset.x, task.offset.y, task.offset.z);
                std::fputc(10, stderr);
            }
        }
    }
    static const bool kFarTrace = std::getenv("IK_JS_FAR_ROWS") != nullptr;
    if (kFarTrace) {
        // The position rows that end a solve further from their targets than a step's bounded ask
        // (5cm): the rows whose pull the ask's bound used to weaken (JointSolver::residuals).
        for (const JointPositionTask& task : problem.positions) {
            const std::size_t tb = static_cast<std::size_t>(task.bone);
            const glm::dvec3 at = glm::dvec3(glm::vec3(m_poseGlobal[tb][3])) +
                                  glm::dmat3(glm::mat3(m_poseGlobal[tb])) * task.offset;
            const double len = glm::length((task.target - at) * task.axisScale);
            static const double kFarFrom = std::max(1.0, std::atof(std::getenv("IK_JS_FAR_ROWS"))) * 0.001;
            if (len > (kFarFrom > 0.0015 ? kFarFrom : 0.05)) {
                std::fprintf(stderr, "[js-far] %s: %.1fmm from its target, weight %.4g, huber %.4g\n",
                             m_boneNames[tb].c_str(), len * 1000.0, task.weight, task.huber);
            }
        }
    }
    static const bool kCostTrace = std::getenv("IK_JS_COST_TRACE") != nullptr;
    if (kCostTrace) {
        // The rows that end a solve COSTING more than IK_JS_COST_TRACE (a number): weight x miss^2 for
        // the position rows, weight x angle^2 for the orientation rows - which row a solve is
        // fighting, when the far-rows trace (misses over 5cm) shows nothing.
        static const double kCostFrom = std::max(1.0, std::atof(std::getenv("IK_JS_COST_TRACE")));
        for (const JointPositionTask& task : problem.positions) {
            const std::size_t tb = static_cast<std::size_t>(task.bone);
            const glm::dvec3 at = glm::dvec3(glm::vec3(m_poseGlobal[tb][3])) +
                                  glm::dmat3(glm::mat3(m_poseGlobal[tb])) * task.offset;
            const glm::dvec3 e = (task.target - at) * task.axisScale;
            const double cost = task.weight * glm::dot(e, e);
            if (cost > kCostFrom) {
                std::fprintf(stderr, "[js-cost] position %s: %.1fmm off, weight %.4g, cost %.1f", m_boneNames[tb].c_str(), glm::length(e) * 1000.0, task.weight, cost);
                std::fputc(10, stderr);
            }
        }
        for (const JointOrientationTask& task : problem.orientations) {
            const std::size_t tb = static_cast<std::size_t>(task.bone);
            const glm::dmat3 nowRot = glm::dmat3(glm::mat3(m_poseGlobal[tb]));
            const glm::dmat3 targetT = glm::transpose(task.target);
            const glm::dmat3 err = targetT * nowRot;
            const double     tr = glm::clamp((err[0][0] + err[1][1] + err[2][2] - 1.0) * 0.5, -1.0, 1.0);
            const double     angle = std::acos(tr);
            const double     w = task.perAxis ? std::max(task.axisWeights.x, std::max(task.axisWeights.y, task.axisWeights.z)) : task.weight;
            const double     cost = w * angle * angle;
            if (cost > kCostFrom) {
                std::fprintf(stderr, "[js-cost] orientation %s: %.2f deg off, weight %.4g (%s), cost <= %.1f", m_boneNames[tb].c_str(), glm::degrees(angle), w, task.perAxis ? "per-axis max" : "uniform", cost);
                std::fputc(10, stderr);
            }
        }
    }
    static const bool kPoseTrace = std::getenv("IK_JS_POSE_TRACE") != nullptr;
    if (kPoseTrace) {
        // The posture bill: every unknown more than half a degree (2mm) off its reference.
        std::fprintf(stderr, "[js-pose]");
        for (const JointSolverDof& dof : problem.dofs) {
            const std::size_t b = static_cast<std::size_t>(dof.bone);
            const double v = dof.axis < 3 ? pose.eulerDeg[b][dof.axis] : pose.translation[b][dof.axis - 3];
            const double d = v - dof.reference;
            if (std::abs(d) > (dof.axis < 3 ? 0.5 : 0.002)) {
                std::fprintf(stderr, " %s.%c%s=%+.2f(s%.2f)", m_boneNames[b].c_str(), "xyzXYZ"[dof.axis],
                             dof.axis < 3 ? "" : "t", dof.axis < 3 ? d : d * 1000.0, dof.stiffness);
            }
        }
        std::fprintf(stderr, "\n");
    }
}

glm::vec3 Armature::followIkTarget(const glm::vec3& rawTarget) {
    // The damped FOLLOWER (see Armature::ikDamping): a critically damped spring from
    // the followed target to the raw one, integrated exactly over one tick (stable at any
    // stiffness). Seeded ON the first target: a drag starts at rest at its grab point.
    const float omega = kDampingLevels[ikDamping()].followOmega;
    if (!m_jsFollowValid) {
        m_jsStartTarget = rawTarget; // the grab point: what "pushed down" is measured from
    }
    if (!m_jsFollowValid || omega <= 0.0f) {
        m_jsFollowPos = rawTarget;
        m_jsFollowVel = glm::vec3(0.0f);
        m_jsFollowValid = true;
    } else {
        const float decay = std::exp(-omega * kTickSeconds);
        const glm::vec3 offset = m_jsFollowPos - rawTarget;
        const glm::vec3 temp = (m_jsFollowVel + offset * omega) * kTickSeconds;
        m_jsFollowVel = (m_jsFollowVel - temp * omega) * decay;
        m_jsFollowPos = rawTarget + (offset + temp) * decay;
    }
    return m_jsFollowPos;
}

void Armature::updateGroupTargets() {
    const std::vector<int>& secs = m_ikRig->secondaryEffectors();
    if (secs.empty() || !m_jsGroupCursorValid || m_jsGroupStartPos.size() != secs.size()) {
        return;
    }
    // The group's own follower — the primary's twin (followIkTarget) — on the raw cursor: the
    // secondaries trail the cursor by the same tick's travel the primary does, and the settle's
    // ticks (dragIkTick under the last target) run it to rest like the primary's.
    const float omega = kDampingLevels[ikDamping()].followOmega;
    if (!m_jsGroupFollowValid || omega <= 0.0f) {
        m_jsGroupFollowPos = m_jsGroupCursorRaw;
        m_jsGroupFollowVel = glm::vec3(0.0f);
        m_jsGroupFollowValid = true;
    } else {
        const float decay = std::exp(-omega * kTickSeconds);
        const glm::vec3 offset = m_jsGroupFollowPos - m_jsGroupCursorRaw;
        const glm::vec3 temp = (m_jsGroupFollowVel + offset * omega) * kTickSeconds;
        m_jsGroupFollowVel = (m_jsGroupFollowVel - temp * omega) * decay;
        m_jsGroupFollowPos = m_jsGroupCursorRaw + (offset + temp) * decay;
    }
    const glm::vec3 delta = m_jsGroupFollowPos - m_jsGroupCursorStart;
    std::vector<glm::vec3> current;
    if (!m_ikRig->bodyVolumes().empty()) {
        current.resize(m_bones.size());
        for (std::size_t i = 0; i < m_bones.size(); ++i) {
            current[i] = glm::vec3(m_poseGlobal[i][3]);
        }
    }
    m_jsGroupTargets.resize(secs.size());
    m_jsGroupTargetValid.assign(secs.size(), 1);
    for (std::size_t i = 0; i < secs.size(); ++i) {
        glm::vec3 target = m_jsGroupStartPos[i] + delta;
        // A secondary KNEE over its planted foot lives on its shin's reach of the ankle's pin, as
        // the primary's knee goal does (ikKneeDrag) — and a secondary elbow over a hand on the
        // floor on its forearm's reach of it. Asked 12cm forward at its own height, unprojected,
        // the shin could only follow by lifting the heel: 13mm, where the primary's foot stayed
        // within a millimetre, and both knees pushed forward together came out lopsided. (A
        // secondary's foot never lets go: no lift, no hang — the reach is the whole rule.)
        for (std::size_t p = 0; p < m_ikRig->pins().size(); ++p) {
            if (!m_ikRig->pinUnderJoint(p, secs[i])) {
                continue;
            }
            const glm::vec3& pinTarget = m_ikRig->pins()[p].target;
            const float      reach = glm::length(m_jsGroupStartPos[i] - pinTarget);
            const glm::vec3  out = target - pinTarget;
            const float      dist = glm::length(out);
            if (dist > reach && dist > 1.0e-5f) {
                target = pinTarget + out * (reach / dist);
            }
            break;
        }
        // The floor and the body volumes bound it, as they bound the primary's (dragIkTo).
        target.y = std::max(target.y, -m_transform[3][1] + m_ikRig->floorClearance(secs[i]));
        if (!current.empty()) {
            target = m_ikRig->clampOutOfVolumes(secs[i], target, current);
        }
        m_jsGroupTargets[i] = target;
    }
}

bool Armature::dragIkTick(const glm::vec3& rawTarget) {
    IkRig& rig = *m_ikRig;
    const std::size_t n = m_bones.size();
    const glm::vec3 target = followIkTarget(rawTarget);
    const bool following = glm::length(rawTarget - target) > 1.0e-5f;
    updateGroupTargets(); // (THE GROUP's secondaries: their targets for this tick)
    // The pelvis's VERTICAL YIELD (see kRootYieldStiffness), from the followed target: down
    // always, up by the height the legs had left when the drag began.
    {
        const glm::vec3 moved = target - m_jsStartTarget;
        const float length = glm::length(moved);
        const float scale = rig.sizeScale();
        const auto yieldFor = [&](float travel) {
            const float share = length > 1.0e-4f ? travel / length : 0.0f;
            return glm::smoothstep(kRootYieldDownFrom * scale, kRootYieldDownFull * scale, travel) *
                   glm::smoothstep(kRootYieldShareFrom, kRootYieldShareFull, share);
        };
        static const bool kNoRise = std::getenv("IK_JS_NO_RISE_YIELD") != nullptr;
        const float room = kNoRise ? 0.0f
                                   : glm::smoothstep(kRootYieldRoomFrom * scale, kRootYieldRoomFull * scale,
                                                     m_jsRootRiseRoom);
        // RISING reads the upward travel alone, not its share of the whole: getting up out of a
        // kneel or a sit travels a third to half a metre sideways too, and on the rigs whose kneel
        // reaches furthest forward the share gate held the rise at 0.7 — the pelvis, the soles and
        // the heels came 70% of the way home, and she stood on bent knees with her heels 3cm up.
        // (The share gate is the push-down's: a sideways pull must not sink the hips. A pull that
        // goes 20cm UP out of a low pose is getting up, whatever else it does.)
        // ... the BODY's upward travel, that is. A trunk joint's or the pelvis's own is the
        // cursor's. A LIMB's is not: a kneeling figure whose hand was raised 40cm — a reach the
        // arm has by itself — read as 40cm of getting up, and with the pelvis freed, its
        // references turned upright and the legs sent home she rose 28cm off her knees to wave;
        // a knee drawn up and forward for a half kneel stood her up the same way. For a limb
        // the rise is what the limb CANNOT give: how far the target lies beyond the limb's reach
        // of its socket as the drag found it, the upward share of that. (A function of the
        // target still.)
        float rose = std::max(0.0f, moved.y);
        // (... and over the LOWEST the target has been in this drag: see m_jsContactRise.)
        m_jsLowestTargetY = std::min(m_jsLowestTargetY, target.y);
        float roseAgain = std::max(0.0f, target.y - m_jsLowestTargetY);
        const int effector = rig.dragEffector();
        static const bool kLimbRises = std::getenv("IK_JS_LIMB_RISES") != nullptr; // A/B probe
        // (A limb's joint: one without the mass of a trunk joint below it — or a FOLD joint, or a
        // foot's: a shin with its foot weighs in as trunk-class, and a knee is no less a limb's.)
        bool limb = effector != rig.rootNode() && static_cast<std::size_t>(effector) < m_jsStartPos.size();
        if (limb && rig.effectorIsTrunk()) {
            limb = rig.isFootClass(effector) || foldAxisOf(effector) >= 0;
        }
        float limbBeyondUp = 0.0f; // (what the limb cannot give UPWARD: THE TIPTOE reads it below)
        if (!kLimbRises && limb) {
            if (m_jsLimbSocket < 0) {
                const int junction = rig.limbJunction(effector);
                int socket = effector;
                float reach = 0.0f;
                while (m_bones[static_cast<std::size_t>(socket)].parent >= 0 &&
                       m_bones[static_cast<std::size_t>(socket)].parent != junction) {
                    reach += glm::length(glm::vec3(m_bones[static_cast<std::size_t>(socket)].localBind[3]));
                    socket = m_bones[static_cast<std::size_t>(socket)].parent;
                }
                m_jsLimbSocket = socket;
                m_jsLimbReach = reach;
            }
            const glm::vec3 out = target - m_jsStartPos[static_cast<std::size_t>(m_jsLimbSocket)];
            const float dist = glm::length(out);
            const float beyond = std::max(0.0f, dist - kLimbReachShare * m_jsLimbReach);
            const float beyondUp = dist > 1.0e-4f ? beyond * std::max(0.0f, out.y) / dist : 0.0f;
            rose = std::min(rose, beyondUp);
            roseAgain = std::min(roseAgain, beyondUp);
            // (THE TIPTOE reads the target beyond the limb's FULL length: kTiptoeReachShare.)
            limbBeyondUp = dist > 1.0e-4f ? std::max(0.0f, dist - kTiptoeReachShare * m_jsLimbReach) * std::max(0.0f, out.y) / dist : 0.0f;
        }
        // ... and a trunk joint of a figure SITTING OR LYING ON THE FLOOR is, to her seat, what a
        // hand is to its shoulder: taken up, the trunk first comes up ABOUT the seat — she sits up,
        // the seat's roll undone (solveIk) — and the body rises by what the trunk cannot give, the
        // target's distance beyond the trunk's reach of the seat as the drag found it. Read as the
        // cursor's whole upward travel, a figure lying on her back and taken up by the chest was
        // "getting up" from the first 20cm: the pelvis freed and sent upright, the hips drawn in
        // under a chest still behind them — 30cm in one tick — and she never sat up.
        static const bool kSeatRises = std::getenv("IK_JS_SEAT_RISES") != nullptr; // A/B probe
        if (!kSeatRises && m_jsFloorSeatStart && !limb && effector != rig.rootNode() &&
            static_cast<std::size_t>(rig.rootNode()) < m_jsStartPos.size()) {
            // (The seat at the height it SITS at — the hip joint over the flesh under an upright
            // pelvis: lying down it rests 10cm lower, and measured from there a plain sit-up read
            // as 8cm of getting up.)
            const glm::vec3 seat = m_jsStartPos[static_cast<std::size_t>(rig.rootNode())];
            const float sits = std::max(seat.y, -m_transform[3][1] + rig.floorClearanceAlong(rig.rootNode(), glm::vec3(0.0f, -1.0f, 0.0f)));
            const glm::vec3 out = target - glm::vec3(seat.x, sits, seat.z);
            const float far = glm::length(out);
            const float beyond = std::max(0.0f, far - kLimbReachShare * glm::length(m_jsStartTarget - seat));
            rose = std::min(rose, far > 1.0e-4f ? beyond * std::max(0.0f, out.y) / far : 0.0f);
            roseAgain = std::min(roseAgain, far > 1.0e-4f ? beyond * std::max(0.0f, out.y) / far : 0.0f);
            // ... and a figure LYING DOWN is sat up first: a drag that found the trunk more than 45
            // degrees from upright does not stand her up, however far it goes (the next one, from
            // the sit, does). Out of a lying pose the rise frees the pelvis, turns it upright and
            // draws the hips in under a chest that is still behind them, all in the ticks it
            // takes the cursor to leave the trunk's reach: 16-20cm pops at the head, the hips
            // hauled 60cm along the floor.
            const glm::vec3 trunk = m_jsStartTarget - seat;
            const float tilt = glm::length(trunk) > 1.0e-4f ? std::acos(glm::clamp(trunk.y / glm::length(trunk), -1.0f, 1.0f)) : 0.0f;
            rose *= 1.0f - glm::smoothstep(glm::radians(25.0f), glm::radians(45.0f), tilt);
            roseAgain *= 1.0f - glm::smoothstep(glm::radians(25.0f), glm::radians(45.0f), tilt);
        }
        // ... and ON HER KNEES the same: a kneeling figure's trunk comes up ABOUT the hips over the
        // planted knees before the body rises — a chest lifted off all fours kneels her upright
        // (the kneel-start hinge, solveIk) — and the body rises by what the trunk cannot give
        // about the hips at their KNEELING height (IkRig::kneelSeatHeight: over the knees with
        // the thighs upright — all fours has the hips there already, a child's pose 15cm lower);
        // found more than 45 degrees from upright, the trunk is kneeled up, not stood up, by that
        // drag. Read as the cursor's whole upward travel, a 26cm chest lift off all fours was
        // getting up from its first centimetre: the knees unloaded and came 6cm off the floor,
        // the legs were sent home and the hips drawn under the chest — she rose toward her feet
        // with her trunk still folded, no kneel between. IK_JS_KNEEL_RISES restores it.
        static const bool kKneelRises = std::getenv("IK_JS_KNEEL_RISES") != nullptr; // A/B probe
        if (!kKneelRises && m_jsKneelStart && !m_jsFloorSeatStart && !limb && effector != rig.rootNode() &&
            static_cast<std::size_t>(rig.rootNode()) < m_jsStartPos.size()) {
            const glm::vec3 hips = m_jsStartPos[static_cast<std::size_t>(rig.rootNode())];
            const float     kneels = std::max(hips.y, m_jsKneelSeatY);
            const glm::vec3 seat(hips.x, kneels, hips.z);
            const glm::vec3 out = target - seat;
            const float     far = glm::length(out);
            const float     beyond = std::max(0.0f, far - kLimbReachShare * glm::length(m_jsStartTarget - seat));
            const float     beyondUp = far > 1.0e-4f ? beyond * std::max(0.0f, out.y) / far : 0.0f; // (the UPWARD share of the excess)
            rose = std::min(rose, beyondUp);
            roseAgain = std::min(roseAgain, beyondUp);
            const glm::vec3 trunk = m_jsStartTarget - seat;
            const float     tilt = glm::length(trunk) > 1.0e-4f ? std::acos(glm::clamp(trunk.y / glm::length(trunk), -1.0f, 1.0f)) : 0.0f;
            const float     kneelUp = glm::smoothstep(glm::radians(25.0f), glm::radians(45.0f), tilt);
            rose *= 1.0f - kneelUp;
            roseAgain *= 1.0f - kneelUp;
            // ... and THE KNEES STAY DOWN meanwhile (m_jsKneelHold, a ceiling on each knee contact:
            // solveIk): a live contact holds its joint in the floor's plane and nothing holds it
            // DOWN, and with the rise off the solve's cheap way to a chest asked 40cm up off all
            // fours was to straighten the legs off the toes — the knees 4.5cm in the air under a
            // trunk still leaning. Held while the trunk can give what is asked (the target within
            // its reach about the seat, let go of over the 5cm beyond — the UPWARD excess only,
            // since 2026-09-25: a chest taken forward and down past a kneeling trunk's reach is a
            // body going onto its hands and on toward the floor, whose knees stay where they are;
            // let go of on ANY excess, a toon character's shorter trunk ran out 13cm before the
            // gallery's chest drag did, the knees came 11cm off the floor and she stood in a plank
            // on her hands and toes — a pose no kneeler is pulled into — and the chest lifted off
            // it afterwards threw a finger 42cm in a tick), and for the whole drag when the trunk
            // began folded: that drag kneels her up, the next stands her.
            m_jsKneelHold = 1.0f - glm::smoothstep(0.0f, 0.05f * scale, beyondUp) * (1.0f - kneelUp);
            // ... and THE HIPS COME UP TO THE SEAT as the trunk comes up (m_jsKneelSeatLift, the
            // root's home in solveIk; 2026-09-23): on all fours the hips sit 9-11cm BELOW the
            // kneeling seat height (the thighs lean), and with the rise nil — the kneeling chest
            // is within reach about the seat AT the seat's height — nothing freed the root's
            // vertical price: the hips came up only as far as the cursor's bounded pull dragged
            // them against 2000/m^2, 2-3cm, and the chest rested 23mm short of its cursor on the
            // base rig, 50-59 on two heavy characters (the sweep's). The home rises to the seat
            // by the TARGET's uprightness about it (none while the target lies level with the
            // seat, all once it stands within kKneelSeatUprightDeg of straight up): a function of
            // the target, and a chest taken back down keeps its hips low. IK_JS_NO_KNEEL_SEAT_LIFT.
            static const bool kNoSeatLift = std::getenv("IK_JS_NO_KNEEL_SEAT_LIFT") != nullptr; // A/B probe
            const float tiltNow = far > 1.0e-4f ? std::acos(glm::clamp(out.y / far, -1.0f, 1.0f)) : glm::half_pi<float>();
            const float upright = 1.0f - glm::smoothstep(glm::radians(kKneelSeatUprightDeg), glm::radians(kKneelSeatLevelDeg), tiltNow);
            // (Its HEIGHT only. On all fours the hips sit 13-15cm BEHIND the knees on most rigs (24cm
            // AHEAD on the oldest), so rising over planted knees is a move along the thigh's ARC,
            // and the thighs' length stops the hips 3-6cm under the seat where they stand — the
            // kneel's own geometry, which the trunk's tilt makes up: 14mm on the base rig. Three
            // horizontal homes were tried and put back the same day, each measured on six rigs:
            // OVER THE KNEES (IkRig::kneelSeat as a point: the trunk leaned forward to keep the
            // chest at its target, which is where the KNEEL had it and not over the knees either —
            // pelvis 26-39 degrees forward, the chest 38-166mm short, a half-size character's knees
            // 32cm in the air); UNDER THE TRUNK'S TARGET by the bind's chest-over-root offset, as the
            // standing rise draws them (a kneeling trunk's target sits well behind an upright
            // trunk's: pelvis 22-28 forward, 167mm short on the newest generation, 429 on the
            // half-size one); and the horizontal price simply YIELDED to the uprightness (the hips
            // drifted where the pull took them, back and down on the oldest: 78mm short).)
            glm::vec3 toSeat(0.0f, std::max(0.0f, m_jsKneelSeat.y - hips.y), 0.0f);
            m_jsKneelSeatShift = kNoSeatLift ? glm::vec3(0.0f) : toSeat * upright;
            m_jsKneelUpright = kNoSeatLift ? 0.0f : upright;
            static const bool kKneelUpTrace = std::getenv("IK_JS_KNEELUP_TRACE") != nullptr;
            if (kKneelUpTrace) {
                std::fprintf(stderr, "[kneel-up] hips.y %.4f seatY %.4f target(%.3f %.3f %.3f) far %.3f reach %.3f beyond %.4f tilt %.1f now %.1f kneelUp %.2f hold %.2f lift %.4f", hips.y, m_jsKneelSeatY, target.x, target.y, target.z, far, kLimbReachShare * glm::length(m_jsStartTarget - seat), beyond, glm::degrees(tilt), glm::degrees(tiltNow), kneelUp, m_jsKneelHold, m_jsKneelSeatShift.y);
                std::fputc(10, stderr);
            }
            // ... and the HANDS unload by the trunk's upward travel (solveIk, the live contacts):
            // the kneel-up has no rise of its own to unload them with. Over its FIRST centimetres
            // (kKneelUpUnloadCm), while the shoulder still stands over the hand: faded over the
            // rise's 5-20cm the hand stayed nailed until the last decade of its rows and sprang to
            // hang under a shoulder that had gone 20cm — 137-303mm at the fingertips in one tick.
            m_jsKneelUpRise = glm::smoothstep(0.0f, kKneelUpUnloadCm * scale, std::max(0.0f, moved.y));
        } else {
            m_jsKneelHold = 0.0f;
            m_jsKneelUpRise = 0.0f;
            m_jsKneelSeatShift = glm::vec3(0.0f);
            m_jsKneelUpright = 0.0f;
        }
        if (rig.seatPin() >= 0) {
            rose = 0.0f; // (THE SEAT, see solveIk: a pinned pelvis goes nowhere, and nothing turns home)
            roseAgain = 0.0f;
        }
        if (m_ikScope == IkScope::Chain) {
            rose = 0.0f; // (a SCOPED drag: the chain alone moves; nothing rises, nothing turns home)
            roseAgain = 0.0f;
        }
        m_jsRootRise = glm::smoothstep(kRootYieldDownFrom * scale, kRootYieldDownFull * scale, rose) * room;
        m_jsContactRise = glm::smoothstep(kRootYieldDownFrom * scale, kRootYieldDownFull * scale, roseAgain);
        // (ON HER KNEES a push DOWN is a push down whatever else it does: the share gate is a
        // standing body's, so that a sideways pull keeps its hips — a kneeling body's hips go
        // where the trunk takes them, and pushed from all fours toward the floor the chest
        // goes as far forward as down.)
        m_jsKneelYield = m_jsKneelStart ? glm::smoothstep(kRootYieldDownFrom * scale, kRootYieldDownFull * scale, std::max(0.0f, -moved.y)) : 0.0f;
        m_jsRootYield = std::max(yieldFor(std::max(0.0f, -moved.y)), m_jsRootRise);
        m_jsReachDown = yieldFor(std::max(0.0f, -moved.y)); // (REACHING DOWN, ikTrunkPolicy: the push-down alone, without the rise)
        m_jsReachTravel = std::max(0.0f, -moved.y);
        // A BODY COMING DOWN (the target below where the drag began, by its DOWNWARD travel
        // alone: kHandCeilingDownFrom..Full) stands the slack hands' CEILINGS down — see the
        // live-contact block in solveIk. A function of the target.
        m_jsTargetDown = glm::smoothstep(kHandCeilingDownFrom * scale, kHandCeilingDownFull * scale, std::max(0.0f, -moved.y));
        // How far ALONG the rise is: the height regained, as a share of the height the legs had
        // left when the drag began. m_jsRootRise says the body is getting up — it is full after
        // 20cm, and it frees the pelvis; THIS says how much of the way up she is, and it is what
        // turns the pelvis upright, the legs home, the soles flat and the hips under a grabbed
        // trunk joint. On the one factor those all snapped home in the first 20cm of a rise: a
        // foot asked flat with its knee still on the floor, and — pulled up out of a sit by the
        // head — hips asked directly under a head that was still reclined 30 degrees behind
        // them, which is backward, away from the planted feet: the whole pose locked.
        const float regained = rose / std::max(m_jsRootRiseRoom, 0.05f * scale);
        m_jsRiseProgress = room * glm::smoothstep(0.0f, 1.0f, glm::clamp(regained, 0.0f, 1.0f));
        // THE TIPTOE (2026-09-25; kTiptoeDeg): a hand pulled UP beyond the arm's reach lifts the
        // HEELS — the body rises onto the balls of the feet, as a person reaching a high shelf
        // does, before the lift-off (IkRig::updateIntent, 15cm beyond reach for a quarter of a
        // second) takes her off the floor. Between the arm going straight and the lift-off
        // nothing gave: the soft heel rows (2 x 2000/m^2) and the root's vertical price (1000)
        // against the cursor's bounded pull are 15mm of heel, and a hand pulled 10cm beyond
        // reach rested 97mm short with the feet flat. A price only prefers less travel; the
        // root's HOME rises (ikPostureModel) and each heel's target with it (ikPinRows) — by
        // what the limb cannot give upward (limbBeyondUp, the limb rise's own measure), less
        // the height the legs had left to give (a crouched start stands up first), up to the
        // heel lift kTiptoeDeg of plantar flexion about the ball gives, read off the feet's own
        // geometry at bind once the solve has found the balls (the first tick's rows). A
        // function of the target: the hand brought back within reach puts the heels down.
        // Only a drag from a two-footed stance (m_jsTiptoeDrag), never a foot's, never while
        // suspended.
        static const bool kNoTiptoe = std::getenv("IK_JS_NO_TIPTOE") != nullptr; // A/B probe
        static const double kTiptoePitch = envOr("IK_JS_TIPTOE_DEG", kTiptoeDeg);
        m_jsTiptoe = 0.0f;
        m_jsTiptoeTheta = 0.0f;
        const bool footEffector = rig.isFootClass(effector);
        if (!kNoTiptoe && m_jsTiptoeDrag && limb && !footEffector && !rig.suspended() && limbBeyondUp > 0.0f) {
            const float pitch = glm::radians(static_cast<float>(kTiptoePitch));
            const auto liftAt = [&](float t) { return m_jsTiptoeD * std::sin(t) + m_jsTiptoeH0 * (std::cos(t) - 1.0f); };
            if (m_jsTiptoeRoom < 0.0f) {
                float d = 0.0f, h0 = 0.0f;
                int   feet = 0;
                bool  pending = false;
                for (const IkEffector& pin : rig.pins()) {
                    if (pin.node < 0 || static_cast<std::size_t>(pin.node) >= m_jsBall.size()) {
                        continue;
                    }
                    const int ball = m_jsBall[static_cast<std::size_t>(pin.node)];
                    if (ball == -2) {
                        pending = true; // (not looked for yet: the first solve's rows find them)
                        break;
                    }
                    if (ball < 0) {
                        continue;
                    }
                    const glm::vec3 off = m_ikBindPos[static_cast<std::size_t>(pin.node)] - m_ikBindPos[static_cast<std::size_t>(ball)];
                    d += glm::length(glm::vec2(off.x, off.z));
                    h0 += off.y;
                    ++feet;
                }
                if (!pending) {
                    if (feet > 0) {
                        m_jsTiptoeD = d / static_cast<float>(feet);
                        m_jsTiptoeH0 = h0 / static_cast<float>(feet);
                        m_jsTiptoeRoom = std::max(0.0f, liftAt(pitch));
                    } else {
                        m_jsTiptoeRoom = 0.0f; // (no ball found under either foot: rigid ankle pins, no heel to lift)
                    }
                }
            }
            if (m_jsTiptoeRoom > 0.0f) {
                const float want = glm::clamp(limbBeyondUp - std::max(0.0f, m_jsRootRiseRoom), 0.0f, m_jsTiptoeRoom);
                float lo = 0.0f, hi = pitch; // (the lift is monotonic in the pitch over [0, kTiptoeDeg])
                for (int i = 0; i < 24; ++i) {
                    const float mid = 0.5f * (lo + hi);
                    (liftAt(mid) < want ? lo : hi) = mid;
                }
                m_jsTiptoeTheta = 0.5f * (lo + hi);
                m_jsTiptoe = want;
            }
            static const bool kTiptoeTrace = std::getenv("IK_JS_TIPTOE_TRACE") != nullptr;
            if (kTiptoeTrace) {
                std::fprintf(stderr, "[tiptoe] beyondUp %.4f legRoom %.4f room %.4f (d %.4f h0 %.4f) lift %.4f pitch %.1f\n",
                             limbBeyondUp, m_jsRootRiseRoom, m_jsTiptoeRoom, m_jsTiptoeD, m_jsTiptoeH0, m_jsTiptoe,
                             glm::degrees(m_jsTiptoeTheta));
            }
        }
    }
    std::vector<glm::vec3>& positions = m_jsPositionsBuf; // (kept across ticks: no allocation)
    positions.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        positions[i] = glm::vec3(m_poseGlobal[i][3]);
    }
    syncRigRotations(); // (the floor clearances of this tick: along DOWN in each bone's frame)
    if (!m_jsSettling) {
        rig.updateIntent(target, positions); // down / up intent, the live pins' bounds, lift-off
    }
    // A LIFT takes time. The lift-off test has just decided the figure hangs from the grabbed
    // joint — and the cursor is by then as far beyond her reach as it took to decide: 30-40cm.
    // Solved in full, that was the whole body 42cm up in ONE tick (59cm at a fingertip). What
    // the solve is given is the cursor less the DEFICIT the lift began with, paid off at
    // kLiftRiseStep a tick: she comes up off the floor over a quarter of a second. And the
    // HANG the same way: the root is hung straight below the cursor, which for a hand raised
    // out at the body's side is half a metre from where the pelvis stands — asked there at
    // once the whole body went sideways in the same tick (a toe 71cm). The hang's target starts
    // where the root IS and swings in under the hand at the same pace.
    glm::vec3 goal = target;
    const bool liftRising = rig.suspended() && !kNoLiftEase; // (the hips' own lift-off eases in the same way, 2026-11-07: its deficit is the strain the pins stopped it at, the hang's nothing)
    if (liftRising) {
        const float step = kLiftRiseStep * rig.sizeScale();
        const auto payOff = [step](glm::vec3& owed) {
            const float left = glm::length(owed);
            owed = left > step ? owed * ((left - step) / left) : glm::vec3(0.0f);
        };
        if (!m_jsLiftEasing) {
            m_jsLiftEasing = true;
            m_jsLiftDeficit = positions[static_cast<std::size_t>(rig.dragEffector())] - target;
            m_jsHangDeficit = positions[static_cast<std::size_t>(rig.rootNode())] -
                              (target + m_jsLiftDeficit - glm::vec3(0.0f, rig.suspendHang(), 0.0f));
        } else {
            payOff(m_jsLiftDeficit);
            payOff(m_jsHangDeficit);
        }
        goal += m_jsLiftDeficit;
    } else {
        m_jsLiftEasing = false;
        m_jsLiftDeficit = glm::vec3(0.0f);
        m_jsHangDeficit = glm::vec3(0.0f);
    }
    // THE DANGLE (kDangle*, 2026-11-02): how far the suspended body has gone over to HANGING — the legs
    // plumb from the hips, the knees soft, the feet pointed, the free arm plumb (ikPostureDofs makes
    // the references, the hang reads the share). It eases in over half a second of suspension and out
    // again at the same pace once she lands, so nothing pops either way; a figure lifted and kept her
    // standing pose in the air before (the legs apart in the stance, the free arm out at the A-pose
    // angle), a mannequin on a hook. IK_JS_NO_DANGLE is the A/B.
    {
        static const bool kNoDangle = std::getenv("IK_JS_NO_DANGLE") != nullptr; // A/B probe
        m_jsDangle = kNoDangle ? 0.0f : glm::clamp(m_jsDangle + (rig.suspended() ? kDangleStep : -kDangleStep), 0.0f, 1.0f);
    }
    m_jsSolveGoal = goal;
    bool moved = solveIk(&goal, nullptr);
    m_jsPostStepMoving = false;
    if (m_ikScope != IkScope::Chain) { // (a SCOPED drag moves the chain alone: no arm hangs, no head is righted)
        const bool hangMoved = hangIdleArms();  // (see IDLE ARMS HANG above: before the contacts read the pose)
        const bool headMoved = rightIdleHead(); // (see THE HEAD STAYS UP)
        // (THE HANGING FEET COME DOWN FLAT, armaturehangfeet.cpp: a floating figure's, as the floor
        // nears — a function of the pose, so nothing the settle has to wait for.)
        const bool feetMoved = landHangFeetByFloor();
        moved = hangMoved || headMoved || feetMoved || moved;
        m_jsPostStepMoving = hangMoved || headMoved; // (the settle waits for them: settleIkSolveTick)
    }
    m_jsSoftBodyMoving = false;
    if (softbody::breastMotion() || softbody::buttockMotion()) {
        // THE SOFT BODY (armaturebreasts.cpp, armaturebuttocks.cpp): after the solve and the post-steps. Its motion is
        // NOT a post-step for the settle's purposes — folded into m_jsPostStepMoving it kept the
        // settle SOLVING while they bounced, and the pose a drag ended in then depended on how long
        // they bounced (a calmer spring moved a knife-edge prone-up's hand 2 mm past its gate); the
        // settle ticks them alone once the pose is at rest (settleIkSolveTick's tail).
        m_jsSoftBodyMoving = tickSoftBody(1.0f / 60.0f);
        moved = moved || m_jsSoftBodyMoving;
    }

    // LIVE CONTACT RE-DETECTION and landings (see IkRig::updateContacts), on the solved pose.
    for (std::size_t i = 0; i < n; ++i) {
        positions[i] = glm::vec3(m_poseGlobal[i][3]);
    }
    syncRigRotations(); // (the contacts are read off the SOLVED pose: its clearances, not the tick's first)
    // A knee still coming down is planted where it IS (see the hard live pins in solveIk).
    for (std::size_t p = 0; p < rig.pins().size() && !kKneeLandingHeld; ++p) {
        const IkEffector& pin = rig.pins()[p];
        if (!rig.pinIsLive(p) || !pin.hard || rig.pinUnderEffector(p) || pin.node < 0) {
            continue;
        }
        const std::size_t at = static_cast<std::size_t>(pin.node);
        if (at < m_jsContactLanding.size() && m_jsContactLanding[at] &&
            positions[at].y - pin.target.y <= 0.003f * rig.sizeScale()) {
            rig.reseatLivePin(p, positions[at].x, positions[at].z); // landed: this is its place
            m_jsPinTarget[at].x = positions[at].x;
            m_jsPinTarget[at].z = positions[at].z;
            m_jsContactLanding[at] = 0;
        }
    }
    // (Tried and taken out, 2026-09-28: a live contact whose faded hold RETURNS — its joint back on
    // the floor after a centimetre's rise — re-seated where the joint stands, its fingertip's height
    // row eased back down from where the tip stood. It was the first reading of a custom character's
    // prone pop (the hands 47mm from their pins as the hold came back), and it cut the pop from 258mm
    // to 190; the cause was the solver's — a trial judged by the rows of the pose it had left, see
    // JointSolver::descend — and with that mended the re-seat only made a reclining sit's hold jump
    // 39mm where it jumps 8 on two rigs, and a prone descent 61 where it jumps 29.)
    std::vector<char>& heldBefore = m_jsHeldBeforeBuf;
    heldBefore.assign(m_jsHangArms.size(), 0);
    for (std::size_t a = 0; a < m_jsHangArms.size(); ++a) {
        heldBefore[a] = hangArmHeld(m_jsHangArms[a]) ? 1 : 0;
    }
    rig.setRiseUnloadedContacts(m_jsContactRiseUnloaded); // (a hand the rise unloaded lifts off like a taut one)
    {
        // (A KNEE whose lying foot is still rolling onto its toes keeps its pin until the handover —
        // IkRig::setLiftOffDeferred: the foot's soft hold in place went with the pin, and the foot slid
        // 10cm while the thigh splayed to its box in one tick, 122mm at the knee, standing up off the
        // heels. The knee's ceiling and the roll pace it meanwhile — the flat block of ikPinRows.)
        std::vector<char>& deferred = m_jsDeferredBuf;
        deferred.assign(n, 0);
        for (std::size_t f = 0; f < n && f < m_jsFootKnee.size() && f < m_jsFootWasFlat.size() && f < m_jsFootUnrolled.size(); ++f) {
            if (m_jsFootWasFlat[f] && !m_jsFootUnrolled[f] && m_jsFootKnee[f] >= 0 && static_cast<std::size_t>(m_jsFootKnee[f]) < n) {
                deferred[static_cast<std::size_t>(m_jsFootKnee[f])] = 1;
            }
        }
        rig.setLiftOffDeferred(deferred);
    }
    if (rig.updateContacts(positions) && rig.takeLanded()) {
        captureFlatHold(true);
    }
    // A contact that has just formed in an arm that HUNG is an accident of the pose, not a hand the
    // user put down: remembered across drags (m_incidentalContact; IkRig::setIncidentalContacts).
    for (std::size_t a = 0; a < m_jsHangArms.size(); ++a) {
        // (An arm the hang admitted THIS tick — its hand a live contact already — was held before it.)
        const bool before = a < heldBefore.size() ? heldBefore[a] != 0 : true;
        const bool now = hangArmHeld(m_jsHangArms[a]);
        if (before && !now) {
            m_jsHangArms[a].released = true; // (its contact lifted: the hang's, and no floor rows — HangArm::released)
        }
        if (before || !now) {
            continue;
        }
        m_jsHangArms[a].landed = true;
        if (m_incidentalContact.size() != n) {
            m_incidentalContact.assign(n, 0);
        }
        for (std::size_t p = 0; p < rig.pins().size(); ++p) {
            for (int cur = rig.pins()[p].node; cur >= 0 && rig.pinIsLive(p); cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                if (cur == m_jsHangArms[a].socket) {
                    m_incidentalContact[static_cast<std::size_t>(rig.pins()[p].node)] = 1;
                    break;
                }
            }
        }
    }
    // (The step policy reads the target the solve WORKED TO: a knee's or an elbow's is taken onto
    // its reach — 14cm from the cursor, by design, for an elbow raised beside the body — and read
    // off the cursor that distance was a body the stance could not follow: she took a step
    // sideways after her own elbow.)
    // (... and under a trunk follow, where the solve's reference is taking the HIPS.)
    glm::vec2 bodyAnchor(0.0f);
    const bool followed = glm::length(m_jsTrunkFollow) > 1.0e-4f &&
                          static_cast<std::size_t>(rig.rootNode()) < m_jsStartPos.size();
    if (followed) {
        const glm::vec3& rootStart = m_jsStartPos[static_cast<std::size_t>(rig.rootNode())];
        bodyAnchor = glm::vec2(rootStart.x + rig.stanceShift().x, rootStart.z + rig.stanceShift().z) + m_jsTrunkFollow;
    }
    if (m_jsSettling) {
        return moved || m_jsPinsEasing; // (the settle: no new steps — see settleIkSolveTick)
    }
    rig.updateStepPolicy(rig.dragEffector() != rig.rootNode() ? m_jsSolveGoal : target,
                         balanceIsBlind() ? balancePositions() : positions, // (blind to the arms' hang, the head's righting)
                         followed ? &bodyAnchor : nullptr);
    return moved || m_jsPinsEasing || following || rig.steppingPin() >= 0 ||
           glm::length(m_jsLiftDeficit) > 0.0f || glm::length(m_jsHangDeficit) > 0.0f;
}

bool Armature::settleIkSolveTick() {
    // The pose already satisfies its pins when the button comes up — there is nothing to land
    // — unless a pin target is still being approached (a hovering figure healing onto the
    // floor): that finishes here, with a placed hand or foot held where it was let go.
    // A step still in flight at mouse-up lands in TARGET terms (IkRig::landPendingStep), and
    // the foot is then brought down onto its spot at the pin-easing pace.
    //
    // The settle IS THE DRAG, continued under a still cursor: the same tick (dragIkTick) toward
    // the followed target the last drag tick solved to — every reference and row the pose was
    // made with, the arms' hang and the head's righting too — with the rig reading no new intent
    // and beginning no new step. It used to be a solve of its own, with no target at all for a
    // trunk drag ("trunk effectors are free in the settle", from the position-space solver's
    // landing round): every posture term pulls to the drag-start pose, so a bow let go of while a
    // step was in the air SPRANG BACK 45 degrees toward upright as the foot came down — with her
    // arms and head riding, since the post-steps did not run — and the next drag found arms
    // pointing forward that no longer counted as hanging: taken back up, she stood with both
    // arms straight out in front of her (found by the in-app two-drag script, 2026-09-20; the
    // harness's phases let go after a hold, when no step is in flight).
    if (!m_jsHoldValid) {
        m_jsHold = glm::vec3(m_poseGlobal[static_cast<std::size_t>(m_ikRig->dragEffector())][3]);
        m_jsHoldValid = true;
        m_jsSettleTarget = m_jsFollowValid ? m_jsFollowPos : m_jsHold;
        m_jsFollowVel = glm::vec3(0.0f);
        if (m_ikRig->steppingPin() >= 0) {
            std::vector<glm::vec3> positions(m_bones.size());
            for (std::size_t i = 0; i < m_bones.size(); ++i) {
                positions[i] = glm::vec3(m_poseGlobal[i][3]);
            }
            m_ikRig->landPendingStep(positions);
            m_jsPinsEasing = true;
        }
    }
    // (... and while a POST-STEP is still turning, 2026-10-27 — the arms' hang, the head's righting:
    // a hand that let go of the floor in the drag's last ticks was left mid-swing at the mouse-up,
    // the arm pointing forward-down with the fingertips on the floor, and stayed there. The hang
    // steps 4 degrees a tick to a direction it reaches exactly, so this ends; the 120-tick cap stands.
    // IK_JS_SETTLE_PINS_ONLY restores the settle that ended with the pins.)
    static const bool kPinsOnly = std::getenv("IK_JS_SETTLE_PINS_ONLY") != nullptr; // A/B probe
    const bool        postStepsMoving = !kPinsOnly && m_jsPostStepMoving;
    if (++m_ikSettleTicks > 120) {
        return false;
    }
    if (!m_jsPinsEasing && !postStepsMoving) {
        // THE SOFT BODY'S TAIL: the pose is at rest; the breasts, if they still bounce, tick on by
        // themselves — no solve, so the pose does not depend on how long they take (2026-11-05).
        if (m_jsSoftBodyMoving) {
            m_jsSoftBodyMoving = tickSoftBody(1.0f / 60.0f);
            return m_jsSoftBodyMoving;
        }
        return false;
    }
    static const bool kOldSettle = std::getenv("IK_JS_OLD_SETTLE") != nullptr; // A/B probe
    if (kOldSettle) {
        solveIk(nullptr, m_ikRig->effectorIsTrunk() ? nullptr : &m_jsHold);
        return m_jsPinsEasing;
    }
    m_jsSettling = true;
    dragIkTick(m_jsSettleTarget);
    m_jsSettling = false;
    return m_jsPinsEasing || (!kPinsOnly && m_jsPostStepMoving) || m_jsSoftBodyMoving;
}

} // namespace pose

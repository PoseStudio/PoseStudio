/**
 * @file jointsolver.h
 * @brief Whole-body JOINT-SPACE inverse kinematics: the pose is solved directly in the engine's
 *        own representation — the per-joint Euler channels plus the solve root's translation —
 *        as one regularized nonlinear least-squares problem, converged every tick.
 *
 * Why it exists. The position-space solver it replaced (a multi-chain FABRIK, deleted on
 * 2026-09-18) placed joint POSITIONS, and the Armature then had to turn those back into Euler
 * channels (the rotation extraction), where the authored limits, the unwitnessed twist and the
 * capped fits all left residuals; because that loop was iterative, redundant and ill-posed, its
 * per-tick output was kept calm by DAMPING (under-relaxation, movement caps, a settle-freeze, a
 * cursor filter), and damping is lag. The two complaints — tremble and lag — were the two ends
 * of that one trade-off.
 *
 * Here there is no trade-off to make, because the problem is WELL-POSED:
 *
 *   minimize   sum_tasks  w_t * |task_t(theta)|^2          (the cursor, the pins, balance)
 *            + sum_dofs   s_i * (theta_i - reference_i)^2   (the posture term)
 *   subject to the authored per-channel limits (a box).
 *
 * The posture term — every channel pulled toward the drag-start pose with a per-channel
 * stiffness — makes the minimizer UNIQUE, so the pose is a (locally) continuous FUNCTION of the
 * targets: a still cursor gives the same pose every tick (nothing to tremble, no freeze to
 * need), a cursor that returns brings the body back, and pixel noise passes through about 1:1
 * instead of being amplified by an iteration that never settles. And since each tick is solved
 * to convergence from the previous tick's pose (a warm start a few Gauss-Newton steps away),
 * the result is applied in FULL: no relaxation, no caps, no lag. The limits are exact because
 * the unknowns ARE the limited channels; there is no extraction.
 *
 * Method: Levenberg-Marquardt on the weighted residuals with an analytic Jacobian (a channel's
 * world axis crossed with the lever to the task point), solved in the DUAL form — the posture
 * term is diagonal and positive, so each step costs one (rows x rows) Cholesky, a few dozen
 * rows, not a (dofs x dofs) one — with the box handled as an active set (a channel at its limit
 * whose step points outward is frozen for that step) and a projected, cost-checked step. The
 * effector row is HUBER-weighted: past a few centimetres of miss its pull stops growing, so an
 * unreachable cursor stretches the body as far as it goes without buying pin slip or posture
 * distortion in proportion to the cursor's distance.
 *
 * All math in double (hard rows weigh 1e7 against posture terms of order one). Qt-free
 * (std + GLM).
 */

#ifndef JOINTSOLVER_H
#define JOINTSOLVER_H

#include "balancecontroller.h"

#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>

namespace pose {

/// One bone of the solver's skeleton: exactly the data the Armature composes a pose from
/// (poseLocal = T(bindOffset + translation) * orient * R(euler, order) * orient^-1).
struct JointSolverBone {
    int         parent = -1;            ///< Parents precede children.
    glm::dvec3  bindOffset{0.0};        ///< The rest transform's translation (parent frame).
    glm::dmat3  orient{1.0};            ///< The oriented rotation frame (columns).
    std::string rotationOrder = "XYZ";
    glm::dvec3  minDeg{0.0};            ///< Authored per-channel limits (degrees) ...
    glm::dvec3  maxDeg{0.0};
    glm::bvec3  limited{false, false, false}; ///< ... and which channels carry them.
};

/// One unknown: an Euler channel of a bone (axis 0-2, the pose's degrees) or a component of a
/// bone's pose translation (axis 3-5 = x/y/z, metres, parent frame).
struct JointSolverDof {
    int    bone = -1;
    int    axis = 0;
    double stiffness = 1.0;  ///< Posture cost per rad^2 (per m^2 for a translation).
    double reference = 0.0;  ///< Posture reference (degrees / metres): the drag-start value.
    /// Bounds TIGHTER than the bone's authored limits (degrees; the defaults leave them).
    double minDeg = -1.0e30;
    double maxDeg = 1.0e30;
    /// A FOLD channel (a knee's or an elbow's flexion): +1 / -1 = the sign of flexion, 0 = not
    /// one. A straight limb is a singular configuration — to first order neither way of bending
    /// it brings its end closer, so the linear model picks a side by noise, and the wrong side
    /// (hyperextension) is a dead end a few degrees deep. The caller bounds such a channel at
    /// straight (minDeg / maxDeg); when a solve ends with such a channel within a few degrees
    /// of straight and a task still unmet, the solver retries from a few degrees of flexion and
    /// keeps whichever pose costs less.
    int    foldSign = 0;
    /// False: bounded at straight like any fold channel, but never KICKED by the retry. The arm of
    /// a hand that has just come down on the floor stands near straight by right - the hand is
    /// rolling onto its palm under a shoulder still on its way down - and kicked there the pose
    /// hopped basins: 17cm at the brow in the tick a figure's hands landed.
    bool   foldRetry = true;
};

/// A point rigidly attached to @p bone (its joint, or @p offset from it in the bone's frame)
/// held at @p target.
struct JointPositionTask {
    int        bone = -1;
    glm::dvec3 offset{0.0};
    glm::dvec3 target{0.0};
    double     weight = 1.0;   ///< Per m^2.
    double     huber = 0.0;    ///< > 0: the row's pull saturates past this miss (metres).
    /// Per world axis multiplier on the miss (0 = that axis is not held): a UNILATERAL contact
    /// holds its place on the floor (x, z) and leaves the height to the floor's one-sided row.
    glm::dvec3 axisScale{1.0};
};

/// @p bone's world rotation held at @p target — uniformly, or with a weight PER AXIS of a world
/// frame (a planted sole: hard in yaw, softer in roll, nearly free in pitch about the ball of
/// the foot — a heel lift).
struct JointOrientationTask {
    int        bone = -1;
    glm::dmat3 target{1.0};
    double     weight = 1.0;          ///< Per rad^2 (the uniform hold).
    bool       perAxis = false;       ///< Use @p frame / @p axisWeights instead of @p weight.
    glm::dmat3 frame{1.0};            ///< Orthonormal world axes (columns) ...
    glm::dvec3 axisWeights{1.0};      ///< ... and the hold's weight about each, per rad^2.
};

/// A ONE-SIDED row: the point on @p bone (its joint + @p offset in the bone's frame) must not
/// sit behind the plane through @p planePoint with outward @p normal — the floor, a body
/// volume's tangent plane. Costs only while violated (a C1 penalty, so the pose stays a
/// continuous function of the targets as the row engages). The plane is fixed in the world, or
/// RIDES @p refBone (its point and normal then given in that bone's frame): a body volume
/// moves with the body, and a row that ignored it would credit a spine rotation — which
/// carries the arm AND the chest it is inside — with resolving the penetration.
struct JointPlaneTask {
    int        bone = -1;
    glm::dvec3 offset{0.0};
    int        refBone = -1;
    glm::dvec3 planePoint{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    double     weight = 1.0;   ///< Per m^2.
};

/// A soft COUPLING of two channels, in joint space: scaleA (a - refA) is held equal to
/// scaleB (b - refB), a and b being problem.dofs[dofA] and [dofB] (radians). The spine's joints
/// bend TOGETHER with it — each by the same share of its range — where left to themselves they
/// are four independent hinges, and an S-curve (the lower spine flexed, the chest extended
/// against it) is as cheap as any other way to put the chest where it is asked.
struct JointCouplingTask {
    int    dofA = -1;
    int    dofB = -1;
    double scaleA = 1.0;
    double scaleB = 1.0;
    double refA = 0.0;   ///< Radians.
    double refB = 0.0;
    double weight = 1.0; ///< Per unit^2 of the scaled difference.
};

/**
 * @class JointSolver
 * @brief The solver: set the skeleton once, then solve() a Problem per tick on a Pose.
 */
class JointSolver {
public:
    struct Pose {
        std::vector<glm::dvec3> eulerDeg;     ///< Per bone (degrees, per world channel).
        std::vector<glm::dvec3> translation;  ///< Per bone pose translation (parent frame).
    };
    struct Frames {
        std::vector<glm::dmat3> rot;          ///< Per bone world rotation.
        std::vector<glm::dvec3> pos;          ///< Per bone world joint position.
    };
    struct Problem {
        std::vector<JointSolverDof>       dofs;
        std::vector<JointPositionTask>    positions;
        std::vector<JointOrientationTask> orientations;
        std::vector<JointPlaneTask>       planes;
        std::vector<JointCouplingTask>    couplings; ///< (their rows come last, after the balance row)
        /// Added to the centre of mass (x, z) before the balance row measures it: what the caller
        /// knows about the weight that the pose does not say (Armature: the idle arms' hang).
        glm::dvec2                        comShift{0.0};
        /// Optional: REGENERATES the plane rows from the current frames at every linearization
        /// (a body volume's tangent plane, the contact normal, follows the joint around the
        /// capsule); `planes` is then only the rows' initial set.
        std::function<void(const Frames&, std::vector<JointPlaneTask>&)> planeSource;
        // BALANCE (optional): the centre of mass' ground projection kept inside the support
        // polygon (counter-clockwise, x = X, y = Z), inset by the margin. One row, one-sided
        // like a plane row: the distance outside the polygon, nothing while balanced.
        const std::vector<float>*     masses = nullptr;  ///< Per bone segment mass.
        const std::vector<glm::vec2>* supportHull = nullptr;
        double                        balanceMargin = 0.0;
        double                        balanceWeight = 0.0; ///< Per m^2 (0 = off).
        /// When non-zero (a unit vector in the floor's plane, x and z): the centre of mass is
        /// balanced ALONG THIS AXIS only — the part of its way back into the support that lies
        /// along it. (A pelvis drag balances fore and aft, with its trunk: sideways is the hip
        /// sway's, whose level chest a sideways lean fights.)
        glm::dvec2                    balanceAxis{0.0, 0.0};
        /// How far outside the polygon the centre of mass may sit for free (metres): the
        /// caller RAMPS a balance requirement in with it when the support changes (a foot
        /// lifted: the polygon shrinks under a body that was balanced a tick ago).
        double                        balanceSlack = 0.0;
    };
    struct Settings {
        int    maxIterations = 12;
        double stepTolerance = 1e-6;     ///< Converged when no DoF moves more (rad / m).
        double maxStepRad = 0.35;        ///< Per-iteration trust bound on a channel's step.
        double maxStepMeters = 0.08;     ///< ... and on a translation's.
        // Per-iteration bounds on what one LINEARIZED step is asked for: a row whose miss is
        // larger asks for this much of it (the linear model is only good over centimetres;
        // asking it for a 20cm balance shift at once produced steps no damping could save).
        double maxTaskStep = 0.05;       ///< Position and plane rows (metres).
        double maxOrientStep = 0.3;      ///< Orientation rows (radians).
        double maxBalanceStep = 0.03;    ///< The balance row (metres).
    };
    struct Result {
        int    iterations = 0;
        double cost = 0.0;
        bool   converged = false;
    };

    void setSkeleton(std::vector<JointSolverBone> bones);
    std::size_t boneCount() const { return m_bones.size(); }
    const JointSolverBone& bone(std::size_t i) const { return m_bones[i]; }

    /// Forward kinematics of @p pose — the Armature's composition, in double.
    void forwardKinematics(const Pose& pose, Frames& frames) const;

    /// Solves @p problem on @p pose in place (the warm start) and leaves the result there.
    Result solve(Pose& pose, const Problem& problem, const Settings& settings) const;

    /// The total cost of @p pose under @p problem (tasks + posture): tests and traces.
    double cost(const Pose& pose, const Problem& problem) const;

    /// Self-test: the largest difference between the analytic Jacobian and a central finite
    /// difference of the residuals at @p pose, over every row and DoF of @p problem (weights
    /// included; the Huber and one-sided rows are differentiated where they are smooth).
    double jacobianError(const Pose& pose, const Problem& problem, int* worstRow = nullptr, int* worstDof = nullptr) const;

    /// True when @p ancestor is @p bone or one of its ancestors.
    bool isAncestorOrSelf(int ancestor, int bone) const {
        return ancestor >= 0 && bone >= 0 &&
               m_tin[static_cast<std::size_t>(ancestor)] <= m_tin[static_cast<std::size_t>(bone)] &&
               m_tin[static_cast<std::size_t>(bone)] < m_tout[static_cast<std::size_t>(ancestor)];
    }

private:
    /// The centre of mass as a linear function of the joint positions (BalanceController's
    /// definition: each bone weighs in at its segment midpoint): per bone, its joint position's
    /// coefficient in the sum, and the total mass.
    struct ComModel {
        std::vector<double>      coeff;
        double                   total = 0.0;
        BalanceController::Inset inset; ///< The support polygon shrunk by the margin, once per descent (balanceNeed reads it).
    };
    ComModel buildComModel(const Problem& problem) const;
    void     buildComModel(const Problem& problem, ComModel& model) const; ///< ... into a reused model.
    /// The balance row: how far (metres) the centre of mass' ground projection sits outside
    /// the inset support polygon, and the unit XZ direction back in (0 when balanced / off).
    double balanceNeed(const Frames& frames, const Problem& problem, const ComModel& com,
                       glm::dvec2& direction) const;

    /// Fills the weighted residuals @p r at @p frames and returns the TASK cost (the Huber
    /// rows at their true cost, which is not their squared weighted residual). With
    /// @p stepBounds the residuals are what one linearized step is ASKED for (each row's miss
    /// bounded, see Settings::maxTaskStep); the returned cost is the true one either way.
    double residuals(const Pose& pose, const Frames& frames, const Problem& problem,
                     const std::vector<JointPlaneTask>& planes, const ComModel& com,
                     std::vector<double>& r, const Settings* stepBounds = nullptr) const;
    /// residuals() plus the Jacobian @p J over problem.dofs (row-major, rows x dofs).
    /// With @p planeClear, a plane row that is CLEAR of its surface (residual 0) gets its Jacobian
    /// row too, and its signed residual (negative: the room it has, in the row's own scale) is
    /// written there, indexed like r (0 for every other row) — what solve() needs to bring a row
    /// in contact into the linear model when a step would cross it.
    double linearize(const Pose& pose, const Frames& frames, const Problem& problem,
                     const std::vector<JointPlaneTask>& planes, const ComModel& com,
                     std::vector<double>& r, std::vector<double>& J,
                     const Settings* stepBounds = nullptr, std::vector<double>* planeClear = nullptr) const;
    double postureCost(const Pose& pose, const Problem& problem) const;
    /// The cost of the problem's SOFT rows: position and orientation rows under kStallRowWeight,
    /// and the couplings - preferences, which rest unmet by design and say nothing of a stall.
    double softCost(const Pose& pose, const Problem& problem) const;
    /// The box of @p problem's DoFs in their own units (radians / metres).
    void dofBounds(const Problem& problem, std::vector<double>& lo, std::vector<double>& hi) const;
    /// One Levenberg-Marquardt descent from @p pose (see solve).
    Result descend(Pose& pose, const Problem& problem, const Settings& settings) const;
    glm::dvec3 dofAxis(const Pose& pose, const Frames& frames, const JointSolverDof& dof) const;

    /// A per-axis orientation row's error frame at the linearization's pose: what every DoF's
    /// column of the row shares (linearize computes it once per row, not once per DoF per row).
    struct OrientationTerms {
        glm::dvec3 lateralNow{0.0};
        glm::dvec3 foreNow{0.0};
        glm::dvec3 f{0.0};
        double     N = 0.0;
        double     D = 0.0;
        double     norm2 = 0.0;
        glm::dvec3 sqrtW{0.0}; ///< per axis: sqrt(axisWeights); .x = sqrt(weight) for a uniform row
    };
    /// The descent's scratch, kept across descents (mutable: a solver serves one descent at a time,
    /// and the Armature's solve is single-threaded). Every buffer is assigned before it is read;
    /// what persists between calls is their capacity, so a tick's dozen descents allocate nothing.
    struct Workspace {
        std::vector<double>           lo, hi, ref, r, rTrial, rLinear, J, planeClear, q, pull, dInv, step, d0, M, rhs;
        std::vector<std::size_t>      act;       ///< descend: the rows IN the model this pass (M and rhsAct are theirs)
        std::vector<double>           Jd, rhsAct; ///< descend: J D^-1 over the active rows; their multipliers
        std::vector<char>             rowOff, rowEntered, blocked;
        std::vector<JointPlaneTask>   planes, trialPlanes;
        Pose                          trial;
        Frames                        frames, trialFrames;
        ComModel                      com;
        std::vector<double>           subCoeff;  ///< linearize: the balance row's subtree sums ...
        std::vector<glm::dvec3>       subMoment;
        std::vector<glm::dvec3>       posX;      ///< linearize: each position row's point ...
        std::vector<double>           posSw;     ///< ... and its weight's root (the Huber and the bounded ask folded in)
        std::vector<OrientationTerms> ori;       ///< linearize: each orientation row's terms
        std::vector<glm::dvec3>       plX, plP, plN; ///< linearize: each plane row's point, plane point and normal
        std::vector<double>           plSw;      ///< ... and its weight's root
    };
    mutable Workspace m_ws;

    std::vector<JointSolverBone>  m_bones;
    std::vector<int>              m_order;    ///< Per bone: 3 axis indices of the rotation order.
    std::vector<int>              m_tin;      ///< DFS entry / exit numbering (ancestor tests).
    std::vector<int>              m_tout;
    std::vector<std::vector<int>> m_children;
};

} // namespace pose

#endif // JOINTSOLVER_H

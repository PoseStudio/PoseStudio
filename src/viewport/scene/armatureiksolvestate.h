/**
 * @file armatureiksolvestate.h
 * @brief The scratch state one tick of Armature::solveIk builds up and hands from stage to stage
 *        (armatureiksolve.cpp): what the drag IS, the unknowns and their prices, the rows, the
 *        solve's result. One instance lives for one call of solveIk; every stage is a member
 *        function over it, and the members are listed in the order the stages set them.
 *
 * A member is here because more than one stage reads it; a stage's own working values stay its
 * locals. A stage begins by naming the members it uses (`float& plant = s.plant;`), so its body
 * reads as it did when the solve was one function, and the compiler flags any it forgot. Private
 * to the solve: only armatureiksolve*.cpp include it. Qt-free (std + GLM).
 */

#ifndef ARMATURE_IK_SOLVE_STATE_H
#define ARMATURE_IK_SOLVE_STATE_H

#include "ikeffector.h"
#include "ikrig.h"
#include "jointsolver.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <tuple>
#include <vector>

namespace pose {

struct IkSolveScratch {
    // --- Given by solveIk ---
    IkRig&                          rig;        ///< The figure's rig (the drag's policy).
    const std::size_t               n;          ///< The bone count.
    const int                       root;       ///< The solve root (IkRig::rootNode).
    const int                       effector;   ///< The dragged joint (IkRig::dragEffector: a token grab promoted).
    const JointSolver&              solver;     ///< The solver built for this skeleton (its ancestry queries).
    const std::vector<IkEffector>&  pins;       ///< The rig's pins (contacts, user pins).
    const std::vector<char>&        active;     ///< Per node: on a path from the effector or a pin to the root.
    const std::vector<int>&         secondaries; ///< THE GROUP's other dragged joints (IkRig::secondaryEffectors): each rides the cursor's displacement with the effector (Armature::m_jsGroupTargets), its limb freed and priced as the dragged one.
    const glm::vec3*                dragTarget; ///< The cursor's target this tick, or null on the release.
    const glm::vec3*                holdTarget; ///< The released joint's hold, or null.

    // --- Set by ikChooseUnknowns ---
    std::vector<char>               dofBone;  ///< Per bone: one of the solve's unknowns (its unlocked channels are DoFs).
    float                           floorModel{};  ///< the floor's height in model space
    float                           rising{};  ///< The body IS getting up (m_jsRootRise, 0-1): it frees the pelvis's prices, its rotation, balance.
    float                           risen{};  ///< How FAR ALONG the rise is (m_jsRiseProgress, 0-1): it turns the references home.
    bool                            rootTurns{};  ///< The root's ROTATION is among the unknowns (a pelvis drag, or a body rising).
    std::size_t                     rootDofMark{};  ///< The root's index: where its rotation DoFs are marked in dofBone.
    // --- Set by ikTrunkPolicy ---
    bool                            swayValid{};  ///< THE HIP SWAY: a pelvis dragged sideways over its standing feet rolls (swayEuler is its reference).
    glm::vec3                       swayEuler{};  ///< ... the root's Euler reference under the sway (the roll taken up).
    float                           pelvisBalance{};  ///< THE HIP HINGE: how far balance is on under a pelvis drag (0-1), served by the trunk.
    float                           hingeSlack{};  ///< what the hinge has LET GO of (a walk, hips leaving the stance): the row's slack
    int                             swayChest{};  ///< The chest bone whose roll the sway keeps LEVEL (-1: none).
    // --- Set by ikStiffnessClasses ---
    std::vector<char>               cls;  ///< Per bone: the stiffness class — 0 = trunk, 1 = the dragged limb, 2 = a limb serving a pin.
    std::vector<char>               twistPriced;  ///< Per bone: carries the limb TWIST price (kPinLimbTwistScale).
    std::vector<char>               landedArm;  ///< the arm of a hand that has come down on the floor
    std::vector<char>               pinJunction;  ///< the bone a pin's limb hangs from (the pelvis bone)
    std::vector<char>               pinNode;  ///< the pinned joints themselves
    std::vector<char>               homeLimb;  ///< a STANDING FOOT's limb: what goes home as the body rises
    std::vector<char>               girdle;  ///< under an elbow drag: the bones above the arm's socket (the collar)
    std::vector<int>                handCollars;  ///< under a HAND drag: the girdle bone each dragged arm's socket hangs from (the collar) — the effector's, and THE GROUP's secondary wrists' — and its ELEVATION channel (the unlocked channel about the fore axis), whose box is capped at kCollarElevationMaxDeg
    std::vector<int>                handCollarAxes;
    // --- Set by ikFootDrag ---
    float                           legScale{};  ///< The rig's size scale (IkRig::sizeScale): every leg-drag length is figure-scaled by it.
    bool                            legDrag{};  ///< The grabbed joint is foot-class (low at BIND): a foot drag.
    const glm::vec3*                goalPoint{};  ///< The target the geometry stages read: the drag's, else the release hold's (may be null).
    float                           footFloor{};  ///< A dragged foot's standing height (its bind height over the model-space floor).
    float                           slide{};  ///< THE SLIDE: how much a dragged foot is on the floor (1 at standing height, 0 from 12cm up).
    // --- Set by ikKneeDrag ---
    bool                            kneeDrag{};  ///< The grabbed joint is a FOLD joint with a foot below it: a knee drag.
    int                             kneeAnkle{};  ///< the first foot-class joint below the dragged knee
    bool                            effectorFolds{};  ///< a hinge: a knee, an elbow
    int                             effectorFoldAxis{};  ///< The effector's fold channel (a hinge), -1 if it is not one.
    int                             kneeFootPin{};  ///< The pin index of the planted foot below a dragged knee (-1: none).
    std::vector<char>               kneeLegBone;  ///< the dragged knee's leg: its twist price comes back as the foot lets go (below)
    float                           plant{};  ///< 1 = the foot below the dragged knee holds; 0 = the leg hangs from it
    float                           kneeLift{};  ///< how far the knee's target is pulled UP off its planted foot (m)
    bool                            lyingKneeFoot{};  ///< the dragged knee's foot was found LYING (a flat kneel's): it rides its shin and hangs clear of the floor (A KNEELING FOOT LIES FLAT)
    float                           lyingFootPitchExcessDeg{};  ///< ... and how far that foot still points DOWN past level (deg): the hang's clearance fades with it
    bool                            flatEasing{};  ///< a foot's flat share or references moved this tick (a roll onto its top, or back): the settle waits for it, as for an easing pin
    glm::vec3                       kneeGoal{};  ///< A dragged knee's goal after its projections (onto the thigh's reach, the lateral clamp, the raise).
    // --- Set by ikElbowDrag ---
    bool                            elbowDrag{};  ///< The grabbed joint is a fold joint that is not a knee: an elbow drag.
    int                             elbowWrist{};  ///< the limb's end below the elbow: past the forearm's twist bones
    int                             elbowHeldPin{};  ///< a pin below the elbow (a user pin, a hand on the floor)
    glm::vec3                       elbowGoal{};  ///< A dragged elbow's goal on the upper arm's reach about its socket.
    // --- Set by ikPostureModel ---
    float                           weightLift{};  ///< THE WEIGHT LIFT this tick: a foot drag's 1 - slide, a knee drag's 1 - plant (the raw share, before the drag-start reference).
    float                           contrapposto{};  ///< CONTRAPPOSTO's share (0-1): the pelvis rolls the free side down under the weight shift, the chest kept level.
    JointSolver::Problem            problem;  ///< THE PROBLEM handed to the solver: DoFs with their stiffness and references, then every row.
    bool                            trunkDrag{};  ///< A trunk-class joint's drag that is not the root's, a knee's, an elbow's or a foot's.
    std::vector<char>               inSpine;  ///< Per bone: a link of the spine chain (m_jsSpineChain), coupled channel by channel.
    double                          seatRollDeg{};  ///< THE SEAT ROLLS: the pelvis's pitch reference turned about the seat by the cursor (degrees).
    // --- Set by ikPinRowsBegin ---
    std::vector<char>               pinned;  ///< Per bone: held by a pin row this tick (the cursor row is skipped on a pinned effector).
    bool                            pinsEasing{};  ///< A pin target is still en route at kPinEaseStep (m_jsPinsEasing after the loop).
    std::array<std::size_t, 4>   kneeFootRows{{0, 0, 0, 0}};  ///< its position rows [0,1) and orientation rows [2,3)
    std::size_t                     kneePlaceRow{};  ///< ... and its ankle's place among them
    bool                            kneeHangMerged{};  ///< The dragged knee's foot has its place spring and its hang as ONE row already (lesson eight).
    std::vector<char>               handTipHeld;  ///< (hands whose fingertip hold stands this tick: see the live contacts)
    std::vector<JointOrientationTask> palmRows;  ///< (... and their palms' pulls, added once the contact's fade is known)
    std::vector<JointPositionTask>  tipRows;  ///< (... and their fingertips' heights)
    std::vector<char>               footFreed;  ///< (see THE FEET COME UP: their floor rows return)
    std::vector<char>               footLying;  ///< (A KNEELING FOOT LIES FLAT: the ankle's and mid-foot's floor rows read standing heights, and are off; the ball's and toes' stay)
    std::vector<std::tuple<int, float, float>> handCeilings;  ///< (a slack-armed hand, a loaded knee may not rise off the floor: node, height, weight share — a one-sided row, see the plane source)
    std::vector<std::tuple<std::size_t, int, float>> kneeCeilings;  ///< (a loaded knee may not rise either: pin, node, height — added to the ceilings below by its hold)
    std::vector<glm::vec3>          risePositions;  ///< (the pose as it stands: what a live contact has risen by)
    // --- Set by ikCursorRows ---
    bool                            trunkRise{};  ///< RISING by the TRUNK: the hips are drawn under a rising trunk joint this tick.
    const glm::vec3*                goal{};  ///< The cursor row's target: the drag's, else the release hold's (may be null).
    std::size_t                     goalTask{};  ///< The cursor row's index in problem.positions (size_t(-1): no cursor row).
    // --- Set by ikBalanceRow ---
    /// The support polygon the balance row is measured against (model xz). A member because
    /// JointSolver::Problem::supportHull POINTS at it — it must outlive its stage.
    std::vector<glm::vec2>          support;
    // --- Set by ikSolve ---
    JointSolver::Pose               pose;  ///< The pose the solver works on: every channel and translation as the tick found them, then as solved.
    JointSolver::Result             result;  ///< The solver's result (converged, iterations, cost).
    int                             iterations{};  ///< Iterations over the whole continuation walk (the trace).
    // --- Set by ikApply ---
    float                           changed{};  ///< The largest channel (deg) or translation (mm) change the apply made.
    bool                            moved{};  ///< The apply changed the pose past 1e-5: what solveIk returns.
};

} // namespace pose

#endif // ARMATURE_IK_SOLVE_STATE_H

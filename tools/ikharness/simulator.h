/**
 * @file simulator.h
 * @brief The drag-loop simulator: the viewport's IK tick, without the viewport. A Scenario
 *        scripts a cursor path over a grabbed joint (plus optional user pins, an FK pre-pose, a
 *        hover, a figure scale and cursor noise); runScenario drives the REAL Armature loop
 *        (beginIkDrag -> dragIkTo per tick through the shared IkCursorFilter -> the animated
 *        release settle -> endIkDrag) and measures everything the real-skeleton phases gate.
 *
 * Metric semantics worth knowing before gating on them: contactMinY / contactBindMinY are mins
 * over DIFFERENT joints (the lowest contact pin reached vs the lowest contact pin's rest height),
 * so their difference only detects sinking below the LOWEST pin's bind height — exact for two
 * same-height feet, weaker for knee + foot pins; penetrationMax is the per-joint check against
 * the rig's own floor clearance. `suspended` reads "every pin released while contacts existed",
 * which cannot detect a suspension that a user pin survives. runIdleMetrics is a second,
 * identical pass that keeps per-joint step histories for the idle-joint tremble metrics, so the
 * main loop stays a plain mirror of the viewport's tick.
 */

#ifndef IKHARNESS_SIMULATOR_H
#define IKHARNESS_SIMULATOR_H

#include "armature.h"
#include "bodymesh.h"

#include <glm/glm.hpp>

#include <string>
#include <utility>
#include <vector>

namespace ikharness {

/// The figure's body mesh sample (the dump's `.mesh` sidecar), handed to every armature the
/// simulator builds so its rig fits the same self-collision volumes as the app; empty = the
/// skeleton-sized volumes.
extern std::vector<pose::BodyMeshPoint> g_bodyMesh;
/// Hands g_bodyMesh (scaled by @p scale, a child-size scenario) to @p arm; a no-op without one.
void applyBodyMesh(pose::Armature& arm, float scale);

struct Waypoint {
    int         tick;   // reached at this tick (tick 0 = the grab)
    glm::vec3   offset; // cursor offset from the grab point (world, metres)
    const char* label;  // the segment ending here
};

/// A drag run to completion (press, move, hold, release, settle) BEFORE the measured one: the
/// measured drag then starts from the pose a real session would have — e.g. a crouch the user
/// made and let go of, which the next drag must be able to stand back up from. Nothing of it
/// is measured.
struct PreludeDrag {
    std::string grab;
    glm::vec3   offset{0.0f};   // cursor offset from the grab point (world, metres)
    int         moveTicks = 90;
    int         holdTicks = 40;
};

struct Scenario {
    std::string           grab;                 // the joint under the cursor
    /// >= 0: the grab is of the bone's BODY, this share of the way along the rigid limb segment
    /// it belongs to (Armature::setIkGrabOnSegment) — the cursor path starts at that point and
    /// every grab metric reads it.
    float                 grabShare = -1.0f;
    /// Non-zero: the grab is of a point on the bone's own body, this far (m, world axes) from its
    /// joint in the pose the drag begins in (Armature::setIkGrabPoint) — a fingertip's pad, a
    /// toe's tip: where a click on a LEAF bone lands, which has no segment to take a share of.
    glm::vec3             grabOffset{0.0f};
    /// THE GROUP (2026-10-27): joints taken hold of WITH the grab — Shift+click's secondaries
    /// (Armature::toggleGroupBone). The measured drag pulls them all by the path's displacement,
    /// the grab the primary; the preludes stay single drags.
    std::vector<std::string> alsoGrab;
    /// The measured drag is the app's CTRL+DRAG (Armature's IkScope::Part): a grab of the body
    /// itself moves the whole figure as she is posed; anything else is SCOPED — the grabbed
    /// chain alone moves. The preludes stay whole-body drags.
    bool                  scoped = false;
    std::vector<PreludeDrag> before;            // drags made and released before the measured one
    /// The measured drag takes the grabbed joint back to where it sat BEFORE the preludes (the
    /// rest pose, pre-pose included): every non-zero waypoint offset is replaced by that one.
    /// "Stand her back up" — wherever a kneel or a sit left the joint, on any rig.
    bool                  toRest = false;
    std::vector<Waypoint> path;                 // starts with {0, 0, "start"}
    std::vector<std::string> userPins;          // pinned before the drag
    /// The pins go on AFTER the preludes (Scenario::before) — "sit her down, THEN pin the hips":
    /// set before them, a pin on the pelvis holds the prelude's own hip drag where it stands.
    bool                  pinsAfterBefore = false;
    /// Reset Pose after the preludes (the pre-pose re-applied), so the measured drag begins from
    /// the rest pose a session's Reset Pose leaves — the determinism phase: a drag made twice from
    /// the same pose must end in the same pose, to the last bit.
    bool                  resetAfterBefore = false;
    std::vector<std::pair<std::string, glm::vec3>> prePose; // FK pre-pose (bone, euler)
    float                 hover = 0.0f;         // root pose translation Y before the drag (a hovering figure)
    float                 scale = 1.0f;         // skeleton scale (0.54 = a child)
    float                 cursorNoise = 0.0f;   // +-metres of per-tick cursor noise
    bool                  release = true;       // run the release settle
    std::vector<std::string> idleJoints;        // oscillation is measured on these
    /// The most left/right asymmetry a SYMMETRIC gesture may gain (mm; RunResult::symmetricGesture:
    /// the gate is applied by construction, to every such run that takes no step). Negative =
    /// reported only, for a phase with a known lopsided result - say which, where it is set.
    double                asymmetryGateMm = 5.0;
};

struct PhaseStats {
    std::string label;
    int         ticks = 0;
    double      lagSum = 0.0, lagMax = 0.0;
    double      effOsc = 0.0;          // grabbed joint's reversal overlap (the app's osc)
    double      maxJump = 0.0;         // worst single-tick joint displacement
    double      effSpeedUpMax = 0.0;   // worst single-tick increase of the grabbed joint's speed
    double      effSpeedDownMax = 0.0; // worst single-tick decrease
    double      cursorSpeedUpMax = 0.0, cursorSpeedDownMax = 0.0;
    int         movedTicks = 0;        // ticks the solve changed the pose
    double      lagMean() const { return ticks > 0 ? lagSum / ticks : 0.0; }
};

struct RunResult {
    bool                     ok = false;
    std::string              error;
    std::vector<PhaseStats>  phases;
    int                      dragTicks = 0;
    // Contact pins (ground contacts the rig pinned at drag start) — drift from their start.
    std::vector<int>         contactPins;
    double                   contactDriftMax = 0.0;   // after the settle
    double                   contactDriftMaxDrag = 0.0; // worst during the drag
    // Mins over DIFFERENT joints (the lowest pin reached vs the lowest pin's rest height): their
    // difference only detects sinking below the LOWEST pin's bind height — exact for two
    // same-height feet, weaker for knee + foot pins; penetrationMax is the per-joint check.
    double                   contactMinY = 1e9;       // lowest a contact pin went (world)
    double                   contactBindMinY = 1e9;   // its rest height (the floor reference)
    double                   contactRiseMax = 0.0;    // highest a contact pin rose above its rest height (a heel lift)
    // Displacements of named joints (start -> end).
    double                   hipDisp = 0.0, hipDispY = 0.0;
    double                   headDisp = 0.0, chestDisp = 0.0;
    // Effector.
    int                      grabIndex = -1;
    glm::vec3                grabStart{0.0f}, grabEnd{0.0f}, cursorEnd{0.0f};
    glm::vec3                grabEulerStart{0.0f}, grabEulerEnd{0.0f}; // the grabbed bone's pose (deg)
    double                   restingMiss = 0.0;       // |grab - cursor| at the end of the drag
    double                   headJumpMax = 0.0;       // the HEAD's worst single-tick move over the drag
    // The head's LOOK direction over the horizon (degrees, negative = down) and its sideways tilt,
    // at the measured drag's start, at mouse-up and at the end (after the settle); -999 without a head.
    double                   gazeStartDeg = -999.0, gazeDragEndDeg = -999.0, gazeEndDeg = -999.0;
    double                   headRollEndDeg = 0.0;
    // LEFT/RIGHT ASYMMETRY (m): the worst distance between a right-side joint and its left twin
    // reflected across the figure's sagittal plane (x = 0; a centre bone: its distance from the
    // plane), at the measured drag's start and at the end. A centre joint dragged IN that plane
    // from a symmetric pose is a symmetric problem: what the pose GAINS is a degree of freedom
    // standing in for one the solve found dear (a corkscrewed spine, legs folded to one side) —
    // and from the side, where such a drag is looked at, it cannot be seen.
    double                   asymmetryStart = 0.0, asymmetryEnd = 0.0;
    /// The run WAS such a symmetric problem: every grab (preludes included) a centre bone, no
    /// waypoint with a sideways part, pins and pre-pose symmetric, no cursor noise.
    /// (Whether she then STEPPED is the caller's to read: a step is lopsided by nature.) Only ever
    /// true on a rig that is itself symmetric at rest: see where it is set.
    bool                     symmetricGesture = false;
    double                   asymmetryGateMm = 5.0; // (Scenario::asymmetryGateMm, carried for the report)
    double                   holdDrift = 0.0;         // effector motion through the settle
    // Settle.
    int                      settleTicks = 0;
    double                   settleMaxStep = 0.0;
    double                   settleWorstPinErr = 0.0; // contact pins after the settle
    // Pins over the drag: the most at once, the count at mouse-up, and the LIVE contacts
    // (joints the rig planted mid-drag when they reached the floor — see
    // IkRig::updateContacts): the most at once and the worst horizontal slip of a live-pinned
    // joint from where it planted (a hand on the floor must not skate).
    int                      pinsMax = 0;
    int                      pinsAtRelease = 0;
    int                      livePinsMax = 0;
    int                      liveLiftOffs = 0;   ///< Live contacts lost between one tick and the next (lift-offs), summed over the run.
    double                   livePinSlideMax = 0.0;
    /// The bones whose Euler channels moved farthest from the drag-start pose by the end (the
    /// six largest |channel delta| in degrees, name + delta): a distortion finder.
    std::vector<std::pair<std::string, double>> topEulerDeltas;
    /// Every bone's Euler channels (degrees) at the end, and the bones' names in the same order.
    std::vector<glm::vec3>   endEuler;
    std::vector<glm::vec3>   startEuler; // at the measured drag's press (after the preludes)
    std::vector<std::string> boneNames;
    // The lower hand's world height: its minimum over the drag + settle (a hand planted on
    // the floor sits ~9cm up with its fingers hanging to the floor; lower = fingers through
    // it) and its value at the end.
    double                   handsMinY = 1e9;
    double                   handsEndY = 1e9;
    // The lower knee's world height at the end (a kneel puts it on the floor).
    double                   kneesEndY = 1e9;
    // Stepping / suspension.
    int                      stepsTaken = 0;
    bool                     suspended = false;       // every pin released while contacts existed
                                                      // (misses a lift a user pin survives)
    double                   feetMinY = 1e9;          // both feet at the end (world)
    double                   feetBindMinY = 1e9;      // ... and the lower ankle's STANDING (bind) height
    // User pins.
    double                   userPinStepMax = 0.0;    // worst per-tick displacement of a pinned joint
    double                   userPinDistMax = 0.0;    // worst distance from its pin position
    double                   userPinRotMaxDeg = 0.0;  // worst world-rotation deviation
    // Everything, for ad-hoc checks: at drag start, at mouse-up (before the settle), after it.
    std::vector<glm::vec3>   startPos, dragEndPos, endPos;
    /// Per bone at the end: a HAND's palm normal (world, unit; Armature::handPalmNormal), zero
    /// elsewhere - where a hand laid on the body faces (the hand-lay phases).
    std::vector<glm::vec3>   endPalm;
    /// The body's CENTRE OF MASS (the rig's masses over the pose) at the measured drag's start
    /// and at the end: where the weight stands over the feet (the weight shift onto a standing foot).
    glm::vec3                comStart{0.0f}, comEnd{0.0f};
    std::vector<glm::vec3>   restPos; // before the preludes (Scenario::before): the standing figure
    double                   hipDropAtRelease = 0.0;   // hip descent at mouse-up (a crouch's depth)
    // FLOOR PENETRATION: the deepest any body joint went below (floor + its rig clearance) —
    // the solver's own floor rule, so 0 means no joint ever crossed it. Drag and settle.
    double                   penetrationMax = 0.0;
    int                      penetrationNode = -1;
    // BODY-VOLUME PENETRATION (self-collision, IkRig::bodyVolumes): the deepest any tested
    // joint sat inside a volume it must stay out of (radius + its clearance), during the drag
    // and the settle, and at the very end.
    double                   volumePenetrationMax = 0.0;
    double                   volumePenetrationEnd = 0.0;
    int                      volumePenetrationNode = -1; // the joint deepest inside at the worst tick
    int                      volumePenetrationTick = -1; // ... and that tick (negative = a settle tick, -1-k)
    /// The same for the hands' RIDERS (fingers, thumbs) — kept apart: the solver places no
    /// rider, so a hand turning at a chest sweeps its fingers through for a few ticks.
    double                   riderPenetrationMax = 0.0;
    double                   riderPenetrationEnd = 0.0;
    int                      riderPenetrationNode = -1;
    double                   minJointY = 1e9;         // lowest ANY joint went vs its bind height (drag; --custom info)
    // LEG diagnostics (the "knees twist inward / feet mangled" complaints), per leg over both:
    // kneeTwist* read the thigh twist bone's one free channel — the fold-plane twist the
    // extraction's witness sets — as |twist - drag start|; kneeInward* is the knee's offset from
    // its hip-socket->ankle line TOWARD the midline (valgus, m; negative = bowed outward);
    // kneeOscMax / kneeStepMax are the knees' reversal-overlap tremble and worst single-tick
    // step during the drag; footRot* is the worst world-rotation deviation of a contact-pinned
    // foot from its drag-start orientation (drag + settle) — the flat-sole hold's fidelity.
    double                   kneeTwistMaxDeg = 0.0;
    double                   kneeTwistEndDeg = 0.0;
    double                   kneeInwardMax = -1e9;
    double                   kneeInwardEnd = 0.0;
    /// The worse knee's OUTBOARD offset from its own hip socket at the end (m; the lateral
    /// direction is the socket-to-socket line): how far the knees splay in a kneel or crouch.
    double                   kneeSplayEnd = 0.0;
    /// The worse knee's position OUTBOARD of its own foot at the end (m, along the socket-to-
    /// socket line; negative = inside the foot): the quantity the knee seed caps (the kneel
    /// round) — a kneel's knees over the feet, not 15cm outside them.
    double                   kneeOverFootEnd = 0.0;
    double                   kneeOscMax = 0.0;
    double                   kneeStepMax = 0.0;
    double                   footRotMaxDeg = 0.0;
    double                   footRotEndDeg = 0.0;
};

// A second, lighter simulator pass for the IDLE-joint metrics (per-tick steps of joints that
// should not move during a gesture): tremble = accumulated reversal overlap, and the worst
// single-tick step. Kept separate so the main loop stays a faithful mirror of the viewport.
struct IdleMetrics {
    double oscMax = 0.0;   // worst accumulated reversal overlap over the idle joints
    double stepMax = 0.0;  // worst single-tick step of an idle joint
};

/// The bone list with every local bind translation scaled by @p scale (a 0.54 child).
std::vector<pose::ArmatureBone> scaledBones(const std::vector<pose::ArmatureBone>& bones,
                                            float scale);

/// The index of the bone the phases call @p canonical — a reference-generation name (`lHand`,
/// `chest_2`, `lThighTwist`) — on THIS rig, or -1. Every figure generation names the same
/// joints differently (`l_hand` / `spine4` / `l_thightwist1` on the newest one, `chestUpper`
/// on the previous), and the phases are rig-agnostic gestures: the alias table the harness and
/// the in-app scripted tests share (scene/ik/bonealiases.h) maps each canonical name to its
/// equivalents, tried in order after the name itself.
int resolveBone(const pose::Armature& arm, const std::string& canonical);
/// The deepest penetration (m) of any tested joint into any of the rig's body volumes in the
/// armature's CURRENT pose (0 without a rig). The FK collision phase's measure.
/// @p riders: false = the solved joints (and their segments), true = the hands' riders only.
double volumePenetration(const pose::Armature& arm, int* worstNode = nullptr, int* worstVolume = nullptr,
                         bool riders = false);
/// The rig's own name for @p canonical (see resolveBone), or the canonical name when absent.
std::string resolveBoneName(const pose::Armature& arm, const std::string& canonical);

/// The angle (degrees) between two rotation matrices.
double rotationAngleDeg(const glm::mat3& a, const glm::mat3& b);

/// Runs @p sc on a fresh Armature built from @p baseBones and returns every measurement; with
/// @p verbose the per-phase motion profile is printed as it goes.
RunResult runScenario(const std::vector<pose::ArmatureBone>& baseBones, const Scenario& sc,
                      bool verbose);

/// The idle-joint tremble pass (see the header note): the same drag, measuring only the joints
/// named in sc.idleJoints.
IdleMetrics runIdleMetrics(const std::vector<pose::ArmatureBone>& baseBones, const Scenario& sc);

/// start -> @p offset over @p moveTicks, then held for @p holdTicks (labels "move" and "hold").
std::vector<Waypoint> pullPath(const glm::vec3& offset, int moveTicks, int holdTicks);

} // namespace ikharness

#endif // IKHARNESS_SIMULATOR_H

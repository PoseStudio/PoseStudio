// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file armatureiksolvepins.cpp
 * @brief The solve's PIN ROWS (see armatureiksolve.cpp for the map): what holds the body to the world
 *        — user pins, the standing foot's contact model (ball, heel, sole, toes), a kneeling foot
 *        lying flat, live contacts and their fading hold, a landed hand's palm and fingertip, the
 *        ceilings, and a dragged knee's letting-go foot with its hang.
 *
 * A closed family: ikPinRowsBegin resets the tick's shared state, ikPinRows builds one pin's rows in
 * parts over an IkPinRowState (ikPinRowFlatFoot, ikPinRowLiveContact, ikPinRowFootFree,
 * ikPinRowFootModel, ikPinRowHold), ikPinRowsEnd adds the rows that need every pin known. Every
 * stage is a member function over the tick's IkSolveScratch (armatureiksolvestate.h) and begins by
 * naming the members it reads and writes. Qt-free (std + GLM).
 */

#include "armature.h"
#include "armatureiksolvegeometry.h"
#include "armatureiksolvestate.h"
#include "armatureiksolvetuning.h"

#include "ikrig.h"
#include "jointsolver.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <tuple>

namespace pose {

namespace {
// A/B levers and probes read at more than one site of this TU.
const double kKneeHeadingW = envOr("IK_JS_KNEEHEADING", kKneeFootHeadingWeight);
const double kKneeHangW = envOr("IK_JS_KNEEHANG", kKneeAnkleHangWeight);
const bool   kKneeHangFlat = std::getenv("IK_JS_KNEE_HANG_FLAT") != nullptr;                               // the hang in the floor's plane only
const bool   kUnrollBallDown = std::getenv("IK_JS_UNROLL_BALL_DOWN") != nullptr;                           // the unrolling ball held down (rejected)
const float  kUnrollBallLift = static_cast<float>(envOr("IK_JS_UNROLL_BALL_LIFT", static_cast<double>(kKneelUnrollBallLift)));
const bool   kUnrollBallLifted = kUnrollBallLift > 0.0f;
const bool   kKneelFlatTrace = std::getenv("IK_JS_KNEEL_FLAT_TRACE") != nullptr;             // each lying foot's share, references and offsets per tick
} // namespace

// THE FOOT SWINGS UNDER A LIFTED KNEE. Its hang was the ankle's offset from the knee AS THE
// DRAG FOUND IT — under it for a standing leg, and BEHIND it for a kneeling one, whose shin
// lies on the floor: a knee drawn up out of a kneel took its foot up behind it, trailing at
// knee height, where a half kneel wants the shin vertical and the foot planted under the
// knee. IK_JS_KNEE_HANG_AS_FOUND restores the found offset.
// The hang SWINGS ALONG THE FLOOR, at the shin's length (2026-09-22): the target is where a
// full-length shin from the knee's goal meets the ankle's floor height — behind the knee by
// sqrt(shin^2 - height^2), in the direction the drag found the ankle, and straight under it
// once the knee is a shin's length up. The swing was a LERP between the found offset and the
// vertical one (kKneeUnderLift), two 33cm vectors 100 degrees apart whose midpoint is 21cm
// long: mid-swing the ankle's spring (1e4-1e5 through the release) asked the ankle CLOSER
// to the knee than the shin is long, and the one answer is to FOLD the knee. Every rig
// folded (the base rig's knee flexion rose 103 -> 122 degrees and recovered as its foot
// slid out flat); a half-size character's ran to its 155-degree limit with the ankle and
// toes jammed at theirs, the leg twisted 33 degrees to get the folded foot's toes level, and
// the foot flipped up over the knee (the thigh at its 85-degree abduction limit at the end,
// a gimbal-lock trap: the foot 55cm up in the half kneel). A standing leg's shin is vertical
// and its knee a shin's length up: bit-for-bit what it was. (The lerp is gone.)
/// Where a dragged knee's foot HANGS from the knee's goal (the ankle's offset from it): on the floor's geometry, under the knee once the knee is a shin's length up.
glm::vec3 Armature::ikKneeHangOffset(const IkSolveScratch& s) const {
    const IkRig& rig = s.rig;
    const int effector = s.effector;
    const int& kneeAnkle = s.kneeAnkle;
    const glm::vec3& kneeGoal = s.kneeGoal;

    const glm::vec3 found = m_jsStartPos[static_cast<std::size_t>(kneeAnkle)] - m_jsStartPos[static_cast<std::size_t>(effector)];
    if (kHangAsFound) {
        return found;
    }
    const glm::vec3& lateral = s.lateral;
    const float     lat = glm::dot(found, lateral);
    const float     len = glm::length(found);
    const glm::vec3 vertical = lateral * lat - glm::vec3(0.0f, std::sqrt(std::max(0.0f, len * len - lat * lat)), 0.0f);
    glm::vec3 along = found - lateral * lat;
    along.y = 0.0f;
    const float alongLen = glm::length(along);
    const float floorAnkle = -m_transform[3][1] + rig.floorClearance(kneeAnkle);
    const float height = kneeGoal.y - floorAnkle; // the knee's goal over where the ankle rests on the floor
    const float reach2 = len * len - lat * lat - height * height;
    if (alongLen < 1.0e-4f || reach2 <= 0.0f) {
        return vertical; // (a vertical shin, or a knee a shin's length up: the ankle hangs under it)
    }
// (... and SWINGS UNDER the knee over the last of the reach — kKneeUnderReachFrom..To of
// the shin's length: a knee at hip height in a half kneel is a centimetre short of a
// shin's length up on every rig, and on the floor's geometry alone the foot rested
// 9-14cm behind it, the toes curled against the ankle's limit. Asked under, the ankle's
// floor contact lifts the knee its last centimetre — the foot lands under the knee, the
// shin vertical, and the sole flat.)
// (The floor's reach point at the goal's OWN height — the raised one — and nothing nearer:
// mixed toward "under" by the same share that ramps the goal's raise, the target lay
// INSIDE the shin's sphere while the raise was partial, unreachable with the knee held
// at its goal, and the foot waited 15cm behind it on the floor and caught up 8cm a tick
// when the last of the raise landed: 88-113mm at a toe on a short-legged character. The
// reach point comes under the knee by itself as the goal reaches a shin's length up.)
    const float reach = std::sqrt(reach2);
    return lateral * lat + along * (reach / alongLen) - glm::vec3(0.0f, std::max(height, 0.0f), 0.0f);
}

// ... AND IS EASED at the pins' pace (2026-09-23; IK_JS_KNEE_UNDER_NO_EASE): where the knee's
// goal reaches a shin's length up the reach point sqrt(shin^2 - height^2) collapses fastest,
// and the goal's raise takes the knee its last centimetres in two ticks — the ankle's target
// jumped 5cm in one tick on a short-legged character whose foot had slid along the floor
// the whole draw, and the foot followed: 112mm at a toe. Approached at kPinEaseStep, as a
// pin target the rig moves is (a memory of the drag, like the ball's).
/// The hanging foot's TARGET: the knee's goal plus ikKneeHangOffset, eased at the pins' pace (m_jsKneeUnderEased).
glm::vec3 Armature::ikKneeHangUnder(IkSolveScratch& s) {
    const float& legScale = s.legScale;
    glm::vec3& kneeGoal = s.kneeGoal;

    const glm::vec3 under = kneeGoal + ikKneeHangOffset(s);
    if (kNoUnderEase) {
        return under;
    }
    if (!m_jsKneeUnderValid) {
        m_jsKneeUnderEased = under;
        m_jsKneeUnderValid = 1;
        return under;
    }
    const glm::vec3 gap = under - m_jsKneeUnderEased;
    const float     len = glm::length(gap);
    m_jsKneeUnderEased = len > kPinEaseStep * legScale ? m_jsKneeUnderEased + gap * (kPinEaseStep * legScale / len) : under;
    // ... AND NEVER LEADS THE ANKLE BY MORE THAN TWO EASE STEPS (2026-10-28; IK_JS_KNEE_UNDER_NO_LEAD_CAP;
    // the palm lead cap's lesson): eased from where it WAS, the target walked on while a foot held
    // BACK by the floor's rows on its toes — a lying foot still pointing down under a drawn-up knee —
    // fell 16cm behind it, and the tick the foot levelled enough to clear, the hang row (300-400/m2)
    // took it the whole way in one solve: 22 degrees of shin, 141-197mm at a toe on the two heavy
    // bodies of the sweep, 55 on the base. (Taking the reach point at the foot's clearing height
    // instead, tried first, left the foot hanging behind the knee on three rigs under the harness's
    // larger draw: a foot that never levelled never came under.) Capped, the target waits for a foot
    // that cannot follow and leads it on at the pins' pace once it can.
    static const bool kNoLeadCap = std::getenv("IK_JS_KNEE_UNDER_NO_LEAD_CAP") != nullptr; // A/B probe
    if (!kNoLeadCap && s.kneeAnkle >= 0 && static_cast<std::size_t>(s.kneeAnkle) < m_poseGlobal.size()) {
        const glm::vec3 ankleNow(m_poseGlobal[static_cast<std::size_t>(s.kneeAnkle)][3]);
        const glm::vec3 lead = m_jsKneeUnderEased - ankleNow;
        const float     leadLen = glm::length(lead);
        const float     cap = 2.0f * kPinEaseStep * legScale;
        if (leadLen > cap) {
            m_jsKneeUnderEased = ankleNow + lead * (cap / leadLen);
        }
    }
    return m_jsKneeUnderEased;
}

// A hand contact's TAUT share (kHandTautReachBand): 0 while its arm is slack by both measures — the
// limb's own stretch (IkRig::liveContactStretch, 1 = a straight arm) and the junction's reach over the
// chain's span — 1 once the arm is dead straight by its stretch, or past kHandSlackShare by the reach.
// A continuous function of the pose: the hold a hand keeps under a rise fades as the arm straightens,
// and comes back if the hand's rise folds it. (IK_JS_HAND_UNLOAD_SWITCH: the switch it replaced.)
static float handTautShare(float stretch, float reachSpan) {
    if (kHandUnloadSwitch) {
        return stretch < kHandSlackShare && reachSpan < kHandSlackShare ? 0.0f : 1.0f;
    }
    const float byStretch = glm::smoothstep(kHandSlackShare, 1.0f, stretch);
    const float byReach = glm::smoothstep(kHandSlackShare - kHandTautReachBand, kHandSlackShare, reachSpan);
    return std::max(byStretch, byReach);
}

/// The PIN ROWS, part one: the per-tick state the pins' rows share, reset (the eased pin targets, the fingertip holds, the ceilings, the rise positions).
void Armature::ikPinRowsBegin(IkSolveScratch& s) {
    const std::size_t n = s.n;
    const std::vector<IkEffector>& pins = s.pins;

    // --- The tasks ------------------------------------------------------------------------
    // Pins: the rig's targets, approached at kPinEaseStep when the rig moves one.
    if (m_jsPinTarget.size() != n) {
        m_jsPinTarget.assign(n, glm::vec3(0.0f));
        m_jsPinTargetValid.assign(n, 0);
    }
    s.pinned.assign(n, 0);
    // (A bone that is no live contact this tick forgets the rise it formed under — see
    // m_jsContactRiseBase; the base is taken afresh when it next lands.)
    if (m_jsContactRiseBase.size() != n) {
        m_jsContactRiseBase.assign(n, -1.0f);
    }
    {
        std::vector<char> livePin(n, 0);
        for (std::size_t p = 0; p < pins.size(); ++p) {
            if (pins[p].node >= 0 && static_cast<std::size_t>(pins[p].node) < n && s.rig.pinIsLive(p)) {
                livePin[static_cast<std::size_t>(pins[p].node)] = 1;
            }
        }
        if (m_jsContactRebased.size() != n) {
            m_jsContactRebased.assign(n, 0);
        }
        if (m_jsHandUnload.size() != n) {
            m_jsHandUnload.assign(n, 0.0f);
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (!livePin[i]) {
                m_jsContactRiseBase[i] = -1.0f;
                m_jsContactRebased[i] = 0;
                m_jsHandUnload[i] = 0.0f;
            }
        }
    }
    s.pinsEasing = false;
    s.kneePlaceRow = static_cast<std::size_t>(-1); // (the dragged knee's foot has no place row yet)
    s.kneeHangMerged = false;
    s.lyingKneeFoot = false;
    s.lyingFootPitchExcessDeg = 0.0f;
    s.flatEasing = false;
    if (m_jsKneeHeldDown.size() == n) {
        m_jsKneeHeldDownPrev = m_jsKneeHeldDown;
        std::fill(m_jsKneeHeldDown.begin(), m_jsKneeHeldDown.end(), 0);
    }
    s.handTipHeld.assign(n, 0); // (hands whose fingertip hold stands this tick: see the live contacts)
    s.footFreed.assign(pins.size(), 0); // (see THE FEET COME UP: their floor rows return)
    s.footLying.assign(pins.size(), 0); // (A KNEELING FOOT LIES FLAT: the ankle's and mid-foot's floor rows off)
    s.risePositions.resize(n); // (the pose as it stands: what a live contact has risen by)
    std::vector<glm::vec3>& risePositions = s.risePositions;
    if (m_jsContactUnloaded.size() != n) {
        m_jsContactUnloaded.assign(n, 0);
    }
    std::fill(m_jsContactUnloaded.begin(), m_jsContactUnloaded.end(), 0);
    if (m_jsContactRiseUnloaded.size() != n) {
        m_jsContactRiseUnloaded.assign(n, 0);
    }
    std::fill(m_jsContactRiseUnloaded.begin(), m_jsContactRiseUnloaded.end(), 0);
    for (std::size_t i = 0; i < n; ++i) {
        risePositions[i] = glm::vec3(m_poseGlobal[i][3]);
    }
}


/// The HEADING the drag found foot @p node at — how far its LATERAL axis (the foot's own, across
/// the ankle-to-ball line at bind, which a pitch about it does not move) has turned about the
/// vertical — into @p yaw; false where that axis stands too near the vertical to read. (Read off
/// the WORLD's lateral axis until 2026-09-30: a foot stands toed out at bind, the world's x is not
/// its pitch axis, and pitched half a turn — lying on its top behind a kneel — that axis read a
/// heading up to twice the toe-out off; with the frames right, a foot rolled back onto its toes
/// was re-seated 35 degrees toed out, its heel 7cm inside its ball, and the leg flipped basins to
/// get there — 14cm at a thigh in one tick. IK_JS_FOOT_HEADING_WORLD_X is the A/B.)
bool Armature::footYawFound(std::size_t node, int ballBone, float& yaw) const {
    static const bool kWorldX = std::getenv("IK_JS_FOOT_HEADING_WORLD_X") != nullptr; // A/B probe
    glm::vec3 fore = m_ikBindPos[static_cast<std::size_t>(ballBone)] - m_ikBindPos[node];
    fore.y = 0.0f;
    const glm::vec3 lateralBind = (!kWorldX && glm::length(fore) > 1.0e-4f)
                                      ? glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), fore))
                                      : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 lateralFound = node < m_jsStartRot.size() ? m_jsStartRot[node] * lateralBind : lateralBind;
    lateralFound.y = 0.0f;
    if (glm::length(lateralFound) <= 0.3f) {
        return false;
    }
    lateralFound = glm::normalize(lateralFound);
    yaw = std::atan2(glm::cross(lateralBind, lateralFound).y, glm::dot(lateralBind, lateralFound));
    return true;
}

/// The STANDING ball offset from ankle @p node: the bind's, turned to the foot's heading as the
/// drag found it (footYawFound).
glm::vec3 Armature::standingBallOffset(std::size_t node, int ballBone) const {
    glm::vec3 standing = m_ikBindPos[static_cast<std::size_t>(ballBone)] - m_ikBindPos[node];
    float     yaw = 0.0f;
    if (footYawFound(node, ballBone, yaw)) {
        standing = glm::mat3_cast(glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f))) * standing;
    }
    return standing;
}

/// The PIN ROWS of ONE pin, in parts over an IkPinRowState: the pin's eased target and its place row, the
/// flat-foot model (ikPinRowFlatFoot), a landing knee's soft place, the live contact's fading hold and the
/// landed hand's palm (ikPinRowLiveContact), the prone release and the place row's push (ikPinRowFootFree),
/// then a standing foot's rows (ikPinRowFootModel) or a user pin's / a seat's hold (ikPinRowHold).
void Armature::ikPinRows(IkSolveScratch& s, std::size_t p) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const std::vector<IkEffector>& pins = s.pins;
    int& kneeFootPin = s.kneeFootPin;
    JointSolver::Problem& problem = s.problem;
    std::vector<char>& pinned = s.pinned;
    bool& pinsEasing = s.pinsEasing;
    std::array<std::size_t, 4>& kneeFootRows = s.kneeFootRows;

    if (static_cast<int>(p) == kneeFootPin) {
        kneeFootRows[0] = problem.positions.size();
        kneeFootRows[2] = problem.orientations.size();
    } else if (kneeFootPin >= 0 && static_cast<int>(p) == kneeFootPin + 1) {
        kneeFootRows[1] = problem.positions.size();
        kneeFootRows[3] = problem.orientations.size();
    }
    const IkEffector& pin = pins[p];
    if (pin.node < 0 || static_cast<std::size_t>(pin.node) >= n) {
        return;
    }
    const std::size_t node = static_cast<std::size_t>(pin.node);
    pinned[node] = 1;
    if (!m_jsPinTargetValid[node]) {
        m_jsPinTarget[node] = glm::vec3(m_poseGlobal[node][3]); // from where the joint is
        m_jsPinTargetValid[node] = 1;
    }
    const glm::vec3 gap = pin.target - m_jsPinTarget[node];
    const float len = glm::length(gap);
    if (len > kPinEaseStep) {
        m_jsPinTarget[node] += gap * (kPinEaseStep / len);
        pinsEasing = true;
    } else {
        m_jsPinTarget[node] = pin.target;
    }
    const double weight = kPinWeight * (rig.pinIsUser(p) ? kUserPinScale : 1.0);
    const int ball = m_jsBall[node];
    static const double kHeel = envOr("IK_JS_HEEL", kHeelHoldWeight);
    IkPinRowState      st;
    JointPositionTask& task = st.task;
    task.bone = pin.node;
    task.target = glm::dvec3(m_jsPinTarget[node]);
    task.weight = ball >= 0 ? kHeel : weight; // a foot with a ball: the heel is the SOFT row
    // The foot below a dragged knee TURNS ON ITS HEEL: the ankle keeps its place (hard, in
    // the floor's plane — its height stays the soft heel's), and the ball bears in height
    // only, free to swing about it. (Pivoting on the ball instead — every other planted
    // foot's hard point — the solve found the cheap way to take a knee outward: carry the
    // ANKLE out under it, 9cm, the toes turning IN.)
    // (... and the foot below one of THE GROUP's secondary knees turns on its heel the same way —
    // its knee never lifts it, so it keeps the planted model in full: both knees pushed forward
    // together swing over both feet, where the standing model lifted the second heel 12mm and the
    // pose came out lopsided. The primary's foot alone has the knee drag's lift and hang rows.)
    const bool primaryKneeFoot = static_cast<int>(p) == kneeFootPin && ball >= 0;
    const bool kneeFoot = ball >= 0 && (primaryKneeFoot || (rig.pinUnderAnyEffector(p) && !rig.pinUnderEffector(p)));
    if (kneeFoot) {
        task.axisScale = glm::dvec3(0.0, 1.0, 0.0);
    }
    st.p = p;
    st.pin = &pin;
    st.node = node;
    st.weight = weight;
    st.ball = ball;
    st.primaryKneeFoot = primaryKneeFoot;
    st.kneeFoot = kneeFoot;
    st.heelTask = problem.positions.size(); // (the place row's index once pushed: its target is finished by the foot rows)
    ikPinRowFlatFoot(s, st);
    // A knee COMING DOWN over its planted foot (the rig's HARD live pin: a kneel) is held hard in
    // its HEIGHT — that is what brings it onto the floor — and only SOFTLY to its place
    // (kKneeLandingHold: enough that it does not wander, a knee left free drifted 12cm inward
    // on 25 degrees of thigh twist) until it has landed, and is planted where it lands
    // (dragIkTick re-seats it then). It was nailed, in full, to the spot it
    // HOVERED over when it crossed the 5cm band: brought 4.5cm straight down there, a tucked
    // foot already at its ankle's limits cannot follow, the knee's pin and the ball's — two
    // 1e8 rows — pulled the leg apart with a force of 47,000 against the cursor's 1,000, and
    // the hips were thrown 20cm down and back in one tick to relieve it, where they stayed.
    if (!kKneeLandingHeld && rig.pinIsLive(p) && pin.hard && !rig.pinUnderEffector(p)) {
        const float above = m_poseGlobal[node][3].y - pin.target.y;
        const float landed = 1.0f - glm::smoothstep(0.003f * rig.sizeScale(), 0.012f * rig.sizeScale(), above);
        static const double kLandingHold = envOr("IK_JS_KNEELANDHOLD", kKneeLandingHold);
        // (The miss's scale: the square root of the weight's. A friction-like hold once landed —
        // a place hold that gives at the pelvis drag's full pull, so hips pushed lower than rigid
        // thighs allow would slide the knees — was tried and did nothing: the tilt was never
        // about reach or sliding, and a landed knee is nailed.)
        const double place = std::sqrt(glm::mix(kLandingHold, weight, static_cast<double>(landed)) / weight);
        st.task.axisScale = glm::dvec3(place, 1.0, place);
        if (landed < 1.0f && node < m_jsContactLanding.size()) {
            m_jsContactLanding[node] = 1;
        }
    }
    ikPinRowLiveContact(s, st);
    ikPinRowFootFree(s, st);
    if (st.ball >= 0) {
        ikPinRowFootModel(s, st); // the standing foot's rows: the ball, the heel, the sole, the toes
        return;
    }
    ikPinRowHold(s, st); // a user pin's or a seat's orientation hold
}

/// A KNEELING FOOT LIES FLAT: this pin's share of the lying model, its references, the roll and the unroll.
void Armature::ikPinRowFlatFoot(IkSolveScratch& s, IkPinRowState& st) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const std::vector<IkEffector>& pins = s.pins;
    const float& legScale = s.legScale;
    float& kneeLift = s.kneeLift;
    std::vector<char>& footFreed = s.footFreed;
    const std::size_t p = st.p;
    const IkEffector& pin = *st.pin;
    const std::size_t node = st.node;
    const int ball = st.ball;
    const bool kneeFoot = st.kneeFoot;
    float& flat = st.flat;
    float& flip = st.flip;
    bool& flatFoot = st.flatFoot;
    bool& unrolling = st.unrolling;
    bool& toLying = st.toLying;
    float& rollProgress = st.rollProgress;
    bool& lyingKneeFoot = st.lyingKneeFoot;

    // A KNEELING FOOT LIES FLAT (2026-09-26; kKneelFlat* in the tuning header, IK_JS_NO_KNEEL_FLAT).
    // The foot behind a knee that is ON the floor is no standing foot: it lies on its top, toes
    // pointing back, as every kneeling person's does — where this model stood it on its tucked
    // toes (the ball pinned hard at the floor, the toes held level, the ankle at its dorsiflexion
    // limit), the first thing the user saw wrong in every kneel. Its share `flat` eases in once
    // the knee above it has LANDED and while that knee's hold stands (the knee's contact fade is
    // the share's ceiling: a body getting up unloads its knees first, and the foot's model turns
    // back to the standing one as it does). Here: the share and the knee; the rows read it below —
    // the ball's row and the toes' hold fall by decades over it (the ball's target tracking the
    // ball, so a returning row starts from where the ball lies), the floor's rows come onto the
    // foot (footFreed), the lying ball and heel offsets are re-seated on the pose, and the ankle's
    // pitch channel gets its plantar REFERENCE (ikPostureModel reads m_jsFootFlatRef: a price can
    // only prefer less travel, so the pose is given and the floor stops it).
    static const bool kNoKneelFlat = std::getenv("IK_JS_NO_KNEEL_FLAT") != nullptr; // A/B probe: the tucked-toe kneel
    // (The STANDING ball offset from the ankle and the HEADING the drag found the foot at are
    // standingBallOffset and footYawFound, above.)
    // The ankle's PITCH channel: the one whose axis lies along the foot's lateral axis (the
    // bind's, across the ankle-to-ball line); its plantar direction is the one that takes the
    // ball DOWN. (Every bind is translation-only: a channel's orientation axis is a world axis
    // at rest.) Found once per foot, for the lying model and for the knee-drag's foot alike.
    if (ball >= 0 && m_jsFootFlat.size() == n && m_jsFootFlatAxis[node] == -2) {
        m_jsFootFlatAxis[node] = -1;
        glm::vec3 fore = m_ikBindPos[static_cast<std::size_t>(ball)] - m_ikBindPos[node];
        fore.y = 0.0f;
        if (glm::length(fore) > 1.0e-4f) {
            const glm::vec3 lateral = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), fore));
            float bestAlong = 0.7f;
            for (int a = 0; a < 3; ++a) {
                const glm::vec3 axis = glm::normalize(glm::vec3(m_bones[node].orient[a]));
                const float along = std::abs(glm::dot(axis, lateral));
                if (along > bestAlong && m_bones[node].rotLimited[a]) {
                    bestAlong = along;
                    m_jsFootFlatAxis[node] = a;
                    const glm::vec3 lever = m_ikBindPos[static_cast<std::size_t>(ball)] - m_ikBindPos[node];
                    const bool plantarPositive = glm::cross(axis, lever).y < 0.0f;
                    m_jsFootFlatLimit[node] = plantarPositive ? m_bones[node].rotMax[a] : m_bones[node].rotMin[a];
                    m_jsFootFlatTuck[node] = plantarPositive ? m_bones[node].rotMin[a] : m_bones[node].rotMax[a];
                }
            }
        }
    }
    if (!kNoKneelFlat && ball >= 0 && !kneeFoot && !rig.pinIsUser(p) && !rig.pinIsLive(p) && m_jsFootFlat.size() == n) {
        const float scale = rig.sizeScale();
        int kneePin = -1;
        for (std::size_t q = 0; q < pins.size() && kneePin < 0; ++q) {
            // (A KNEE: a leg joint with a foot below it. Any live pin among the foot's ancestors
            // was taken for one, and a floor-sitter's SEAT — the pelvis bone, a live contact on
            // the floor — flattened both her feet: 252mm jumps and 77mm through the floor in the
            // floor-seat phases.)
            // (... and not a SHADOW: a lifted contact the body still leans on is no knee on the floor.)
            if (q == p || !rig.pinIsLive(q) || rig.pinIsShadow(q) || pins[q].node < 0 || !rig.isLegJointAboveFoot(pins[q].node)) {
                continue;
            }
            for (int cur = m_bones[node].parent; cur >= 0 && cur != root; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                if (cur == pins[q].node) {
                    kneePin = static_cast<int>(q);
                    break;
                }
            }
        }
        float want = 0.0f;
        float kneeAboveTrace = 0.0f, kneePinYTrace = 0.0f; // (for the trace)
        if (kneePin >= 0) {
            const std::size_t kneeNode = static_cast<std::size_t>(pins[static_cast<std::size_t>(kneePin)].node);
            m_jsFootKnee[node] = static_cast<int>(kneeNode);
            const float above = m_poseGlobal[kneeNode][3].y - pins[static_cast<std::size_t>(kneePin)].target.y;
            kneeAboveTrace = above;
            kneePinYTrace = pins[static_cast<std::size_t>(kneePin)].target.y;
            const float landed = 1.0f - glm::smoothstep(kKneelFlatLandFrom * scale, kKneelFlatLandTo * scale, above);
            // The foot LIES while its knee is ON the floor (landed: the knee joint within 2cm of its
            // planted height) and the body is not getting up; it rolls back onto its toes with the
            // body's RISE (m_jsRootRise / m_jsContactRise, over its first 30%) or once the knee has
            // really left the floor. Not with the knee's HOLD fading, as until 2026-09-26: the hold
            // fades for a dip of the target too, and a kneel sat back onto its heels — a body going
            // DOWN — unrolled both feet in mid-air on the third generation, 102mm at a toe, where the
            // user's photo has them lying flat under her.
            // (... and only when the knee is being UNLOADED by that rise: a knee held down by its
            // ceiling — one the hips were LOW over at the press, kneeBelowHips: a body pushed up off
            // its belly — keeps its foot lying, and from prone onto all fours the feet stay flat, as a
            // person's do. Read off the rise alone the push-up rolled both feet onto their toes under
            // held knees, and the returning rows brought them down in a tick: 107-117mm at a toe.)
            const bool kneeHeld = kneeNode < m_jsKneeHeldDownPrev.size() && m_jsKneeHeldDownPrev[kneeNode];
            const float bodyRise = kneeHeld ? 0.0f : std::max(m_jsRootRise, m_jsContactRise);
            want = landed * (1.0f - glm::smoothstep(0.0f, 0.3f, bodyRise));
            // (The lying ankle's height: the knee's planted height — the shin lies level along the
            // floor on its front, and the ankle joint sits as high over it as the knee joint does.
            // Held at its STANDING height, 7.5cm, the foot floated level 5cm off the floor: the
            // rig's 65 degrees of plantar flexion are spent bringing a foot that points 20 degrees
            // down at rest — the ankle stands above the ball — up to the shin's line.)
            m_jsFlatLyingY[node] = pins[static_cast<std::size_t>(kneePin)].target.y;
        }
        const int axis = m_jsFootFlatAxis[node];
        if (!m_jsFootFlatSeen[node]) {
            // The first tick of a drag: is this foot FLAT? No planted knee — no (whatever a kneel
            // let go of and Reset left in the share); a planted knee and the ankle within
            // kKneelFlatNearLimitDeg of its plantar limit — yes, from the first tick.
            m_jsFootFlatSeen[node] = 1;
            if (want < 0.5f) {
                m_jsFootFlat[node] = 0.0f;
            } else if (axis >= 0 && std::abs(m_boneEuler[node][axis] - m_jsFootFlatLimit[node]) < kKneelFlatNearLimitDeg) {
                m_jsFootFlat[node] = want;
            }
            m_jsFootWasFlat[node] = m_jsFootFlatAtStart[node] = m_jsFootFlat[node] > 0.5f ? 1 : 0;
        }
        if (axis < 0) {
            want = 0.0f;
        }
        // THE FOOT'S STATE. Toward LYING while its knee holds (want); let go of, it UNROLLS first —
        // its references run back to standing at the turn's pace, in the air, every row of the
        // foot still off, the ankle riding its shin — and only once it stands again does its
        // share fade and the standing model return (below: the rows come back by decades as the
        // ball's target eases from where the ball is to its planted spot — a landing). Turned back
        // by the knee's LIFT while the rows hardened, a chest rise that lifted the knee 13cm in a
        // tick had the ball's row haul the ball 16cm forward along the floor and the leg shot
        // straight: the knee 51cm up in two ticks, 578mm at a toe.
        // (... to the TUCKED foot — the ankle's other limit, the ball coming down under the ankle
        // on the floor — not to standing: unrolled to standing the foot hung in the air on a shin
        // the rise had lifted, and landing it onto a spot 20cm ahead snapped the knee straight, 267mm
        // in a tick. Tucked, it is the tucked kneel's own foot, and that kneel's rise — the ball
        // planted, the heel turning flat with the progress — is the path every rig gets up by.)
        toLying = want > 0.5f;
        if (!toLying && m_jsFootWasFlat[node] && m_jsFootFlatRefValid[node] && axis >= 0) {
            glm::vec3 left = m_jsFootFlatRef[node];
            left[axis] -= m_jsFootFlatTuck[node];
            unrolling = std::abs(left.x) + std::abs(left.y) + std::abs(left.z) > 1.0f;
        }
        const float flatGoal = toLying ? want : (unrolling ? m_jsFootFlat[node] : 0.0f);
        // (How far the foot has rolled back onto its toes — 0 while it lies, 1 once handed over — for
        // the ceiling on the knee above it, below in the live contacts: the knee lifts as the toes tuck.)
        if (m_jsFootUnrollProgress.size() == n) {
            if (toLying) {
                m_jsFootUnrollProgress[node] = 0.0f;
            } else if (unrolling && axis >= 0 && m_jsFootFlatRefValid[node]) {
                const float whole = m_jsFootFlatTuck[node] - m_jsFootFlatLimit[node];
                m_jsFootUnrollProgress[node] = std::abs(whole) > 0.5f
                                                   ? glm::clamp((m_jsFootFlatRef[node][axis] - m_jsFootFlatLimit[node]) / whole, 0.0f, 1.0f)
                                                   : 1.0f;
            } else {
                m_jsFootUnrollProgress[node] = 1.0f;
            }
        }
        {
            const float d = glm::clamp(flatGoal - m_jsFootFlat[node], -kKneelFlatEaseStep, kKneelFlatEaseStep);
            m_jsFootFlat[node] += d;
            if (std::abs(d) > 1.0e-4f) {
                s.flatEasing = true; // (the settle waits for it: settleIkSolveTick)
            }
        }
        flat = m_jsFootFlat[node];
        if (flat > 0.5f) {
            m_jsFootWasFlat[node] = 1;
        }
        flatFoot = m_jsFootWasFlat[node] != 0;
        // The references: from where the channels stand when the share first rises, the pitch's
        // toward the plantar limit and the two others' toward 0 — in line with the shin — at
        // kKneelFlatTurnStepDeg a tick. (Asked there at once, a foot on tucked toes has 100
        // degrees to turn, and would turn them in a tick; and the other two channels, which a
        // tucked foot under a kneel's turned-in shin holds 17 and 25 degrees off to keep the
        // standing heading the sole's hold asks for, sprang back 78mm at the toes the tick that
        // hold went — every channel of a lying foot eases to its lying value.)
        if (flat > 0.0f && axis >= 0) {
            if (!m_jsFootFlatRefValid[node]) {
                m_jsFootFlatRefValid[node] = 1;
                m_jsFootFlatRef[node] = m_boneEuler[node];
                m_jsFlatRollStartY[node] = m_poseGlobal[node][3].y;
                m_jsFlatRollStartRef[node] = m_boneEuler[node][axis];
                if (flat > 0.5f) {
                    m_jsFootFlatRef[node][axis] = m_jsFootFlatLimit[node];
                }
            }
            const float turnStep = toLying ? kKneelFlatTurnStepDeg : kKneelFlatUnrollStepDeg; // (the unroll is quicker: the body is on its way up, its knee lifting with the roll)
            for (int a = 0; a < 3; ++a) {
                const float goal = a == axis ? (toLying ? m_jsFootFlatLimit[node] : m_jsFootFlatTuck[node]) : 0.0f; // (tucked, once its knee lets go)
                const float gap = goal - m_jsFootFlatRef[node][a];
                const float d = glm::clamp(gap, -turnStep, turnStep);
                m_jsFootFlatRef[node][a] += d;
                if (std::abs(d) > 1.0e-3f) {
                    s.flatEasing = true;
                }
            }
            // (The roll's progress: the pitch reference's way from where it began to the limit —
            // 1 at once for a foot found lying.)
            const float whole = m_jsFootFlatLimit[node] - m_jsFlatRollStartRef[node];
            rollProgress = std::abs(whole) < 0.5f ? 1.0f : glm::clamp((m_jsFootFlatRef[node][axis] - m_jsFlatRollStartRef[node]) / whole, 0.0f, 1.0f);
            // The TOES and the mid-foot — EVERY joint below the ankle: the ball's chain, the toe
            // segments, the toe mates — ease STRAIGHT at the same pace: a tucked foot's toes are
            // folded back under it, and rolled over with them folded — and the floor's rows on their
            // joints — the roll locked and let go at once. (The ball's chain alone until 2026-09-26:
            // the toe segments rode at their weak posture and the floor bent them, a big toe 32
            // degrees through a kneel's hold on the generation whose toes fan from the mid-foot.)
            std::vector<int>& footBelow = s.subtreeBuf; // (the scratch's buffer)
            footBelow.clear();
            collectSubtree(pin.node, footBelow);
            for (const int cur : footBelow) {
                if (cur == pin.node) {
                    continue;
                }
                const std::size_t c = static_cast<std::size_t>(cur);
                if (!m_jsToeFlatValid[c]) {
                    m_jsToeFlatValid[c] = 1;
                    m_jsToeFlatRef[c] = m_boneEuler[c];
                }
                for (int a = 0; a < 3; ++a) {
                    const float d = glm::clamp(-m_jsToeFlatRef[c][a], -kKneelFlatTurnStepDeg, kKneelFlatTurnStepDeg);
                    m_jsToeFlatRef[c][a] += d;
                    if (std::abs(d) > 1.0e-3f) {
                        s.flatEasing = true;
                    }
                }
                // (While the foot rolls ONTO its top, or lies: through the UNROLL back onto tucked toes
                // the toes must BEND — a tucked foot's are folded back against the floor under the
                // ankle — and held straight at the lying stiffness the foot could not tuck: the male
                // rig's getting-up by the hip jumped 133mm where it had passed, the third generation's 140.)
                m_jsToeFlatShare[c] = toLying ? flat : 0.0f;
            }
        } else {
            m_jsFootFlatRefValid[node] = 0;
            std::vector<int>& footBelow = s.subtreeBuf; // (the scratch's buffer)
            footBelow.clear();
            collectSubtree(pin.node, footBelow);
            for (const int cur : footBelow) {
                if (cur != pin.node) {
                    m_jsToeFlatValid[static_cast<std::size_t>(cur)] = 0;
                    m_jsToeFlatShare[static_cast<std::size_t>(cur)] = 0.0f;
                }
            }
        }
        // The lying model, re-seated on the pose while the foot lies (the ball where it rests, the
        // ankle where it stands), and its turn back to the standing one once the foot has rolled
        // back onto its toes (flip, below).
        const glm::vec3 ballNow(m_poseGlobal[static_cast<std::size_t>(ball)][3]);
        const glm::vec3 pinNow(m_poseGlobal[node][3]);
        // The model SWITCHES back to the standing one once the foot stands again (flip: 0 while it
        // lies or unrolls, 1 after) — and its posture references are the standing ones from then
        // (a foot found lying has the plantar limit as its drag-start reference, and as the share
        // fades that would draw it back toward the floor).
        if (flatFoot && !toLying && !unrolling) {
            flip = 1.0f;
            if (!m_jsFootUnrolled[node]) {
                // (From here the foot is the tucked kneel's: its posture references and the sole's and
                // toes' held orientations are re-read off the tucked foot, as a drag that found it
                // tucked would have them, and the rise turns them flat with its progress.)
                m_jsFootUnrolled[node] = 1;
                m_jsFlatRiseBase[node] = m_jsRiseProgress;
                // The RIG's ankle pin goes where the standing ankle will stand: a heel's length
                // behind the ball, which stays where it lay. Left at the spot the ankle LAY at — a
                // foot's length ahead — the rig's support polygon (balance, the steps) stood 24cm
                // ahead of the feet, the heel row pulled the ankle 21cm forward, and every rise
                // ended on a straight leg on tiptoe with the hips 12.5cm short of the cursor.
                // (Re-seating it where the ankle STANDS at the handover, with the ball slid under
                // the ankle through the unroll, was tried on 2026-09-26 and taken out: it moved the
                // getting-up's jumps about between rigs without settling them.)
                {
                    const glm::vec3 standing = standingBallOffset(node, ball);
                    const glm::vec3 lyingBall = pin.target + m_jsFlatBallOffset[node];
                    const glm::vec3 newAnkle = lyingBall - standing;
                    rig.reseatPin(p, newAnkle.x, newAnkle.z);
                    m_jsFlatBallOffset[node] = lyingBall - pin.target; // (relative to the re-seated pin: the ball where it lay)
                }
                // The ball's row is back at its FULL weight from this tick: its target starts where the
                // ball IS and eases to the floor from there. Through the unroll the target's height —
                // no row's business then, the spring holds the ball's place only — was set to the
                // ball's and then walked a pin's step (2.5cm) back toward the LYING height by the
                // easing below, every tick: the handover found it 3cm under the ball, and 1e8 hauled
                // the ball there in one tick. A foot that had unrolled in the air took it as a twitch;
                // one standing on its toes on the floor had the next solve fail under it — the knees
                // back on the floor and the hips 25cm from the cursor for a tick, then back: 17-27cm.
                // (IK_JS_HANDOVER_BALL_TARGET_STALE is the A/B.)
                static const bool kStaleBallTarget = std::getenv("IK_JS_HANDOVER_BALL_TARGET_STALE") != nullptr; // A/B probe
                if (!kStaleBallTarget) {
                    m_jsBallTarget[node] = ballNow;
                    m_jsBallTargetValid[node] = 1;
                }
                for (std::size_t k = 0; k < m_ikFlatNodes.size(); ++k) {
                    if (m_ikFlatOwner[k] == pin.node && m_ikFlatNodes[k] >= 0 && static_cast<std::size_t>(m_ikFlatNodes[k]) < n) {
                        m_ikFlatRot[k] = glm::mat3(m_poseGlobal[static_cast<std::size_t>(m_ikFlatNodes[k])]);
                        if (static_cast<std::size_t>(m_ikFlatNodes[k]) < m_ikStartEuler.size()) {
                            m_ikStartEuler[static_cast<std::size_t>(m_ikFlatNodes[k])] = m_boneEuler[static_cast<std::size_t>(m_ikFlatNodes[k])];
                        }
                    }
                }
            }
        }
        // (The ball's target TRACKS the ball while its row is soft — whatever the model: a target
        // that ran ahead while the row hardened yanked the ball 16cm in the tick the row was back.)
        if (flat > kKneelFlatTrackFrom) {
            if (toLying) {
                m_jsFlatBallOffset[node] = ballNow - pin.target; // (the lying ball, where it rests)
                m_jsFlatHeelOffset[node] = pinNow - ballNow;
                m_jsBallTarget[node] = ballNow;
                m_jsBallTargetValid[node] = 1;
            } else if (unrolling) {
                // (Rolling back onto its toes IN PLACE: the ball's spot is the lying one, frozen; the
                // heel's relation to it is read to the switch — the tucked foot's, for the rise. Sliding
                // the spot under the ankle through the roll was tried, 2026-09-26, and taken out with
                // the rest of that variant.)
                // (... and the ball is held DOWN too, softly, its height target easing to the ball's
                // floor height at the landing pace and never above where the ball is: free in its
                // height, the foot flew 15cm into the air on a leg the rise folded up while the foot's
                // rows were off, and the returning rows brought it down in a tick — 175mm at a
                // fingertip on the third generation; that variant is gone.)
                // (... held DOWN too, behind IK_JS_UNROLL_BALL_DOWN: it measured worse than the plane-only
                // spring on all three rigs it was tried on — the knee's ceiling, below, is what keeps the
                // foot from flying.)
                const float ballFloorY = pin.target.y + standingBallOffset(node, ball).y;
                // THE FOOT COMES OFF THE FLOOR TO TURN OVER (2026-09-30; kKneelUnrollBallLift,
                // IK_JS_UNROLL_BALL_LIFT, 0 = the plane-only spring): the ball's height target rises
                // with the roll's progress, softly, and never below where the ball is. A lying foot's
                // toes point back, on their tops; a tucked foot's point forward, on their pads — the
                // toes turn OVER between the two, and on the floor they can only do it on their tips,
                // the ball a toe's length up: where the handover then found the foot, and whether the
                // returning ball row folded the toes forward (the tuck) or back (the foot lying down
                // again, the knee after it, the hips 25cm from their cursor for a tick) was the lean of
                // a toe a few degrees from vertical — 12-27cm pops on three of the eight rigs. Lifted,
                // the toes swing under in the air and the foot LANDS on them, as a person's does.
                // (It had been doing just that by accident: until the bones' frames were put right the
                // foot pitched about an axis 7 degrees off its own, its toes dug at the floor's rows,
                // and the leg folded the foot 25cm into the air.)
                const float liftY = ballFloorY + kUnrollBallLift * rig.sizeScale() * (node < m_jsFootUnrollProgress.size() ? m_jsFootUnrollProgress[node] : 0.0f);
                const float ballY = kUnrollBallLifted ? std::max(liftY, ballNow.y)
                                    : kUnrollBallDown ? std::max(ballFloorY, ballNow.y - kKneelFlatLandStep * rig.sizeScale()) : ballNow.y;
                m_jsFlatHeelOffset[node] = pinNow - ballNow;
                m_jsBallTarget[node] = glm::vec3(pin.target.x + m_jsFlatBallOffset[node].x, ballY, pin.target.z + m_jsFlatBallOffset[node].z);
                m_jsBallTargetValid[node] = 1;
            }
        }
        if (flat > 0.0f || flatFoot) {
            footFreed[p] = 1; // (the floor's rows keep the lying foot up: its ball row is gone)
        }
        if (flat > 0.0f) {
            s.footLying[p] = 1; // (... all but the ankle's and the mid-foot's, which read standing heights)
        }
        if (kKneelFlatTrace) {
            std::fprintf(stderr, "[kneel-flat] %s: want %.2f flat %.2f flip %.2f axis %d ref %.1f limit %.1f euler %.1f knee %d (pinY %.3f above %.3f) ball(%.3f %.3f %.3f) ankle(%.3f %.3f %.3f) pinY %.3f",
                         m_boneNames[node].c_str(), want, flat, flip, axis, axis >= 0 ? m_jsFootFlatRef[node][axis] : 0.0f, m_jsFootFlatLimit[node],
                         axis >= 0 ? m_boneEuler[node][axis] : 0.0f, m_jsFootKnee[node], kneePinYTrace, kneeAboveTrace, ballNow.x, ballNow.y, ballNow.z, pinNow.x, pinNow.y, pinNow.z, pin.target.y);
            {
                const int bp = m_bones[static_cast<std::size_t>(ball)].parent;
                if (bp >= 0) {
                    std::fprintf(stderr, " %s(%.3f %.3f %.3f) e(%.1f %.1f %.1f)", m_boneNames[static_cast<std::size_t>(bp)].c_str(), m_poseGlobal[static_cast<std::size_t>(bp)][3].x,
                                 m_poseGlobal[static_cast<std::size_t>(bp)][3].y, m_poseGlobal[static_cast<std::size_t>(bp)][3].z, m_boneEuler[static_cast<std::size_t>(bp)].x,
                                 m_boneEuler[static_cast<std::size_t>(bp)].y, m_boneEuler[static_cast<std::size_t>(bp)].z);
                }
            }
            if (m_jsFootKnee[node] >= 0) {
                const std::size_t k = static_cast<std::size_t>(m_jsFootKnee[node]);
                const int kp = m_bones[k].parent;
                std::fprintf(stderr, " knee(%.3f %.3f %.3f) shin(%.1f %.1f %.1f) parent %s(%.1f %.1f %.1f) foot(%.1f %.1f %.1f)", m_poseGlobal[k][3].x, m_poseGlobal[k][3].y, m_poseGlobal[k][3].z,
                             m_boneEuler[k].x, m_boneEuler[k].y, m_boneEuler[k].z, kp >= 0 ? m_boneNames[static_cast<std::size_t>(kp)].c_str() : "-",
                             kp >= 0 ? m_boneEuler[static_cast<std::size_t>(kp)].x : 0.0f, kp >= 0 ? m_boneEuler[static_cast<std::size_t>(kp)].y : 0.0f, kp >= 0 ? m_boneEuler[static_cast<std::size_t>(kp)].z : 0.0f,
                             m_boneEuler[node].x, m_boneEuler[node].y, m_boneEuler[node].z);
            }
            std::fprintf(stderr, " rise %.2f progress %.2f base %.2f unroll %.2f pin(%.3f %.3f %.3f)", m_jsRootRise, m_jsRiseProgress,
                         node < m_jsFlatRiseBase.size() ? m_jsFlatRiseBase[node] : 0.0f,
                         node < m_jsFootUnrollProgress.size() ? m_jsFootUnrollProgress[node] : 0.0f, pin.target.x, pin.target.y, pin.target.z);
            std::fputc(10, stderr);
        }
    } else if (m_jsFootFlat.size() == n && !rig.pinIsLive(p) && !rig.pinIsUser(p) && ball >= 0) {
        if (kneeFoot && !kNoKneelFlat) {
            // A LYING FOOT UNDER A DRAWN-UP KNEE RIDES ITS SHIN (2026-09-26). The knee-drag's foot
            // has no lying model — it is planted, swung over, let go of and landed by the knee
            // drag's own rows — and found LYING (its share from the kneel that made it, and the
            // ankle at its plantar limit) it was given the STANDING foot's: the sole's and toes'
            // holds at the lying orientation IN THE WORLD, which a shin lifting off the floor
            // cannot keep — the ankle dorsiflexed to its limit, the foot stood up on its toe pads
            // with the ankle 16cm up, and when the hang brought the ankle down under the knee the
            // toes flipped 147mm in one tick (the half kneel out of a flat kneel). Such a foot
            // now RIDES: its ankle keeps its lying height (footLying: the ankle's and mid-foot's
            // floor rows read standing heights), its ball row is off, and the sole's and toes'
            // holds come in with the trail's flatten toward LEVEL — the foot hangs from its shin
            // as the shin rises and turns flat for the landing, as a person's does.
            if (!m_jsFootFlatSeen[node]) {
                m_jsFootFlatSeen[node] = 1;
                const int axis = m_jsFootFlatAxis[node];
                const bool lying = axis >= 0 && m_jsFootFlat[node] > 0.5f &&
                                   std::abs(m_boneEuler[node][axis] - m_jsFootFlatLimit[node]) < kKneelFlatNearLimitDeg;
                m_jsFootFlatAtStart[node] = lying ? 1 : 0;
                m_jsFootFlat[node] = lying ? 1.0f : 0.0f;
                m_jsFlatLyingY[node] = m_poseGlobal[node][3].y; // (the lying ankle's height, as found)
                m_jsFlatRollStartY[node] = m_jsFlatLyingY[node];
            }
            lyingKneeFoot = m_jsFootFlatAtStart[node] != 0;
            s.lyingKneeFoot = s.lyingKneeFoot || lyingKneeFoot;
            flat = lyingKneeFoot ? 1.0f : 0.0f;
            float refLevel = 0.0f, pitchNowDeg = 0.0f; // (for the trace)
            if (lyingKneeFoot) {
                footFreed[p] = 1;     // (the floor's rows on the toes and the ball keep the lying foot up)
                s.footLying[p] = 1;   // (... the ankle's and the mid-foot's, which read standing heights, off)
                // ... and its TOES are held STRAIGHT at the lying stiffness (m_jsToeFlatRef/Share,
                // ikPostureModel) while it rides: at their own weak posture, bending a toe 40 degrees
                // against the floor cost less than lifting the ankle 3cm against its hang, so the
                // floor's rows bent them to their limits tick by tick as the foot turned — the hang's
                // clearance, read off the bent shape, could never take hold — and they sprang back
                // 100mm at a tip when the turning foot lifted them off. Held, the floor lifts the ankle.
                {
                    std::vector<int>& footBelow = s.subtreeBuf; // (the scratch's buffer)
                    footBelow.clear();
                    collectSubtree(pin.node, footBelow);
                    for (const int cur : footBelow) {
                        if (cur == pin.node) {
                            continue;
                        }
                        const std::size_t c = static_cast<std::size_t>(cur);
                        if (c < m_jsToeFlatValid.size()) {
                            m_jsToeFlatValid[c] = 1;
                            m_jsToeFlatRef[c] = glm::vec3(0.0f);
                            m_jsToeFlatShare[c] = 1.0f;
                        }
                    }
                }
                // ... AND TURNS TOWARD LEVEL IN THE AIR: the ankle's pitch REFERENCE (ikPostureModel reads
                // m_jsFootFlatRef at kKneelFlatStiffness over the share) eases from the plantar limit
                // the lying foot has toward the value that LEVELS the sole — read off the shin's
                // orientation as it stands, each tick — at kKneelFlatTurnStepDeg a tick, once the knee
                // has lifted the foot (over kKneeLiftFull .. kKneeTrailFull of lift). A lying foot's
                // level is 180 degrees away in pitch: no orientation row can say which way round, and
                // a foot left hanging plantar until its ankle came down landed on its toe tips pointing
                // down and back — a local minimum (any forward turn dipped the toes into the floor's
                // rows first), the foot 88 degrees from level and 15cm from its hang on every rig.
                const int axis = m_jsFootFlatAxis[node];
                if (axis >= 0) {
                    if (!m_jsFootFlatRefValid[node]) {
                        m_jsFootFlatRefValid[node] = 1;
                        m_jsFootFlatRef[node] = m_boneEuler[node]; // (the two other channels stay as found)
                    }
                    const glm::mat3 R(m_poseGlobal[node]);
                    glm::vec3 bindFore = m_ikBindPos[static_cast<std::size_t>(ball)] - m_ikBindPos[node];
                    glm::vec3 bindForeH = bindFore;
                    bindForeH.y = 0.0f;
                    if (glm::length(bindFore) > 1.0e-4f && glm::length(bindForeH) > 1.0e-4f) {
                        const glm::vec3 lateralLocal0 = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), bindForeH));
                        glm::vec3 lateralNow = R * lateralLocal0;
                        lateralNow.y = 0.0f;
                        if (glm::length(lateralNow) > 0.2f) {
                            lateralNow = glm::normalize(lateralNow);
                            const glm::vec3 headingDir = glm::normalize(glm::cross(lateralNow, glm::vec3(0.0f, 1.0f, 0.0f)));
                            const glm::vec3 fore = glm::normalize(R * bindFore);
                            const float pitchNow = std::atan2(-fore.y, glm::dot(fore, headingDir));      // toes down positive; back and down > 90
                            const float pitchBind = std::atan2(-bindFore.y, glm::length(bindForeH));     // the level sole's droop (~20 degrees)
                            const bool plantarPositive = m_jsFootFlatLimit[node] > m_jsFootFlatTuck[node];
                            const float dPlantar = glm::degrees(pitchBind - pitchNow);
                            pitchNowDeg = glm::degrees(pitchNow);
                            s.lyingFootPitchExcessDeg = std::max(s.lyingFootPitchExcessDeg, glm::degrees(pitchNow - pitchBind));
                            refLevel = glm::clamp(m_boneEuler[node][axis] + (plantarPositive ? dPlantar : -dPlantar),
                                                  m_bones[node].rotMin[axis], m_bones[node].rotMax[axis]);
                            const float turn = glm::smoothstep(kKneeLiftFull * legScale, kKneeTrailFull * legScale, kneeLift);
                            const float step = kKneelFlatTurnStepDeg * 2.0f * turn;
                            const float d = glm::clamp(refLevel - m_jsFootFlatRef[node][axis], -step, step);
                            m_jsFootFlatRef[node][axis] += d;
                            if (std::abs(d) > 1.0e-3f) {
                                s.flatEasing = true;
                            }
                        }
                    }
                }
            }
            if (kKneelFlatTrace) {
                std::fprintf(stderr, "[kneel-flat] %s: the dragged knee's foot, lying %d, pitch now %.1f, ref %.1f -> level %.1f, lift %.3f\n", m_boneNames[node].c_str(),
                             lyingKneeFoot ? 1 : 0, pitchNowDeg, m_jsFootFlatAxis[node] >= 0 ? m_jsFootFlatRef[node][m_jsFootFlatAxis[node]] : 0.0f, refLevel, kneeLift);
            }
            if (!lyingKneeFoot) {
                m_jsFootFlatRefValid[node] = 0;
            }
        } else {
            // (A flat kneel A/B'd off: no lying model this drag.)
            m_jsFootFlat[node] = 0.0f;
            m_jsFootFlatRefValid[node] = 0;
        }
    }
}

/// A LIVE CONTACT (a hand, a knee come down on the floor): its unilateral hold, the ceilings, the landed
/// hand's palm and fingertip rows, and the fade of the hold as the joint or the body rises.
void Armature::ikPinRowLiveContact(IkSolveScratch& s, IkPinRowState& st) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    JointSolver::Problem& problem = s.problem;
    std::vector<char>& handTipHeld = s.handTipHeld;
    std::vector<std::tuple<int, float, float>>& handCeilings = s.handCeilings;
    std::vector<glm::vec3>& risePositions = s.risePositions;
    // (This call's own: a landed hand's palm pull and fingertip height, made where the hand is found and
    // added once the contact's fade is known.)
    std::vector<JointOrientationTask> palmRows;
    std::vector<JointPositionTask>    tipRows;
    const glm::vec3& fore = s.fore;
    const std::size_t p = st.p;
    const IkEffector& pin = *st.pin;
    const std::size_t node = st.node;
    JointPositionTask& task = st.task;

    // The arm of a hand contact the rise is UNLOADING is referred to where the unload finds it (the
    // fade block): EVERY TICK the unload is in progress, the channels from the hand up to the limb's
    // junction — in this tick's dofs, and through m_jsPostStepTurned for the rest of the drag. Once
    // per contact it stored strain instead: over the three ticks the hold faded the shoulder pulled
    // the arm dead straight and twisted 23 degrees against references fixed at the first of them,
    // and the tick the pin went the arm sprang back by that much — 131mm at a fingertip. An arm
    // whose hand is letting go has no pose of its own to spring to: it is where the body left it,
    // and the hang takes it from there. (IK_JS_HAND_UNLOAD_SWITCH keeps the drag-start references.)
    const auto rebaseContactArm = [&](std::size_t hand) {
        if (kHandUnloadSwitch || hand >= m_jsContactRebased.size()) {
            return;
        }
        m_jsContactRebased[hand] = 1;
        const int junction = rig.limbJunction(static_cast<int>(hand));
        for (int cur = static_cast<int>(hand); cur >= 0 && cur != junction && cur != root;
             cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            const std::size_t b = static_cast<std::size_t>(cur);
            if (b < m_jsPostStepEuler.size() && b < m_jsPostStepTurned.size()) {
                m_jsPostStepEuler[b] = m_boneEuler[b];
                m_jsPostStepTurned[b] = 1;
            }
            for (JointSolverDof& dof : problem.dofs) {
                if (dof.bone == cur) {
                    dof.reference = m_boneEuler[b][dof.axis];
                }
            }
        }
        // ... AND THE TRUNK THE HAND HAS HOLD OF, under a pelvis drag (2026-10-28): the hand's path
        // to the root runs through the spine, which is among the unknowns while the hand is pinned,
        // and the nailed hand curled it against its drag-start references as the hips rose — 5-10
        // degrees over three joints priced 87/rad2, a spring weaker than a 100/m2 row at a 2cm miss
        // on the arm's lever, so no fade of the hold can release it — and the pin's going let it go
        // in one tick: 144-341mm at a fingertip, 101 at the brow, standing up off all fours by the
        // hips. Re-based as the arm is (the spine chain and the root's rotation), nothing springs,
        // and the trunk comes home at kTrunkHomeStep from where the hands left it: ikPostureModel
        // and ikSpineCoupling measure the home from the re-based value (m_jsPostStepEuler).
        // IK_JS_HAND_UNLOAD_NO_TRUNK_REBASE is the A/B.
        // (... and only while the body is GETTING UP (s.rising: a drag that began low), which gives a
        // re-based trunk its way home — the trunk-home share. A drag that began STANDING and dipped
        // has no rise mode: re-based there the trunk kept whatever the nailed hands had made of it,
        // and the pre-posed all-fours rise ended with the trunk still curled, the hands 18cm up where
        // they hang at 40; un-rebased it springs back to the drag-start fold as it always did.)
        static const bool kNoTrunkRebase = std::getenv("IK_JS_HAND_UNLOAD_NO_TRUNK_REBASE") != nullptr; // A/B probe
        // (... and while the trunk LIES DOWN under a prone descent — THE FACE LIFT's share, which gives
        // the trunk its way home as the rise does, 2026-10-29: the hands had held the chest BACK as a
        // leash, and when their trailing release let go the strain the spine and the root had stored
        // against it sprang in one tick — the root pitched 5 degrees, a brow 77mm, the solve out of
        // iterations.)
        // (THE HOME IS RE-BASED, NOT THIS TICK'S REFERENCE, 2026-10-29: the trunk's reference is
        // mix(home, upright-or-lying, the paced share), and the home — m_jsPostStepEuler, read by the
        // posture model at the NEXT tick — is where the hands leave the trunk; overwriting the
        // reference with the current value as well, as this did for a day, left the trunk with no
        // reference at all for as long as a hand's hold was fading, which under the prone is the
        // whole drag: the trailing hands' unload never ends while they stand, so the root's lying
        // reference never reached the solver and the pelvis dived 32 degrees past flat after the
        // cursor. The rise's hands are unloaded with the share frozen at zero, so there the stale
        // home is the current value to within one tick's move, as it was.)
        // (... and NOT under the prone, 2026-10-29: the re-base's FIRST tick moves the home from the
        // drag-start value to the current one — on the base rig from 56 to 77 degrees of pelvis
        // pitch, the lying reference with it by 16 at a share of 0.29 — and the posture term took
        // 8 of them in one tick, 93mm at the brow. The trailing hands' leash is let go of through
        // its own eased unload (the fade block), and the lying references take the trunk from
        // where the leash leaves it at the posture term's pace.)
        if (!kNoTrunkRebase && effector == root && s.rising > 0.0f) {
            const auto rebase = [&](int bone) {
                if (bone < 0 || static_cast<std::size_t>(bone) >= n) {
                    return;
                }
                const std::size_t b = static_cast<std::size_t>(bone);
                if (b < m_jsPostStepEuler.size() && b < m_jsPostStepTurned.size()) {
                    m_jsPostStepEuler[b] = m_boneEuler[b];
                    m_jsPostStepTurned[b] = 1;
                }
            };
            for (std::size_t k = 1; k < m_jsSpineChain.size(); ++k) {
                rebase(m_jsSpineChain[k]);
            }
            rebase(root);
        }
    };

    if (rig.pinIsLive(p) && !pin.hard) {
        // A LIVE contact (a hand, a knee come down onto the floor) is UNILATERAL: the floor
        // holds its place and stops it sinking (the floor's row, below), and nothing holds
        // it DOWN — a rising body lifts it off, which is what the rig's lift-off rule then
        // sees. (Held in full, the hard row kept a rising figure's hands nailed to the floor.)
        task.axisScale = glm::dvec3(1.0, 0.0, 1.0);
        // (... but A KNEE ON THE FLOOR KEEPS IT UNDER A LIMB DRAG, 2026-10-28;
        // IK_JS_KNEE_RISES_UNDER_LIMB_DRAG: the rise the unilateral hold is for is the BODY's, and
        // a hand slid forward from under a prone chest lifts nothing — yet on a heavy body the
        // sphinx it made arched the trunk and carried both knees 3cm up, past the hold's fade,
        // and the hips-up that followed found no knee within the seed band: she planked on her
        // toes and the cursor with her hands in the air. Under a hand's or an elbow's drag, the
        // body neither rising nor suspended, a live knee gets a CEILING at its planted height at
        // full weight — it may settle, not rise. The dragged limb's own contacts, the hard pins
        // and every trunk and pelvis drag are as they were.)
        static const bool kKneeRisesUnderLimbDrag = std::getenv("IK_JS_KNEE_RISES_UNDER_LIMB_DRAG") != nullptr; // A/B probe
        const bool limbDrag = effector >= 0 && effector != root && !rig.effectorIsTrunk() &&
                              (s.elbowDrag || (m_jsWristOf.size() == n && static_cast<std::size_t>(effector) < n &&
                                               m_jsWristOf[static_cast<std::size_t>(effector)] == effector));
        if (!kKneeRisesUnderLimbDrag && limbDrag && m_jsRootRise <= 1.0e-4f && !rig.suspended() && !rig.pinIsShadow(p) &&
            !rig.pinUnderEffector(p) && rig.isLegJointAboveFoot(pin.node)) {
            handCeilings.emplace_back(pin.node, pin.target.y, 1.0f);
        }
        // (... except a HAND whose arm still has length to give: it stays DOWN, in full, while the
        // body rises and the arm straightens — a push-up — and is unilateral only once the arm is
        // taut, where the lift-off takes over. Free in its height from the first centimetre, a
        // hand under a trunk swinging up off the floor rose with it, its own hold faded with
        // its rise, and a figure got up off her belly into a kneeling plank with both hands in
        // the air. IK_JS_HANDS_UNILATERAL restores it.)
        static const bool kHandsUnilateral = std::getenv("IK_JS_HANDS_UNILATERAL") != nullptr; // A/B probe
        if (!kHandsUnilateral && !rig.pinIsShadow(p) && m_jsKneelUpRise < 0.5f && m_jsWristOf.size() == n && m_jsWristOf[node] == static_cast<int>(node)) { // (the wrist's own contact; a carpal's rides it)
            const float reachSpan = glm::length(glm::vec3(m_poseGlobal[static_cast<std::size_t>(rig.limbJunction(pin.node))][3]) - pin.target) / std::max(rig.liveSpan(p), 1.0e-4f);
            const float tautShare = rig.liveSpan(p) > 0.0f ? handTautShare(rig.liveContactStretch(p, risePositions), reachSpan) : 1.0f;
            if (rig.liveSpan(p) > 0.0f && tautShare < 0.999f) {
                // (A CEILING at its planted height, not a hold: the hand may still go DOWN — a
                // hand that lands on its fingertips rolls onto its palm 13cm lower — only not up.
                // Held in y it stayed on its fingertips, the palm roll blocked.)
                // (... and NOT under a body COMING DOWN, 2026-09-23; IK_JS_HAND_CEILING_ALWAYS:
                // the ceiling is for a body pushing UP off its belly, whose hands must stay
                // loaded as the arms straighten. Kept under a descent it stopped the PRONE
                // descent dead — from all fours the hips taken a thigh's length forward and
                // down need the arms to FOLD under the body, the wrists pitching up off the
                // palm-flat height on their planted fingertips, and capped at 3.5cm they could
                // not: the hips stood 45cm up, 43cm from their cursor, on every rig (15cm and
                // 11 before the ceiling came), which the phase's rows — all informational — had
                // not said since the ceiling's own round. The share goes with the target's
                // downward travel, m_jsTargetDown: a function of the target.)
                // (Released for EVERY arm: a release only for an arm that is folding — by its
                // reach over its span — was tried for a day and dropped. Both kinds of arm end
                // taut, and from the half-released band the strut-armed rigs' hands rose with
                // the rearing trunk and let go, a 41cm collapse; what keeps such a body from
                // collapsing is the rig holding its contacts under a descent — no lift-off,
                // IkRig::updateContacts.)
                // (LET GO OF THROUGH ITS SLACK, NOT ITS WEIGHT, 2026-09-28; IK_JS_HAND_CEILING_FADES
                // is the weight's fade, as it was: the ceiling RISES with the target's descent,
                // kHandCeilingSlack at the last of it, and is gone once it stands that high. A hard
                // row barely reads its weight until the rest of the cost rivals it — at a
                // sixteenth of 1e7 this one still held a wrist 12mm under where it was going, 85
                // of a solve's cost of 135 — and the last of the fade let the hand go at once: on
                // one character the freed arm turned 28 degrees at the shoulder in that tick,
                // 130mm at a fingertip. Under a rising ceiling the wrist comes up at the
                // ceiling's pace.)
                static const bool kCeilingAlways = std::getenv("IK_JS_HAND_CEILING_ALWAYS") != nullptr; // A/B probe
                static const bool kCeilingFades = std::getenv("IK_JS_HAND_CEILING_FADES") != nullptr;   // A/B probe
                if (kCeilingAlways) {
                    handCeilings.emplace_back(pin.node, pin.target.y, 1.0f);
                } else if (kCeilingFades) {
                    if (1.0f - m_jsTargetDown > 1.0e-3f) {
                        handCeilings.emplace_back(pin.node, pin.target.y, 1.0f - m_jsTargetDown);
                    }
                } else {
                    // (... and through its slack as the arm STRAIGHTENS too, 2026-10-27: the ceiling
                    // rises with the hand's taut share — handTautShare — so a hand under an arm going
                    // dead straight may come up off the floor by the slack before its hold is gone,
                    // instead of being nailed down one tick and free the next.)
                    // (... and no faster than the hold fades: the EASED unload, as the last tick left
                    // it — m_jsHandUnload, kHandUnloadStep — not the raw taut share.)
                    const float eased = node < m_jsHandUnload.size() ? std::min(tautShare, m_jsHandUnload[node]) : tautShare;
                    const float lift = std::max(m_jsTargetDown, eased);
                    if (lift < 0.999f) {
                        handCeilings.emplace_back(pin.node, pin.target.y + kHandCeilingSlack * rig.sizeScale() * lift, 1.0f);
                    }
                }
                // (Tried and taken out the same day, 2026-09-26: the held arm's posture references
                // FOLLOWING the pose while the ceiling holds, so that the arm would not spring back
                // to "folded under the chest" when the ceiling let go. It did not stop the prone
                // push-up's hand shooting up at the release — 247mm where it read 196 — and added a
                // 104mm finger pop to the prone descent's hold: what springs at that release is the
                // TRUNK the arm strut was holding down, not the arm.)
            }
        }
        // A HAND ON THE FLOOR LIES ON ITS PALM. Its place is held where the rig holds it - at the
        // joint that touched, in the floor's plane - and what was missing was only the pull onto
        // the palm: a hanging arm's hand comes down fingers first, the wrist a hand's length up,
        // and with nothing to say which way a loaded hand lies the solve bent the ELBOW instead as
        // the body came on down - a figure going onto all fours stood on her fingertips, her back
        // rounding and her head tucking under. Two rows: the FINGERTIP is held to the floor in
        // its HEIGHT (the hand's longest rider, as it landed), and a soft pull turns the hand
        // PALM DOWN, fingers along the heading they landed with - so the hand can only lie down
        // by bringing its WRIST down, as far as the shoulder lets it, the fingers sliding forward
        // under it. (Each half alone was tried: the fingertip held in PLACE too, the roll carried
        // the wrist a hand's length BACK while the body came forward - the arm went taut, held
        // the chest 3-6cm off its cursor and was kicked by the fold retry every tick; and with
        // nothing on the fingertip's height the pull was answered by swinging the whole arm UP,
        // the hands half a metre off the floor.) One pair of rows a HAND, whichever of its joints
        // touched (m_jsWristOf: on some generations the carpals are real joints, and land).
        static const bool kNoPalm = std::getenv("IK_JS_NO_PALM_FLAT") != nullptr; // A/B probe
        ensureHandMaps();
        const int         wrist = (kNoPalm || m_jsWristOf.size() != n) ? -1 : m_jsWristOf[node];
        const std::size_t hnode = wrist >= 0 ? static_cast<std::size_t>(wrist) : node;
        const int         tip = wrist >= 0 ? m_jsHandTip[hnode] : -1;
        if (tip >= 0 && !handTipHeld[hnode]) {
            handTipHeld[hnode] = 1;
            if (m_jsHandTipValid.size() != n) {
                m_jsHandTipValid.assign(n, 0);
            }
            if (m_jsPalmFrom.size() != n) {
                m_jsPalmFrom.assign(n, glm::mat3(1.0f));
                m_jsPalmEase.assign(n, 0.0f);
                m_jsHandHeading.assign(n, glm::vec3(0.0f, 0.0f, 1.0f));
            }
            const glm::vec3 tipAt(m_poseGlobal[static_cast<std::size_t>(tip)][3]);
            const glm::vec3 handAt(m_poseGlobal[hnode][3]);
            if (!m_jsHandTipValid[hnode]) {
                m_jsHandTipValid[hnode] = 1;
                if (m_jsHandTipTarget.size() != n) {
                    m_jsHandTipTarget.assign(n, glm::vec3(0.0f));
                }
                m_jsHandTipTarget[hnode] = tipAt;
                m_jsHandTipTarget[hnode].y = std::max(tipAt.y, -m_transform[3][1] + rig.floorClearance(tip));
                m_jsPalmFrom[hnode] = glm::mat3(m_poseGlobal[hnode]);
                m_jsPalmEase[hnode] = 0.0f;
                // (The fingers' heading along the floor AS THE HAND LANDED - FORWARD for a hand
                // that lands ahead of the hips, whatever its fingers did on the way down: hands
                // under a body on all fours point where it faces, and a hanging hand's point
                // straight down and have no heading. A hand behind her, propping a recline,
                // keeps the way it came down.)
                glm::vec3 along = tipAt - handAt;
                along.y = 0.0f;
                const float     reach = glm::length(tipAt - handAt);
                const glm::vec3& lateralNow = s.lateral;
                // (Not the scratch's fore axis: the normalized cross product differs from it in the last bit.)
                const glm::vec3 forward = glm::normalize(glm::cross(lateralNow, glm::vec3(0.0f, 1.0f, 0.0f)));
                const float     ahead = glm::dot(handAt - glm::vec3(m_poseGlobal[static_cast<std::size_t>(root)][3]), forward);
                const bool      foundHeading = reach > 1.0e-4f && glm::length(along) > 0.35f * reach;
                m_jsHandHeading[hnode] = (ahead > 0.10f * rig.sizeScale() || !foundHeading) ? forward : glm::normalize(along);
            }
            // Fingers' line and palm normal are the BIND's (every bind is translation-only: the
            // bone's local axes are the world's there): wrist to longest fingertip, and the part
            // of DOWN across it (palms face down in a T-pose, down and in in an A-pose).
            const glm::vec3 fingers = m_ikBindPos[static_cast<std::size_t>(tip)] - m_ikBindPos[hnode];
            const float     fingersReach = glm::length(fingers);
            if (fingersReach > 1.0e-4f) {
                glm::vec3 f(0.0f);
                glm::vec3 palm(0.0f);
                if (handBindFrame(fingers, f, palm)) {
                    const glm::vec3 h = m_jsHandHeading[hnode];
                    const glm::vec3 down(0.0f, -1.0f, 0.0f);
                    const glm::mat3 local(f, palm, glm::cross(f, palm));
                    const glm::mat3 world(h, down, glm::cross(h, down));
                    const glm::mat3 lieRot = world * glm::transpose(local);
                    // (The fingertip's height: a row of its own, finished with the contact's fade.)
                    JointPositionTask tipDown;
                    tipDown.bone = wrist;
                    tipDown.offset = glm::dvec3(glm::transpose(glm::mat3(m_poseGlobal[hnode])) * (tipAt - handAt));
                    tipDown.target = glm::dvec3(m_jsHandTipTarget[hnode]);
                    tipDown.axisScale = glm::dvec3(0.0, 1.0, 0.0);
                    tipRows.push_back(tipDown);
                    static const double kPitch = envOr("IK_JS_PALMPITCH", kPalmPitchWeight);
                    static const double kLevel = envOr("IK_JS_PALMLEVEL", kPalmLevelWeight);
                    static const double kHeading = envOr("IK_JS_PALMHEADING", kPalmHeadingWeight);
                    // (Eased in from the rotation the hand landed with, kPalmTurnStepDeg a tick -
                    // a hand that hung palm-to-thigh has a quarter turn of the forearm to make -
                    // and the ease only ever LEADS: a hand already further along is met there.)
                    glm::mat3 targetRot = lieRot;
                    if (m_jsPalmEase[hnode] < 1.0f) {
                        const LeadingEase ends = advanceLeadingEase(m_jsPalmFrom[hnode], lieRot, glm::mat3(m_poseGlobal[hnode]),
                                                                    kPalmTurnStepDeg, m_jsPalmEase[hnode]);
                        targetRot = glm::mat3_cast(glm::slerp(ends.from, ends.to, m_jsPalmEase[hnode]));
                    }
                    // (... and never more than kPalmLeadDeg ahead of the hand: an arm folded under a prone chest
                    // cannot turn its pinned hand, and the target sailing on to flat snapped the arm into another
                    // basin — the one-tick unfold as the hips rose off the belly.)
                    targetRot = capPalmLead(glm::mat3(m_poseGlobal[hnode]), targetRot);
                    JointOrientationTask flat;
                    flat.bone = wrist;
                    flat.target = glm::dmat3(targetRot);
                    flat.perAxis = true;
                    const glm::dvec3 up(0.0, 1.0, 0.0);
                    const glm::dvec3 headingAxis(h);
                    flat.frame = glm::dmat3(glm::cross(up, headingAxis), up, headingAxis);
                    flat.axisWeights = glm::dvec3(kPitch, kHeading, kLevel);
                    palmRows.push_back(flat);
                }
            }
        }
        // ... and its hold on the joint's PLACE lets go as the joint leaves the floor (read
        // off the pose the tick begins with): a contact is friction, and an unloading joint
        // has less and less of it. The weight falls by SIX DECADES over the first 2cm of
        // rise — a linear fade does nothing here: half of 1e8 is as rigid as all of it, and a
        // kneeling shin cannot rise without its knee sliding (the knee rides a circle about
        // the ankle), so held "half" it rose 1.5cm and deadlocked: for a third of a second
        // the body stood still under a leaving cursor, then popped 5cm in a tick.
        float contactGone = 0.0f;
        static const bool kNoFade = std::getenv("IK_JS_NO_CONTACT_FADE") != nullptr; // A/B probe
        if (!kNoFade) {
            const float scale = rig.sizeScale();
            // (Its rise over where its FLESH rests now, not over the height it was planted at: a
            // pelvis rolled back from a sit goes over the buttocks, the hip joint 3.5cm higher
            // on the way, resting all the while — read as a rise, its seat let go of it halfway
            // down, and the hold came and went on alternate ticks.)
            const float rise = rig.liveContactRise(p, risePositions);
            float gone = glm::smoothstep(0.002f * scale, 0.02f * scale, rise);
            // (EASED over the ticks: the rise is read off the pose the LAST solve left, so the
            // hold is a switch thrown a tick late — a hand whose hold lifts it 2cm lets go, the
            // arm relaxes, the hand is back on the floor, the hold returns: a relaxation
            // oscillator, 3-4cm at the fingertips on alternate ticks under a held sit. Eased,
            // it settles where the two agree.)
            static const bool kNoFadeEase = std::getenv("IK_JS_NO_FADE_EASE") != nullptr; // A/B probe
            if (!kNoFadeEase && node < m_jsContactGone.size()) {
                gone = glm::mix(m_jsContactGone[node], gone, 0.35f);
                m_jsContactGone[node] = gone;
            }
            // ... and as the BODY gets up (m_jsRootRise: a function of the cursor alone). Read off
            // the joint's own rise the hold is a switch with a memory: it keeps a kneeling knee
            // down until the rising hips leave it no way to stay within 2cm of the floor, and
            // then lets go of all of it at once — the knee swung 16cm in ONE tick on a character
            // rig getting up out of a kneel (2-9cm on the others: the gate had been 11). A body
            // that is getting up is UNLOADING what it kneels or leans on from the first
            // centimetre: the hold goes with the rise, and the knee leaves the floor as the
            // cursor takes the hips up.
            // (... and out of the low the DRAG ITSELF went to, whether or not it began low - the
            // room a drag that began standing has is none, so m_jsRootRise is 0 through a dip
            // and its return: see m_jsContactRise.)
            static const bool kNoRiseUnload = std::getenv("IK_JS_NO_RISE_UNLOAD") != nullptr; // A/B probe
            static const bool kNoDipUnload = std::getenv("IK_JS_NO_DIP_UNLOAD") != nullptr;   // A/B probe
            // (... but not a HAND whose arm still has length to give: a body pushed up off its
            // belly or off all fours keeps its hands on the floor while the arms straighten — a
            // push-up — and lets go of them when they are taut (the lift-off). Unloaded by the
            // rise from its first centimetre, the hands left the floor as the trunk swung up with
            // the pelvis, and a figure got up off her belly into a kneeling plank with her hands
            // in the air. IK_JS_RISE_UNLOADS_HANDS restores it.)
            static const bool kRiseUnloadsHands = std::getenv("IK_JS_RISE_UNLOADS_HANDS") != nullptr; // A/B probe
            bool  armSlack = false;
            float handTaut = 1.0f; // (a hand's: 0 slack, 1 taut — handTautShare; everything else unloads in full)
            if (!kRiseUnloadsHands && m_jsWristOf.size() == n && m_jsWristOf[node] >= 0) {
                const float stretch = rig.liveContactStretch(p, risePositions); // (1 = a straight arm)
                const float reachSpan = glm::length(glm::vec3(m_poseGlobal[static_cast<std::size_t>(rig.limbJunction(pin.node))][3]) - pin.target) / std::max(rig.liveSpan(p), 1.0e-4f);
                // (The hand HANGS FROM ITS STRAIGHT ARM: held down while the arm is slack, let go of
                // as it straightens, held again as the hand's rise folds it — so it follows the
                // shoulder at kHandSlackShare of the arm's STRETCH (IkRig::liveContactStretch), and stays on the floor under a
                // push-up whose shoulders stop at the arm's length. The share sits ABOVE the rig's
                // taut test (kLiveContactTaut 0.95): at the same 0.95 a hand following its shoulder
                // read taut and slack on alternate ticks, never three taut ticks running, and never
                // lifted off — a figure stood up off all fours by the chest with one hand still a
                // contact, the arm the solve's, and the wrist flipped under the palm's rows, 27cm at
                // a fingertip in one tick. A hand taut for the drag once taut was tried instead:
                // it left the push-up in a plank, the hands in the air the moment the arms were
                // straight.)
                // (SLACK by BOTH measures: the limb's own stretch (IkRig::liveContactStretch: bend
                // left in the arm) AND the junction's reach over the chain's span (the body still
                // within the limb's reach, the lift-off's test). Neither alone is right on every
                // rig: by the junction's reach the two oldest generations' straight arms read 0.85
                // (their collars run sideways from the chest) and a hand under a kneel-up never
                // let go; by the stretch alone the main family's hands, landed with 30 degrees of
                // elbow, were held under a hip rising to standing while the solve folded the trunk
                // rather than straighten the arms — the reach had released them at 1.05.)
                handTaut = rig.liveSpan(p) > 0.0f ? handTautShare(stretch, reachSpan) : 1.0f;
                armSlack = handTaut < 1.0e-3f;
                static const bool kHandTrace = std::getenv("IK_JS_HAND_CEILING_TRACE") != nullptr;
                if (kHandTrace) {
                    std::fprintf(stderr, "[hand-ceiling] %s: stretch %.3f reach/span %.3f slack=%d shadow=%d", m_boneNames[node].c_str(), stretch,
                                 reachSpan, armSlack ? 1 : 0, rig.pinIsShadow(p) ? 1 : 0);
                    std::fputc(10, stderr);
                }
            }
            // (... nor a KNEE the hips are still not up over: a body getting up off its belly by
            // the hips comes up onto all fours first, the thighs swinging back to upright over
            // knees that stay where they are — unloaded from the rise's first 20cm, the knees
            // let go and the shins swung 14-16cm in a tick. A knee is a support until the hips
            // stand kKneeUnloadHeight of the thigh's length over it. IK_JS_RISE_UNLOADS_KNEES.)
            static const bool kRiseUnloadsKnees = std::getenv("IK_JS_RISE_UNLOADS_KNEES") != nullptr; // A/B probe
            bool  kneeBelowHips = false;
            bool  kneeCeiling = false;    // (a loaded knee may not rise either: its ceiling is added below, by its hold)
            float kneeHoldRelease = 1.0f; // (a knee the hips were low over: how far its hold has been let go of, 0..1)
            if (!kRiseUnloadsKnees && m_jsWristOf.size() == n && m_jsWristOf[node] < 0 && !rig.isGirdleJoint(pin.node)) {
                if (rig.isLegJointAboveFoot(pin.node)) { // (a knee: a foot-class joint below it)
                    const int socket = m_bones[node].parent;
                    const float thigh = socket >= 0 ? glm::length(m_ikBindPos[node] - m_ikBindPos[static_cast<std::size_t>(socket)]) : 0.0f;
                    // (The SOCKET's height over the knee, not the root's: the hip joint sits a hand above
                    // the thigh's socket, and read from it a kneeling thigh was already 'past' its length.)
                    // (AS THE DRAG FOUND THEM — a constant of the drag, never a mode off the pose it
                    // changes: read live, a kneeling figure's hips passed the thigh's height, the knees
                    // unloaded, and the ceiling came back the tick they dipped — she could not get up
                    // out of a kneel at all. A drag that begins with the hips low over the knees — off
                    // the belly — keeps its knees for the whole drag: it comes up onto all fours; to
                    // stand from there is the next drag's.)
                    const std::size_t socketIndex = socket >= 0 ? static_cast<std::size_t>(socket) : static_cast<std::size_t>(root);
                    const float over = socketIndex < m_jsStartPos.size() && node < m_jsStartPos.size()
                                           ? m_jsStartPos[socketIndex].y - m_jsStartPos[node].y
                                           : thigh;
                    const bool belowAtPress = thigh > 0.0f && over < kKneeUnloadHeight * thigh;
                    // (... LET GO OF once the TARGET rises past what a planted knee can serve, 2026-10-28:
                    // the hips' height the target asks for — the root's start plus the target's rise over
                    // the effector's — over the knee's planted height, against the ROOT'S STANDING HEIGHT
                    // OVER THE KNEE at bind, of which a tall kneel is 1.0 (the thigh vertical):
                    // kKneeHoldReleaseFrom..To of it, a function of the target; the last value kept
                    // through the release's settle. Held for the whole drag, a figure sat back on her
                    // heels could not be pulled up off her knees: the hips rose to a tall kneel and
                    // stopped against a pull of 75cm. (Sized by `thigh` above — the PARENT bone's length,
                    // the thigh's twist bone on this generation, 25cm — a first cut let the belly's
                    // prone-up go at 25cm of rise.) IK_JS_KNEE_HOLD_FOR_DRAG.)
                    static const bool kHoldForDrag = std::getenv("IK_JS_KNEE_HOLD_FOR_DRAG") != nullptr; // A/B probe
                    float kneeHold = belowAtPress ? 1.0f : 0.0f;
                    if (belowAtPress && !kHoldForDrag && node < m_jsKneeHold.size()) {
                        const std::size_t rootIndex = static_cast<std::size_t>(root);
                        if (s.dragTarget != nullptr && static_cast<std::size_t>(effector) < m_jsStartPos.size() && rootIndex < m_jsStartPos.size() &&
                            rootIndex < m_ikBindPos.size()) {
                            const float standingOverKnee = m_ikBindPos[rootIndex].y - m_ikBindPos[node].y;
                            const float hipsTargetY = m_jsStartPos[rootIndex].y + (s.dragTarget->y - m_jsStartPos[static_cast<std::size_t>(effector)].y);
                            const float overKnee = hipsTargetY - pin.target.y;
                            kneeHold = standingOverKnee > 1.0e-3f
                                           ? 1.0f - glm::smoothstep(kKneeHoldReleaseFrom * standingOverKnee, kKneeHoldReleaseTo * standingOverKnee, overKnee)
                                           : 1.0f;
                            m_jsKneeHold[node] = kneeHold;
                        } else {
                            kneeHold = m_jsKneeHold[node];
                        }
                    }
                    kneeBelowHips = belowAtPress && kneeHold > 0.5f;
                    kneeHoldRelease = belowAtPress ? 1.0f - kneeHold : 1.0f; // (the rise's unload comes in with the release)
                    if (kneeBelowHips && node < m_jsKneeHeldDown.size()) {
                        m_jsKneeHeldDown[node] = 1; // (its lying foot stays lying: the flat block)
                    }
                    // (Tried and taken out, 2026-09-26: holding a knee down while the lying foot below it
                    // rolled back onto its toes, so the weight would stay on the knees while the toes
                    // tucked under. It stalled the rise — the grab 47mm behind its cursor — and let go of
                    // everything in one tick at the handover: 240-260mm jumps on every rig. The unroll's
                    // foot is held by its BALL instead, hard in place: the leg straightens over it.)
                    static const bool kKneeCeilingTrace = std::getenv("IK_JS_KNEE_CEILING_TRACE") != nullptr;
                    if (kKneeCeilingTrace) {
                        std::fprintf(stderr, "[knee-ceiling] node=%zu socket=%d over=%.3f thigh=%.3f below=%d rise=%.2f", node, socket, over, thigh, kneeBelowHips ? 1 : 0, m_jsRootRise);
                        std::fputc(10, stderr);
                    }
                    // (... and such a knee may not RISE either: free in its height, the solve lifted
                    // it under the rising hips — the thigh swinging up from the knee instead of the
                    // hips coming up over it — and the lift-off let it go: 14-16cm at the shins.)
                    // (Whatever the hips' height: a knee that is still a support keeps its height,
                    // and slides if it must — pulled by hips rising up and back off a prone body,
                    // free in its height, the knee rose 13cm toward them, lifted off and re-planted:
                    // a 15cm pop at the shins. The rise's own fade, below, is what unloads it.)
                    // (Only such a knee: one a drag found its hips UP over — a kneel — keeps the free
                    // height it always had, and rises with the rise's own fade; a ceiling on it, even
                    // one fading with the hold, lagged the hips 22mm and let go 9cm at once.)
                    if (kneeBelowHips && !pin.hard) {
                        kneeCeiling = true;
                    }
                    // (ON HER KNEES under a trunk drag: the knees stay down while the trunk can
                    // give what is asked — see m_jsKneelHold, dragIkTick. IK_JS_NO_KNEEL_HOLD.)
                    static const bool kNoKneelHold = std::getenv("IK_JS_NO_KNEEL_HOLD") != nullptr; // A/B probe
                    if (!kNoKneelHold && !pin.hard && m_jsKneelHold > 1.0e-3f) {
                        handCeilings.emplace_back(pin.node, pin.target.y, m_jsKneelHold);
                    }
                }
            }
            // (A SHADOW — a contact that has lifted, kept only for the support it lends — holds
            // nothing: its rows have faded with the joint's own rise by the time it is one, and
            // no ceiling comes back on it (below), whatever its arm's slack says: kept on the
            // shadow, the ceiling nailed a lifted hand to the floor at the arm's length while
            // the other hand hung. Set to gone outright here instead, the rows went in one tick:
            // 88mm at a fingertip. IkRig::pinIsShadow.)
            // (A KNEEL-UP is the trunk LEAVING its hands: a chest lifted off all fours has no rise
            // of its own — the trunk comes up about the hips, the knees held down — and the hands
            // were left to the fade of their own rise, which a hand standing on its fingertips
            // cannot make: the oldest generation's, the carpals its contact joints 13cm up and no
            // pitch left in the wrist, held its trunk down 17cm short by the fingertip's row and
            // the palm's — a deadlock, the row that would let the wrist rise waiting on the rise.
            // The HAND contacts of a kneel-start trunk drag unload by its upward travel,
            // m_jsKneelUpRise — a function of the target, as m_jsRootRise is — whatever their arms
            // read; the knees keep their hold. IK_JS_NO_KNEELUP_UNLOAD.)
            static const bool kNoKneelUpUnload = std::getenv("IK_JS_NO_KNEELUP_UNLOAD") != nullptr; // A/B probe
            if (!kNoKneelUpUnload && m_jsWristOf.size() == n && m_jsWristOf[node] >= 0) {
                gone = std::max(gone, m_jsKneelUpRise);
            }
            // THE RISE SINCE THE CONTACT FORMED (m_jsContactRiseBase; IK_JS_RISE_UNLOAD_ABSOLUTE is the
            // drag's whole rise, as before): a contact that lands under a body already risen holds in
            // full, and is unloaded by what the body rises from there.
            static const bool kAbsoluteRise = std::getenv("IK_JS_RISE_UNLOAD_ABSOLUTE") != nullptr; // A/B probe
            const float riseNow = std::max(m_jsRootRise, kNoDipUnload ? 0.0f : m_jsContactRise);
            if (node < m_jsContactRiseBase.size() && m_jsContactRiseBase[node] < 0.0f) {
                m_jsContactRiseBase[node] = riseNow;
            }
            const float riseSince = (kAbsoluteRise || node >= m_jsContactRiseBase.size())
                                        ? riseNow
                                        : std::max(0.0f, riseNow - m_jsContactRiseBase[node]);
            // (A HAND's unload comes in with its arm's TAUT share — handTautShare, 2026-10-27 — never
            // at once: "slack" had been a switch, and thrown with the body's rise already complete
            // it took the hold from 1e8 to nothing in one solve. And the arm the pin has stretched
            // is REFERRED TO WHERE THE UNLOAD FINDS IT — its channels re-based as a post-step's are,
            // rebaseContactArm — so the fading hold leaves the arm as it stands for the hang to take,
            // instead of snapping it back toward the fold the drag began in: 219mm at a thumb, the
            // collar 13 degrees, the shoulder 15 and the forearm 20 in one tick — the prone-up's pop.)
            float unloadRise = riseSince * handTaut;
            // THE TRAILING HAND LETS GO UNDER A DESCENT (2026-10-29; kHandTrailFrom/Full): under a
            // pelvis drag going down, a hand the body has slid forward OVER — its arm taut, its
            // wrist behind its shoulder along the heading by that share of the arm's span — is a
            // leash, not a support (the prone's arms: straight, horizontal, the chest held back
            // against the cursor by their length), and is unloaded as the rise unloads a taut hand:
            // the same eased fade, the arm re-based, the rig's lift-off — which the descent's
            // no-lift-off rule stands down for (the 4 bit below: IkRig::trailReleased).
            static const bool kNoHandTrail = std::getenv("IK_JS_NO_HAND_TRAIL") != nullptr; // A/B probe
            bool trailing = false;
            float trailRead = 0.0f; // (the trace's)
            glm::vec3 socketRead(0.0f);
            if (!kNoHandTrail && effector == root && m_jsTargetDown > 0.0f && m_jsWristOf.size() == n && m_jsWristOf[node] >= 0 &&
                rig.liveSpan(p) > 0.0f) {
                const int socket = armSocketOf(static_cast<int>(node));
                if (socket >= 0) {
                    const glm::vec3 behind = glm::vec3(m_poseGlobal[static_cast<std::size_t>(socket)][3]) - glm::vec3(m_poseGlobal[node][3]);
                    const float trail = glm::dot(behind, fore) / rig.liveSpan(p);
                    trailRead = trail;
                    socketRead = glm::vec3(m_poseGlobal[static_cast<std::size_t>(socket)][3]);
                    // (By the trail alone, not the arm's taut share: a hand 0.75 of its span behind the
                    // shoulder is a leash whatever the elbow reads — by the share the base rig's release
                    // stalled at 0.28 with the elbow straight and the hand 0.9 spans behind.)
                    const float unloadTrail = m_jsTargetDown * glm::smoothstep(kHandTrailFrom, kHandTrailFull, trail);
                    if (unloadTrail > unloadRise) {
                        unloadRise = unloadTrail;
                        trailing = true;
                    }
                }
            }
            // ... AND UNDER THE PRONE THE HANDS ARE LEASHES BY THE SHARE (2026-10-29; m_jsProneLie, a
            // function of the target: a kneel-start pelvis drag's target ahead of the planted knees and
            // down toward the belly). The trail alone read the base rig's arms but not the third
            // generation's: its hands stood 22cm behind the shoulders and 41cm below them at the
            // press, the arms folded back to the shoulder's extension limit, and the hips stuck
            // dead for thirty ticks (0.3mm a tick against a cursor 27cm ahead) at a trail of 0.32,
            // then paid the backlog off in 5cm jumps as the hold fell six decades — a leash released
            // by weight releases geometrically. By the share the hands let go as the body lies down
            // — through the same eased unload — before any backlog forms.
            if (!kNoHandTrail && effector == root && m_jsProneLie > unloadRise && m_jsWristOf.size() == n && m_jsWristOf[node] >= 0) {
                unloadRise = m_jsProneLie;
                trailing = true;
            }
            if (m_jsWristOf.size() == n && m_jsWristOf[node] >= 0 && node < m_jsHandUnload.size()) {
                // (EASED IN TIME, kHandUnloadStep a tick — a hand's hold fades over ten ticks however
                // fast its arm crosses the taut band: at a drag's pace the band is two ticks, and the
                // trunk the hand had been holding down sprang 122mm at the brow in the one tick the
                // hold went. IK_JS_HAND_UNLOAD_SWITCH: unlimited, as before.)
                if (!kHandUnloadSwitch) {
                    const float prev = m_jsHandUnload[node];
                    static const float kStep = envOr("IK_JS_HANDUNLOADSTEP", kHandUnloadStep); // (A/B lever)
                    unloadRise = glm::clamp(unloadRise, prev - kStep, prev + kStep);
                }
                m_jsHandUnload[node] = unloadRise;
            }
            unloadRise *= kneeHoldRelease; // (a held knee's unload comes in with the release: the knee-hold block)
            if (!kNoRiseUnload && (!kneeBelowHips || kneeHoldRelease > 1.0e-4f) && unloadRise > 1.0e-4f) {
                // (A TRAILING hand's hold goes at the trail's EASED pace and no faster, 2026-10-29: its
                // rise is the leash's own doing — held in x/z, free in y, a hand behind a shoulder
                // moving away is lifted by the arm's length the moment the arm is taut, and the
                // joint's own two-centimetre fade let the leash go in three ticks: the trunk sprang
                // into its lying reference, 90mm at the brow. The leash is let go of through the
                // unload, which is eased for exactly this.)
                gone = trailing ? unloadRise : std::max(gone, unloadRise);
                if (m_jsWristOf.size() == n && m_jsWristOf[node] >= 0) {
                    rebaseContactArm(node);
                }
                // (A HAND the rise has unloaded outright is TAUT to the rig's lift-off — see
                // m_jsContactRiseUnloaded: its arm has no length to give by the solve's measure,
                // and left to the rig's — the junction's reach — a straight arm on a shrugged
                // collar never read taut, its hand riding the rising chest 17cm into the air
                // weightless with its pin standing.)
                // (... and a hand whose hold the rise has taken OUTRIGHT (the rise term at 1: the
                // row's weight is zero) is let go of by the rig AT ONCE, whatever its height —
                // 2026-10-27: a prone-up's hand pressed to the floor under a rising chest, its
                // arm folded beneath, was never 2cm up for the three ticks the lift-off asks,
                // held nothing, and the torso's volume rows threw the weightless arm out from
                // under the chest in one tick, 219mm at a thumb.)
                // (... and while the unload is still EASING IN — under kRiseUnloadedShare — the rig is
                // told so (3, IkRig::riseUnloading): its lift-off by the junction's reach WAITS for
                // the hold to fade. Released on the reach alone with the hold still at 1e5, the trunk
                // the hand had been holding down sprang 83mm at the brow in the tick the pin went.)
                if (m_jsWristOf.size() == n && m_jsWristOf[node] >= 0 && node < m_jsContactRiseUnloaded.size()) {
                    m_jsContactRiseUnloaded[node] = static_cast<char>((unloadRise >= kRiseUnloadedOutrightShare ? 2
                                                                       : unloadRise >= kRiseUnloadedShare       ? 1
                                                                                                                : 3) |
                                                                      (trailing ? 4 : 0)); // (4: the descent lets it go — IkRig::trailReleased)
                }
            }
            // (The six decades end at 100/m2 — `gone` eases toward 1 and never reaches it — and a
            // contact kept past its band holds that much however far its joint rises: a hand riding
            // the rising shoulder through its eased unload, a knee whose lift-off waits for its
            // foot's handover. A TAIL taking the hold on down to nothing by the joint's rise was
            // tried on 2026-10-28 and removed the same day: a hand rolling onto its fingertips in
            // the prone raises its WRIST 6cm with the tips still down, and let go outright there the
            // arm was thrown 140-238mm (two rigs' prone failing). What the phantom spring strains is
            // answered where it is stored instead — see rebaseContactArm.)
            task.weight *= gone >= 1.0f ? 0.0 : std::pow(10.0, -6.0 * static_cast<double>(gone));
            // A TRAILING HAND IS A FRICTION CONTACT (2026-10-29; kHandTrailPull): its pull is BOUNDED
            // while its weight fades. Released by weight alone the leash let go GEOMETRICALLY — the
            // third generation's hips stood dead for thirty ticks against a cursor 29cm ahead (the
            // arm folded back to its limits, the hold still 1e5) and paid the backlog off in one
            // 112mm tick when the weight fell below what the pull could stretch. Bounded, the hand
            // slides along the floor behind the body by what the cursor's pull exceeds the bound,
            // from the trail's first tick, and the fade takes the rest.
            // (... and the BOUND fades with the unload — kHandTrailPull x (1 - gone): kept at 300 while the
            // weight fell six decades, the leash was as strong at gone 0.99 as at 0.1 and went in the one
            // tick the weight reached zero — 140mm at a shoulder on the third generation.)
            if (trailing && task.weight > 0.0) {
                task.huber = kHandTrailPull * static_cast<double>(1.0f - gone) / (2.0 * task.weight);
            }
            contactGone = gone;
            static const bool kFadeTrace = std::getenv("IK_JS_CONTACT_FADE_TRACE") != nullptr; // which contact is letting go, and why
            if (kFadeTrace && (gone > 1.0e-3f || trailRead > 0.0f)) {
                std::fprintf(stderr, "[fade] node=%zu hard=%d rise=%.4f gone=%.3f rootRise=%.2f contactRise=%.2f kneelUp=%.2f armSlack=%d kneeBelowHips=%d trailing=%d trail=%.2f span=%.3f socket(%.3f %.3f %.3f) hand(%.3f %.3f %.3f) hips(%.3f %.3f %.3f) target.y=%.4f", node, pin.hard ? 1 : 0, rise, gone, m_jsRootRise, m_jsContactRise, m_jsKneelUpRise, armSlack ? 1 : 0, kneeBelowHips ? 1 : 0, trailing ? 1 : 0, trailRead, rig.liveSpan(p), socketRead.x, socketRead.y, socketRead.z, m_poseGlobal[node][3].x, m_poseGlobal[node][3].y, m_poseGlobal[node][3].z, m_poseGlobal[static_cast<std::size_t>(root)][3].x, m_poseGlobal[static_cast<std::size_t>(root)][3].y, m_poseGlobal[static_cast<std::size_t>(root)][3].z, pin.target.y);
                std::fputc(10, stderr);
            }
            // (The KNEEL-UP's unload only: a hand's rows fade to nothing for a tick as it bobs
            // under a held prone too, and handed to the hang for that tick the arm turned 4
            // degrees and re-planted against its palm rows — a 224mm pop on the newest rig.)
            if (m_jsKneelUpRise >= 0.999f && gone >= 0.999f && node < m_jsContactUnloaded.size() &&
                m_jsWristOf.size() == n && m_jsWristOf[node] >= 0) {
                m_jsContactUnloaded[node] = 1;
            }
            // (The knee's ceiling FADES with its hold — held to the floor until the rise was complete
            // and let go of at once, the knees of a figure getting up out of a kneel jumped 21cm.)
            if (kneeCeiling && gone < 0.999f) {
                handCeilings.emplace_back(pin.node, pin.target.y, static_cast<float>(std::pow(10.0, -6.0 * static_cast<double>(gone))));
            }
            // THE KNEE LIFTS AS ITS FOOT ROLLS ONTO ITS TOES (A KNEELING FOOT LIES FLAT, 2026-09-26): a
            // knee whose lying foot is still rolling back gets a ceiling that RISES with the roll's
            // progress (kKneeUnrollLift), so the knee comes up as the toes tuck under and no faster.
            // Held down outright until the foot had handed over, the rise stalled (the grab 47mm behind
            // its cursor) and let go of everything in one tick — 260mm; free, the knee lifted with the
            // hips while the foot still hung in the air on its soft spring, and where the foot hung when
            // its rows returned decided the pop: 52 to 275mm, rig by rig and variant by variant.
            // (The ceiling itself is made in ikPinRowsEnd, from the planted height recorded here —
            // so it SURVIVES the knee pin's lift-off, 2026-10-28: tied to the pin, it went the tick the
            // knee rose 2cm off the floor, a third of the way through the roll, and a knee under a
            // cursor yanked 70cm up caught up in one tick, 375mm at the shin.)
            if (rig.isLegJointAboveFoot(pin.node) && m_jsFootUnrollProgress.size() == n && m_jsKneeUnrollBaseY.size() == n) {
                for (std::size_t f = 0; f < m_jsFootKnee.size() && f < m_jsFootWasFlat.size() && f < m_jsFootUnrolled.size(); ++f) {
                    if (m_jsFootKnee[f] == static_cast<int>(node) && m_jsFootWasFlat[f] && !m_jsFootUnrolled[f]) {
                        m_jsKneeUnrollBaseY[f] = pin.target.y;
                        m_jsKneeUnrollBaseValid[f] = 1;
                    }
                }
            }
        }
        // (The palm's pull goes as the hand unloads: a hand coming off the floor hangs as it will.)
        for (JointPositionTask& tipDown : tipRows) {
            static const double kTipHold = envOr("IK_JS_FINGERTIPHOLD", kFingertipHold);
            tipDown.weight = std::min(task.weight, kTipHold); // (soft, and gone with the contact's fade)
            // (... and NOT a TRAILING hand's, 2026-10-29: held down by its fingertips while its arm was
            // JAMMED — the third generation's collar at two of its limits, its wrist at 80 — the hand
            // was a strut from the floor to the chest that no bound on its x/z hold could shorten (a
            // hand may slide, but sliding lowers nothing): the hips stood dead for thirty ticks and
            // dropped 11cm when the row had faded to 4e4 and the hand popped up. A leash's hand lifts
            // as its arm pivots.)
            // (BOUNDED, not cut: cut at the trail's onset the hands popped up at once — 158-165mm on
            // two rigs. A friction contact's hold on the fingertips is the hand's weight on the floor,
            // kHandTrailPull as the x/z hold's: the hand lifts as the body's pull exceeds it.)
            if (node < m_jsContactRiseUnloaded.size() && (m_jsContactRiseUnloaded[node] & 4) != 0 && tipDown.weight > 0.0) { // (the fade block's `trailing`)
                tipDown.huber = kHandTrailPull * static_cast<double>(1.0f - contactGone) / (2.0 * tipDown.weight);
            }
            problem.positions.push_back(tipDown);
        }
        for (JointOrientationTask& flat : palmRows) {
            flat.axisWeights *= static_cast<double>(1.0f - contactGone);
            problem.orientations.push_back(flat);
        }
    }
}

/// THE FEET COME UP as she lies forward off her knees (the foot's rows let go), and the pin's place row
/// is pushed — the soft heel of a foot, the hard place of a hand or a knee — with the knee-drag foot's.
void Armature::ikPinRowFootFree(IkSolveScratch& s, IkPinRowState& st) {
    IkRig& rig = s.rig;
    const int root = s.root;
    const int effector = s.effector;
    const std::vector<IkEffector>& pins = s.pins;
    JointSolver::Problem& problem = s.problem;
    std::size_t& kneePlaceRow = s.kneePlaceRow;
    std::vector<char>& footFreed = s.footFreed;
    const std::size_t p = st.p;
    const IkEffector& pin = *st.pin;
    const std::size_t node = st.node;
    const double weight = st.weight;
    const int ball = st.ball;
    const bool primaryKneeFoot = st.primaryKneeFoot;
    const bool kneeFoot = st.kneeFoot;
    JointPositionTask& task = st.task;
    float& flat = st.flat;
    bool& unrolling = st.unrolling;
    bool& toLying = st.toLying;
    double& footFree = st.footFree;

    // THE FEET COME UP AS SHE LIES FORWARD OFF HER KNEES. A standing foot behind a PLANTED KNEE
    // is no support once the hips have gone ahead of that knee (a pelvis drag from a kneel or
    // all fours toward the floor in front): the knee is, and a body lowered onto its belly
    // folds its shins up behind it. Held on the floor by the foot's rows, the shin lay along
    // the floor and the KNEE'S 155-degree limit stopped the thigh 25 degrees short of flat —
    // the hips 22-35cm up, whatever the cursor asked. The foot's rows fade with the hips'
    // travel AHEAD of its knee (kProneFootFreeFrom .. Full, figure-scaled); the floor's rows
    // keep it from sinking, and nothing else holds it down. Not under a limb's drag.
    // (... and NOT a LYING foot, 2026-09-26: the rule is a tucked foot's, whose shin must fold up
    // behind as the thigh lays forward — a lying foot's shin stays where it lies and the knee
    // OPENS as the hips go ahead of it, the legs straightening into a flat prone. Let go of
    // outright, the lying ankle's one hold — the soft heel row at its lying place — went in a
    // tick, the thigh's twist relaxed 9 degrees toward its reference and swung the lying leg up
    // and sideways: 140mm at a toe, and both legs pointing up in the prone. A/B: IK_JS_NO_KNEEL_FLAT.)
    if (!kneeFoot && !toLying && !rig.pinIsUser(p) && !rig.pinIsLive(p) && ball >= 0 && effector == root && m_jsProneShare > 0.0f) {
        for (std::size_t q = 0; q < pins.size(); ++q) {
            if (q == p || !rig.pinIsLive(q) || pins[q].node < 0) {
                continue;
            }
            bool above = false;
            for (int cur = m_bones[node].parent; cur >= 0 && cur != root; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                above = above || cur == pins[q].node;
            }
            if (above) {
                // (Let go OUTRIGHT, not faded: a hard row given up through its weight does nothing
                // for nine tenths of the fade and goes in the last — the feet flipped 16cm in a
                // tick. Nothing pulls a resting foot up the tick its rows go; it rises as the
                // thigh comes forward and the knee opens, at the drag's pace.)
                footFree = 1.0;
                break;
            }
        }
    }
    task.weight *= 1.0 - footFree;
    if (footFree > 0.0) {
        footFreed[p] = 1; // (... and the floor's rows take over its height: see floorExempt)
    }
    if (unrolling) {
        // (A KNEELING FOOT LIES FLAT: the ankle rides its shin while the foot rolls back onto its
        // toes. After the handover the row is back in full, its target measured from the tucked
        // foot as it stands — see m_jsFlatRiseBase.)
        task.weight *= std::pow(10.0, -kKneelFlatRowDecades * static_cast<double>(flat));
    }
    problem.positions.push_back(task);
    if (kneeFoot) {
        if (primaryKneeFoot) {
            kneePlaceRow = problem.positions.size(); // (the knee drag's lift fades THIS row: the primary's alone)
        }
        JointPositionTask place;
        place.bone = pin.node;
        place.target = glm::dvec3(m_jsPinTarget[node]);
        place.weight = weight;
        place.axisScale = glm::dvec3(1.0, 0.0, 1.0);
        problem.positions.push_back(place);
    }
}

/// A STANDING FOOT's rows: the ball (hard), the heel's soft target (the tiptoe's lift), the sole's and the
/// toes' orientation holds — and the lying foot's variants of each.
void Armature::ikPinRowFootModel(IkSolveScratch& s, IkPinRowState& st) {
    IkRig& rig = s.rig;
    const int root = s.root;
    const std::vector<IkEffector>& pins = s.pins;
    const float& legScale = s.legScale;
    int& kneeFootPin = s.kneeFootPin;
    float& kneeLift = s.kneeLift;
    JointSolver::Problem& problem = s.problem;
    bool& pinsEasing = s.pinsEasing;
    const std::size_t p = st.p;
    const IkEffector& pin = *st.pin;
    const std::size_t node = st.node;
    const double weight = st.weight;
    const int ball = st.ball;
    const bool kneeFoot = st.kneeFoot;
    const std::size_t heelTask = st.heelTask;
    float& flat = st.flat;
    float& flip = st.flip;
    bool& flatFoot = st.flatFoot;
    bool& unrolling = st.unrolling;
    bool& toLying = st.toLying;
    float& rollProgress = st.rollProgress;
    bool& lyingKneeFoot = st.lyingKneeFoot;
    double& footFree = st.footFree;
    const auto foundFootYaw = [&](int ballBone, float& yaw) { return footYawFound(node, ballBone, yaw); };

    // The ball bears, hard, carried with the pin target (a step swings the whole foot)
    // and approached at the pins' pace from where the ball is (see m_jsBallTarget).
    {
        glm::vec3 ballGoal = pin.target + m_jsBallOffset[node];
        if (flatFoot) {
            // (The lying ball, and — once the foot has rolled back onto its toes — the standing
            // model's: the ball where it lay, at its own floor height, the re-seated ankle pin a
            // heel's length behind it.)
            const glm::vec3 standing = standingBallOffset(node, ball);
            // (Where the ball LAY, at its own floor height: a kneeling person stands up without
            // moving her feet — the toes tuck under in place, the ankle rises over them, and the
            // heel comes down behind.)
            ballGoal = pin.target + glm::vec3(m_jsFlatBallOffset[node].x, flip >= 1.0f ? standing.y : m_jsFlatBallOffset[node].y, m_jsFlatBallOffset[node].z);
        }
        if (!m_jsBallTargetValid[node]) {
            m_jsBallTarget[node] = glm::vec3(m_poseGlobal[static_cast<std::size_t>(ball)][3]);
            m_jsBallTargetValid[node] = 1;
        }
        const glm::vec3 ballGap = ballGoal - m_jsBallTarget[node];
        const float     ballLen = glm::length(ballGap);
        // (A foot rolled back to standing lands gently — kKneelFlatLandStep a tick WHILE IT IS
        // LANDING, its ball's target still above its spot; once it stands its target moves at
        // the pins' pace. Eased gently for the rest of the drag, 2026-10-28, a STEP the rig then
        // took — 93cm forward, standing up off a sphinx-like all fours — walked the foot after
        // its pin at a third of the swing's pace: both feet landed 20-50cm behind their spots,
        // the stance error read 0 with the pins there, and she stood on her toes with her legs
        // angled back. Gentle in the HEIGHT alone, with the ball's few centimetres of travel at
        // the handover taken at the pins' pace, the kneel-ups' knee steps read 74-83mm where they
        // read 37-44. IK_JS_LAND_GENTLY_ALWAYS is the A/B.)
        // (... until the HANDOVER's landing is done — the target has met its spot once since the
        // handover, m_jsBallLanded — and the pins' pace from then, a step's swing and its landing
        // included: "while the target is above its spot" read a step's lift arc as a landing and
        // lowered the foot at 1cm a tick under a goal already on the floor, 210mm at the brow.)
        static const bool kLandGentlyAlways = std::getenv("IK_JS_LAND_GENTLY_ALWAYS") != nullptr; // A/B probe
        const bool landing = flatFoot && flip >= 1.0f && (kLandGentlyAlways || (node < m_jsBallLanded.size() && !m_jsBallLanded[node]));
        const float ballStep = landing ? kKneelFlatLandStep : kPinEaseStep;
        if (ballLen > ballStep) {
            m_jsBallTarget[node] += ballGap * (ballStep / ballLen);
            pinsEasing = true;
        } else {
            m_jsBallTarget[node] = ballGoal;
            if (flip >= 1.0f && node < m_jsBallLanded.size()) {
                m_jsBallLanded[node] = 1;
            }
        }
    }
    // The HEEL's soft target, for a foot the drag found PITCHED (toes tucked under a
    // kneel, heels lifted in a deep crouch) or a body that rises: where the ankle sits
    // over the ball — as found, turning to flat on the floor as the body rises — not the
    // rig's pin, which is healed to the ankle's STANDING height the moment the drag
    // begins. From a kneel that was 5cm of pull on each heel from the first tick, as
    // strong as the whole cursor's (2 x 2000 x 0.05): it distorted a kneel under any
    // drag, and rising it preferred whatever put the heels down soonest — hips at full
    // standing height and behind the feet with the chest still 30cm low, the torso bowed
    // to its limits between them. (And as found its horizontal part sat 5cm ahead of
    // where a flat foot's ankle belongs, which is why she stood up on 2cm of heel.)
    {
        const float scale = rig.sizeScale();
        const glm::vec3 bindOffset = m_ikBindPos[node] - m_ikBindPos[static_cast<std::size_t>(ball)];
        const glm::vec3& found = flatFoot ? m_jsFlatHeelOffset[node] : m_jsHeelOffset[node];
        const bool pitched = std::abs(found.y - bindOffset.y) > 0.01f * scale;
        if (pitched || m_jsRootRise > 0.0f || flatFoot) {
            // Flat: the bind offset turned to the foot's heading — its LATERAL axis's, as the
            // drag found it (the bone's own: a pitch does not move it). It was read off the
            // ankle-to-ball offset's horizontal part, which a foot standing on tucked toes
            // PAST upright has pointing backwards: on the oldest generation the "flat" ankle
            // was asked 11cm behind its ball, on the wrong side of it. Nobody saw while a row
            // more than 5cm from its target pulled no harder than at 5 (JointSolver::
            // residuals); pulled in full, she stood up out of a kneel on 3cm of heel.
            glm::vec3 flat = bindOffset;
            float     yaw = 0.0f;
            if (foundFootYaw(ball, yaw)) {
                flat = glm::mat3_cast(glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f))) * bindOffset;
            }
            float turn = m_jsRiseProgress;
            if (flatFoot && flip >= 1.0f) { // (from the tucked foot the handover left, not the drag's start)
                const float base = m_jsFlatRiseBase[node];
                turn = base < 0.999f ? glm::clamp((m_jsRiseProgress - base) / (1.0f - base), 0.0f, 1.0f) : 1.0f;
            }
            problem.positions[heelTask].target =
                glm::dvec3(m_jsBallTarget[node] + glm::mix(found, flat, turn));
        }
        // (A KNEELING FOOT LIES FLAT: the SHIN lies down with it, and STAYS. Nothing else brings
        // the ankle down once the foot no longer props it — the floor's rows only push up, and
        // the model has no gravity — so the heel's soft row asks the ankle DOWN to the lying
        // height (the knee's planted height: the shin's radius), by the foot's share; and it
        // holds the ankle at its planted PLACE: a lying shin is held to the floor by friction,
        // and with the ball's pin gone the thigh's twist relaxed to its reference and swung the
        // shin 10cm inward across the floor through a kneel's still hold.)
        // (... and IN THE AIR: the ankle keeps the height the roll found it at through the
        // roll's first half — the ball and the toes swing up and back off the floor — and the
        // shin lies down through the second. Asked down from the first tick, the foot rolled
        // over its ball and toe tips resting on the floor: the toes folded to their limits
        // under it, the floor's rows on their joints locked the roll, and the foot flipped 8cm
        // in the tick the active set gave — 131mm at a toe in a kneel's still hold.)
        // (... and only while the foot LIES or rolls: once it has been handed back to the standing
        // model (flip) the heel's target is the tucked foot's alone. Mixed in through the share's
        // fade after the handover too — until 2026-09-30 — the row, back at its full weight by
        // then, asked the ankle of a foot standing on its toes down to the lying height at the
        // RE-SEATED pin, a foot's length behind: as strong a pull as the cursor's, for the five
        // ticks of the fade. A foot that had unrolled in the air shrugged it off; one that keeps
        // its toes on the floor through the unroll, as a foot pitching about its true axis does,
        // lay down again under it and sprang back, the knee 13cm down and up: 17-27cm in a tick on
        // the rigs with a thigh twist bone. IK_JS_LYING_HEEL_AFTER_HANDOVER is the A/B.)
        static const bool kLyingAfterHandover = std::getenv("IK_JS_LYING_HEEL_AFTER_HANDOVER") != nullptr; // A/B probe
        if (flat > 0.0f && (flip < 1.0f || kLyingAfterHandover)) {
            const float rollDone = glm::smoothstep(0.5f, 1.0f, rollProgress); // (1 for a foot found lying)
            const float lyingY = glm::mix(m_jsFlatRollStartY[node], m_jsFlatLyingY[node], rollDone);
            glm::dvec3 lying(pin.target.x, lyingY, pin.target.z);
            // THE LYING ANKLE'S PLACE IS ON ITS SHIN'S REACH OF THE PLANTED KNEE (2026-09-28;
            // IK_JS_LYING_ANKLE_AT_SPOT is the A/B): toward the spot the foot stood on, a shin's
            // length from where the knee landed, at the height asked. It was the spot itself —
            // which a lying shin does not reach: the knee comes down over a foot standing on
            // its tucked toes, the ankle up over its ball, a palm ahead of where it stood, and
            // the drag that MAKES a kneel ended with the row 2-9cm from its target on every
            // rig (23mm on the base, 87 on the oldest). A soft row that can never be met is a
            // standing pull on the leg, through a hold the solve creeps under: on two
            // long-footed characters both shins went from one side-bend limit to the other in
            // one tick of the still hold, the thighs twisting 5 degrees — 34-38mm at a toe.
            static const bool kLyingAtSpot = std::getenv("IK_JS_LYING_ANKLE_AT_SPOT") != nullptr; // A/B probe
            if (!kLyingAtSpot && toLying && m_jsFootKnee[node] >= 0) {
                const std::size_t kneeNode = static_cast<std::size_t>(m_jsFootKnee[node]);
                for (std::size_t q = 0; q < pins.size(); ++q) {
                    if (pins[q].node != static_cast<int>(kneeNode) || !rig.pinIsLive(q) || rig.pinIsShadow(q)) {
                        continue;
                    }
                    const glm::dvec3 knee(pins[q].target);
                    const double     shin = static_cast<double>(glm::length(m_ikBindPos[node] - m_ikBindPos[kneeNode]));
                    const double     rise = static_cast<double>(lyingY) - knee.y;
                    const double     reach = std::sqrt(std::max(shin * shin - rise * rise, 0.0));
                    const glm::dvec2 toSpot(static_cast<double>(pin.target.x) - knee.x, static_cast<double>(pin.target.z) - knee.z);
                    const double     away = glm::length(toSpot);
                    if (away > 1.0e-4 && reach > 1.0e-4) {
                        lying.x = knee.x + toSpot.x / away * reach;
                        lying.z = knee.z + toSpot.y / away * reach;
                    }
                    break;
                }
            }
            problem.positions[heelTask].target = glm::mix(problem.positions[heelTask].target, lying, static_cast<double>(flat));
            if (kKneelFlatTrace) {
                const glm::dvec3& t = problem.positions[heelTask].target;
                std::fprintf(stderr, "[kneel-flat]   %s lies at (%.3f %.3f %.3f), asked to (%.3f %.3f %.3f): %.1fmm, the spot it stood on (%.3f %.3f)", m_boneNames[node].c_str(),
                             m_poseGlobal[node][3].x, m_poseGlobal[node][3].y, m_poseGlobal[node][3].z, t.x, t.y, t.z,
                             glm::length(t - glm::dvec3(glm::vec3(m_poseGlobal[node][3]))) * 1000.0, pin.target.x, pin.target.z);
                std::fputc(10, stderr);
            }
        }
    }
    // THE TIPTOE (dragIkTick, m_jsTiptoe): a hand pulled up beyond its reach lifts the heels.
    // The root's home rises by the lift, and each heel's target with it — the ankle pitched
    // about ITS ball by m_jsTiptoeTheta: up by d sin t + h0 (cos t - 1), toward the ball by
    // d (1 - cos t) + h0 sin t, this foot's own d and h0 read off the target as it stands (the
    // ankle over its ball). Left where it was, the soft heel row (2000/m^2, twice) pulled the
    // heels down against the root's home and the body rose a fifth of the way.
    if (m_jsTiptoe > 0.0f && !rig.pinIsLive(p) && !rig.pinIsUser(p) && !kneeFoot) {
        const glm::dvec3 off = problem.positions[heelTask].target - glm::dvec3(m_jsBallTarget[node]);
        const glm::dvec3 horizontal(off.x, 0.0, off.z);
        const double d = glm::length(horizontal);
        const double h0 = off.y;
        const double t = m_jsTiptoeTheta;
        glm::dvec3 delta(0.0, d * std::sin(t) + h0 * (std::cos(t) - 1.0), 0.0);
        if (d > 1.0e-6) {
            delta -= horizontal / d * (d * (1.0 - std::cos(t)) + h0 * std::sin(t));
        }
        problem.positions[heelTask].target += delta;
    }
    JointPositionTask bear;
    bear.bone = ball;
    bear.target = glm::dvec3(m_jsBallTarget[node]);
    bear.weight = weight;
    if (kneeFoot) {
        bear.axisScale = glm::dvec3(0.0, 1.0, 0.0);
    }
    // THE FOOT BELOW A KNEE THAT IS ON THE FLOOR bears in its height and keeps its place only
    // softly (kKneelFootHold): the knee is that leg's support now, and a kneeling person's
    // toes are not nailed down. Held in full, the knee's pin and the ball's — two 1e8 rows a
    // shin and a tucked foot at its ankle's limits apart — were never quite compatible, and
    // the difference leaked into the one thing that moves both: the PELVIS's pitch (the
    // ball's rows asked +195 of it, the knee's -148). A kneel dragged a few centimetres off
    // the rig's own tipped the pelvis 27 degrees and the pelvis bone 25 more, to its limit,
    // under an arched back — which no gate measured, and the first rendered frame showed.
    static const bool kKneelFootHard = std::getenv("IK_JS_KNEEL_FOOT_HARD") != nullptr; // A/B probe
    if (!kKneelFootHard && !kneeFoot && !rig.pinIsUser(p)) {
        for (std::size_t q = 0; q < pins.size(); ++q) {
            // (A SHADOW — a lifted contact the body still leans on, its hold long faded — is no
            // knee ON the floor: a foot under one is a standing foot, its place hard. Taken for
            // a knee, the feet of a figure rising by the chest off a kneel kept the kneeling
            // foot's soft place while she stood up on them: 105mm jumps, 175 on one rig.)
            if (q == p || !rig.pinIsLive(q) || rig.pinIsShadow(q) || pins[q].node < 0) {
                continue;
            }
            bool above = false;
            for (int cur = m_bones[node].parent; cur >= 0 && cur != root; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                above = above || cur == pins[q].node;
            }
            if (above) {
                static const double kFootHold = envOr("IK_JS_KNEELFOOTHOLD", kKneelFootHold);
                const double place = std::sqrt(kFootHold / weight);
                bear.axisScale = glm::dvec3(place, 1.0, place);
                break;
            }
        }
    }
    // (Its HEIGHT and the ankle's place only: the sole's and the toes' orientation holds stay,
    // and the ankle turns under them as the shin comes up. Let go of too, the foot — held
    // at its dorsiflexion limit under the tucked toes — sprang 10cm in the tick.)
    bear.weight *= 1.0 - footFree;
    // (A KNEELING FOOT LIES FLAT: rolling onto its top the ball is no bearing — the row goes by
    // decades over the share, its target tracking the ball. Let go of by its knee the foot
    // rolls back onto its TOES IN PLACE: the ball is pinned where it lay and the ankle rises
    // over it as the shin lifts. Faded through that roll too, the free leg followed its
    // kneeling posture references while the rise drew the hips up and away, and the foot
    // trailed 30cm behind in the air until its rows came back — a 130mm yank.)
    if (toLying || lyingKneeFoot) { // (a lying foot under a drawn-up knee: no bearing either, its toes on the floor's rows)
        bear.weight *= std::pow(10.0, -kKneelFlatRowDecades * static_cast<double>(flat));
    } else if (unrolling) {
        // (A soft spring on the ball's PLACE at its lying spot — and, softly too, on its HEIGHT
        // toward the floor (the tracking above): the toes come up through the roll, and pinned
        // HARD at the lying height they went 9cm through the floor; pinned hard in every axis
        // (tried 2026-09-26) the tuck was infeasible — the toes at their curl limits against a
        // nailed ball, the solve failing, the hips collapsing back onto the knees. Soft, the foot
        // cannot trail after a rising pelvis, nor fly up on a leg the rise folds.)
        bear.weight = kKneelFootHold;
        bear.axisScale = (kUnrollBallDown || kUnrollBallLifted) ? glm::dvec3(1.0, 1.0, 1.0) : glm::dvec3(1.0, 0.0, 1.0); // (lifted: UP, off the floor — see the tracking above)
    }
    // (After the handover the row is HARD at once: its target is the spot the spring held the
    // ball at, so nothing is yanked — faded in over the share the free foot trailed the rising
    // body 22cm and snapped back 195mm when the row was there.)
    // A FREED FOOT KEEPS THE UNROLL'S SPRING THROUGH THE HANDOVER (2026-10-28): a foot the prone
    // rule has freed — the hips ahead of its planted knee, footFree above — has no ball row after
    // the handover (the row returns at (1 - footFree) of its weight: nothing), and the spring that
    // held it through the unroll ended with the unroll. Until the knee's pin lifted off the foot
    // was held by nothing but the floor, and a stand-up by the hips off a sphinx-like all fours —
    // the hips 30cm ahead of the knees (the prone-up's), held 18cm behind the cursor by the knee
    // ceilings — flung the free leg 36cm forward through that gap and yanked it back when the pin
    // went and the row returned at 1e8 with its target 8cm from the ball: 528mm at a toe, the
    // body lurching 10cm toward the cursor and back. The spring stays until the row is back.
    static const bool kNoFreedSpring = std::getenv("IK_JS_FREED_FOOT_NO_SPRING") != nullptr; // A/B probe
    if (!kNoFreedSpring && footFree > 0.0 && flatFoot && flip >= 1.0f && !unrolling && !toLying) {
        bear.weight = std::max(bear.weight, static_cast<double>(kKneelFootHold) * footFree);
        bear.axisScale = glm::dvec3(1.0, 1.0, 1.0);
    }
    problem.positions.push_back(bear);
    // The sole's frame: pitch (about the foot's LATERAL axis), heading (about the
    // vertical), level (about the fore axis) — built from the foot BONE's own lateral
    // axis, which its bind gives (across the ankle-to-ball line, level) and which a
    // pitch does not move. It was built from the ankle-to-ball offset's horizontal part
    // in the pose the drag begins with — a fine heading for a flat foot and no heading at
    // all for one standing nearly upright on tucked toes (a kneel), where that part is a
    // couple of centimetres of roll and yaw: the hardest row of the foot then held a
    // wrong heading, and a figure stood up out of a kneel with both ankles twisted to
    // their limits (35 and 10 degrees), rolled onto the edges of her feet.
    glm::vec3 bindFore = m_ikBindPos[static_cast<std::size_t>(ball)] - m_ikBindPos[node];
    bindFore.y = 0.0f;
    const glm::vec3 lateralLocal = glm::length(bindFore) > 1.0e-4f
                                       ? glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), bindFore))
                                       : glm::vec3(1.0f, 0.0f, 0.0f);
    // (... and the foot below a DRAGGED KNEE turns flat as it TRAILS — once it is off the
    // floor, over kKneeLiftFull..kKneeTrailFull of lift, with the hang's swing (kneeHangOffset):
    // held as the drag found it, tucked toes-down under a kneel, a foot asked forward along
    // the floor under the rising knee ran its toe tips into the floor's rows, and folding the
    // shin to its limit, the foot up against the thigh, was the cheaper pose. Turned by the
    // lift's FIRST centimetres it was worse: a foot still standing on its tucked toes was
    // asked flat, its toes pushed 18mm into the floor against the floor's 1e7 rows, the
    // solve rejected every step from a cost of 6000 and the knee shot 8cm sideways.)
    float flatten = m_jsRootRise > 0.0f ? m_jsRiseProgress : 0.0f;
    if (flatFoot && flip >= 1.0f && m_jsRootRise > 0.0f) { // (a foot rolled back onto its toes turns flat from the handover on)
        const float base = m_jsFlatRiseBase[node];
        flatten = base < 0.999f ? glm::clamp((m_jsRiseProgress - base) / (1.0f - base), 0.0f, 1.0f) : 1.0f;
    }
    if (static_cast<int>(p) == kneeFootPin && !kHangAsFound) {
        flatten = std::max(flatten, glm::smoothstep(kKneeLiftFull * legScale, kKneeTrailFull * legScale, kneeLift));
    }
    const auto isToeMate = [&](int bone) {
        if (pin.node < 0 || static_cast<std::size_t>(pin.node) >= m_jsToeMates.size()) {
            return false;
        }
        for (const int mate : m_jsToeMates[static_cast<std::size_t>(pin.node)]) {
            if (mate == bone) {
                return true;
            }
        }
        return false;
    };
    for (std::size_t k = 0; k < m_ikFlatNodes.size(); ++k) {
        if (m_ikFlatOwner[k] != pin.node) {
            continue;
        }
        if (m_ikFlatNodes[k] == pin.node) {
            JointOrientationTask hold;
            hold.bone = pin.node;
            glm::mat3 target = m_ikFlatRot[k];
            // RISING (m_jsRootRise), the sole's target turns from the orientation the
            // drag began with toward FLAT on the floor, its heading kept (the lateral
            // axis's, which a pitched foot still has): a body that gets up out of a
            // kneel (toes tucked) or a deep crouch (heels lifted) puts its heels down as
            // it stands. Held to the pitched orientation, she stood up with her heels
            // 4cm off the floor — the pitch hold's pull meeting the soft heel row's.
            // (A lying foot under a drawn-up knee: its target is LEVEL from the start — the
            // lying orientation is not one a rising shin can keep — and the hold's weight comes
            // in with the flatten, below.)
            const float soleTurn = lyingKneeFoot ? 1.0f : flatten;
            if (soleTurn > 0.0f) {
                glm::quat level;
                if (flatKeepingHeading(target, lateralLocal, level)) {
                    target = glm::mat3_cast(glm::slerp(glm::quat_cast(target), level, soleTurn));
                }
            }
            hold.target = glm::dmat3(target);
            // (lateral, up made square to it, fore): right-handed like (x, y, z).
            const glm::dvec3 lateral = glm::normalize(glm::dvec3(target * lateralLocal));
            static const double kRoll = envOr("IK_JS_SOLEROLL", kSoleRollWeight);
            hold.perAxis = true;
            hold.frame = frameSquareTo(lateral);
            hold.axisWeights = glm::dvec3(kSolePitchWeight, weight * kPinOrientScale * kPinOrientScale, kRoll);
            if (kneeFoot) {
                hold.axisWeights[1] = kKneeHeadingW;
            }
            // (A KNEELING FOOT LIES FLAT: the sole's hold goes with the foot's share. Its pitch is
            // the plantar reference's; and its HEADING — the standing foot's, as the drag found
            // it — is not a lying foot's: a kneel turns the shins in over the feet by the thighs'
            // twist, and held to the heading it stood with the lying foot twisted 18 and rolled
            // 25 degrees against its shin to keep it. A lying foot follows its shin, its own
            // channels at their references.)
            if (lyingKneeFoot) {
                // (Rides its shin, and is held LEVEL over the trail's second half: a lying foot's
                // level target is 180 degrees away in pitch, and an orientation row that far off
                // cannot say which way round — by the trail's middle the shin has lifted the foot
                // to hang, the error is under 120 degrees, and the hold turns it flat to land.)
                hold.axisWeights *= static_cast<double>(glm::smoothstep(0.3f, 1.0f, flatten));
            } else {
                hold.axisWeights *= std::pow(10.0, -kKneelFlatRowDecades * static_cast<double>(flat));
            }
            // (A foot let go of behind a planted knee — THE FEET COME UP — keeps its sole's
            // hold, but two decades softer: at the heading's 4e6 the floor's rows lost, and
            // the newest generation's toes went 44mm through the floor as she lay down.)
            if (footFree > 0.0) {
                hold.axisWeights *= 0.01;
            }
            problem.orientations.push_back(hold);
        } else if (m_ikFlatNodes[k] == ball || isToeMate(m_ikFlatNodes[k])) {
            JointOrientationTask toes;
            toes.bone = m_ikFlatNodes[k];
            // The toes' target goes FLAT with the sole's as the body rises: tucked under
            // a kneel they are pitched, held there (this row is all but hard) a foot can
            // come no flatter than its toe joint lets it against them — and on the
            // generations with a short toe range the heels stayed 4-7cm up.
            glm::mat3 toeTarget = m_ikFlatRot[k];
            if (flatten > 0.0f) {
                glm::quat level;
                if (flatKeepingHeading(toeTarget, lateralLocal, level)) {
                    toeTarget = glm::mat3_cast(glm::slerp(glm::quat_cast(toeTarget), level, flatten));
                }
            }
            toes.target = glm::dmat3(toeTarget);
            toes.weight = kToeHoldWeight;
            // (A KNEELING FOOT LIES FLAT: the toes' hold — to the level orientation the drag
            // found them at — goes with the foot's share while the foot rolls onto its top; a
            // foot found lying keeps it, its found orientation being the lying one.)
            toes.weight *= std::pow(10.0, -6.0 * static_cast<double>(flat));
            if (kneeFoot) {
                // (The toes turn WITH their foot: their yaw is locked to it, so a toe
                // hold in full was the foot's heading held hard by another name.)
                const glm::dvec3 lateral = glm::normalize(glm::dvec3(toeTarget * lateralLocal));
                toes.perAxis = true;
                toes.frame = frameSquareTo(lateral);
                toes.axisWeights = glm::dvec3(kToeHoldWeight, kKneeHeadingW, kToeHoldWeight);
                if (lyingKneeFoot) {
                    // (A LYING FOOT UNDER A DRAWN-UP KNEE: no toes' hold at all — their posture
                    // reference, the straight toes the lying foot has, is what a landing foot
                    // wants too. Held to "level" — 127 degrees away for toes lying on their top —
                    // at 4e4, the drag's first step pushed them 16mm into the floor.)
                    toes.weight *= 1.0e-8;
                    toes.axisWeights *= 1.0e-8;
                }
            }
            // (The ball and its toe mates together weigh what one ball does: at the full
            // weight each, five level holds against a foot rolling onto its edge under a
            // swung knee ran every toe to its roll limit and a toe joint 6.5mm through the
            // floor on the generation whose toes fan from the mid-foot.)
            if (pin.node >= 0 && static_cast<std::size_t>(pin.node) < m_jsToeMates.size() && !m_jsToeMates[static_cast<std::size_t>(pin.node)].empty()) {
                const double share = 1.0 / static_cast<double>(1 + m_jsToeMates[static_cast<std::size_t>(pin.node)].size());
                toes.weight *= share;
                toes.axisWeights *= share;
            }
            problem.orientations.push_back(toes);
        }
    }
}

/// A USER PIN's six-degree hold, and THE SEAT's rock: a pinned pelvis keeps its heading and its level and
/// pitches under a trunk drag.
void Armature::ikPinRowHold(IkSolveScratch& s, IkPinRowState& st) {
    IkRig& rig = s.rig;
    const int root = s.root;
    JointSolver::Problem& problem = s.problem;
    std::vector<char>& inSpine = s.inSpine;
    const std::size_t p = st.p;
    const IkEffector& pin = *st.pin;
    const double weight = st.weight;

    for (std::size_t k = 0; k < m_ikFlatNodes.size(); ++k) {
        // (A foot mid-STEP keeps its orientation hold: it swings pointing where it pointed.
        // Released for the swing, the leg found the thigh's TWIST the cheapest way to carry
        // a bent leg's foot sideways: 50-60 degrees of knee roll mid-stride.)
        if (m_ikFlatNodes[k] == pin.node && m_ikFlatOwner[k] == pin.node) {
            JointOrientationTask hold;
            hold.bone = pin.node;
            hold.target = glm::dmat3(m_ikFlatRot[k]);
            hold.weight = weight * kPinOrientScale * kPinOrientScale;
            // THE SEAT ROCKS: a pinned pelvis keeps its place, its heading and its level, and
            // PITCHES under a trunk drag (m_jsTrunkHinge: the root's pitch is an unknown there,
            // one more link of the spine's chain). Held in full — six degrees, as any user pin
            // is — a seated figure's chest leaned 15cm forward was the spine's alone to reach,
            // a 90-degree curl it rightly refused: 27mm short, trailing the cursor all the way.
            // (The pelvis itself or a trunk bone of the girdle; a pinned THIGH is a thigh that
            // rests on the seat, and stays as it is.)
            static const bool kNoSeatRock = std::getenv("IK_JS_NO_SEAT_ROCK") != nullptr; // A/B probe
            if (!kNoSeatRock && static_cast<int>(p) == rig.seatPin() && m_jsTrunkHinge > 1.0e-3f &&
                (pin.node == root || inSpine[static_cast<std::size_t>(pin.node)] || rig.isPelvisBone(pin.node))) {
                const glm::dvec3 lateral = glm::normalize(hold.target * glm::dvec3(1.0, 0.0, 0.0));
                hold.perAxis = true;
                hold.frame = frameSquareTo(lateral);
                hold.axisWeights = glm::dvec3(0.0, hold.weight, hold.weight); // pitch, heading, level
            }
            problem.orientations.push_back(hold);
            break;
        }
    }
}

/// The PIN ROWS, part three: the rows that need every pin known — the letting-go foot's spring and hang, the elbow drag's hand hold, the knee's hanging foot.
void Armature::ikPinRowsEnd(IkSolveScratch& s) {
    const std::size_t n = s.n;
    const int effector = s.effector;
    // THE KNEE LIFTS AS ITS FOOT ROLLS ONTO ITS TOES (A KNEELING FOOT LIES FLAT, 2026-09-26): a knee
    // whose lying foot is still rolling back gets a ceiling that RISES with the roll's progress
    // (kKneeUnrollLift), so the knee comes up as the toes tuck under and no faster — with or without
    // the knee's pin (its planted height was recorded in ikPinRows while it stood; 2026-10-28) — and
    // goes on rising at the pins' pace for kKneeUnrollExtra after the handover, then is dropped.
    if (m_jsKneeUnrollBaseY.size() == n && m_jsFootUnrollProgress.size() == n) {
        static const bool kCeilingWithPin = std::getenv("IK_JS_KNEE_UNROLL_CEILING_WITH_PIN") != nullptr; // A/B probe
        for (std::size_t f = 0; f < n && f < m_jsFootKnee.size() && f < m_jsFootWasFlat.size() && f < m_jsFootUnrolled.size(); ++f) {
            if (!m_jsKneeUnrollBaseValid[f] || !m_jsFootWasFlat[f] || m_jsFootKnee[f] < 0) {
                continue;
            }
            const int knee = m_jsFootKnee[f];
            if (kCeilingWithPin) {
                bool pinned = false;
                for (const IkEffector& pin : s.pins) {
                    pinned = pinned || pin.node == knee;
                }
                if (!pinned || m_jsFootUnrolled[f]) {
                    continue;
                }
            }
            float lift = kKneeUnrollLift * s.rig.sizeScale();
            if (!m_jsFootUnrolled[f]) {
                lift *= m_jsFootUnrollProgress[f];
                m_jsKneeUnrollExtra[f] = 0.0f;
            } else {
                m_jsKneeUnrollExtra[f] += kPinEaseStep * s.legScale;
                if (m_jsKneeUnrollExtra[f] > kKneeUnrollExtra * s.rig.sizeScale()) {
                    m_jsKneeUnrollBaseValid[f] = 0; // (dropped: the knee is free)
                    continue;
                }
                lift += m_jsKneeUnrollExtra[f];
            }
            s.handCeilings.emplace_back(knee, m_jsKneeUnrollBaseY[f] + lift, 1.0f);
        }
    }
    const std::vector<IkEffector>& pins = s.pins;
    const float& legScale = s.legScale;
    bool& kneeDrag = s.kneeDrag;
    int& kneeAnkle = s.kneeAnkle;
    int& effectorFoldAxis = s.effectorFoldAxis;
    int& kneeFootPin = s.kneeFootPin;
    float& plant = s.plant;
    float& kneeLift = s.kneeLift;
    bool& elbowDrag = s.elbowDrag;
    int& elbowWrist = s.elbowWrist;
    int& elbowHeldPin = s.elbowHeldPin;
    glm::vec3& elbowGoal = s.elbowGoal;
    JointSolver::Problem& problem = s.problem;
    // (The ankle height at which every joint of the dragged knee's foot clears the floor, the foot
    // shaped as it is now: A KNEELING FOOT LIES FLAT, the lying foot under a drawn-up knee.)
    const auto lyingFootHangY = [&]() {
        float y = -1.0e9f;
        if (kneeAnkle < 0 || static_cast<std::size_t>(kneeAnkle) >= n) {
            return y;
        }
        const float ankleY = m_poseGlobal[static_cast<std::size_t>(kneeAnkle)][3].y;
        std::vector<int>& foot = s.subtreeBuf; // (the scratch's buffer)
        foot.clear();
        collectSubtree(kneeAnkle, foot);
        for (const int j : foot) {
            const float jointY = m_poseGlobal[static_cast<std::size_t>(j)][3].y;
            y = std::max(y, -m_transform[3][1] + s.rig.floorClearance(j) + (ankleY - jointY));
        }
        // (... plus a CLEARANCE while the foot still points down past level, fading as it comes
        // level so the landing is a landing: hung exactly at the height that put its lowest joint AT
        // the floor, the toes dragged and the floor's rows bent them up to their limits tick by
        // tick — the hang read off the shape the rows had just bent — and they sprang back 100mm
        // at a tip when the turning foot lifted them off.)
        y += kLyingFootHangClear * s.rig.sizeScale() *
             glm::smoothstep(kLyingFootHangClearFromDeg, kLyingFootHangClearToDeg, s.lyingFootPitchExcessDeg);
        return y;
    };
    // (... EASED at the pins' pace, 2026-10-28; IK_JS_LYING_HANG_NO_EASE: read fresh every tick, the
    // height drops 7cm a tick as the foot LEVELS — the sole's hold coming in over the trail's second
    // half turns it 22 degrees a tick on a heavy body — and the hang row, 300-400/m2 on a shin with no
    // price, pulls the ankle down and forward by that much; the shin swings 15-22 degrees a tick, and
    // the levelling foot drops the target further: 158-197mm at a toe on the two heavy bodies of the
    // sweep, 55 on the base. The horizontal target was already eased (ikKneeHangUnder).)
    const float lyingHangY = [&]() {
        static const bool kNoEase = std::getenv("IK_JS_LYING_HANG_NO_EASE") != nullptr; // A/B probe
        const float fresh = lyingFootHangY();
        if (kNoEase || fresh < -1.0e8f) {
            m_jsLyingHangYValid = 0;
            return fresh;
        }
        if (!m_jsLyingHangYValid) {
            m_jsLyingHangY = fresh;
            m_jsLyingHangYValid = 1;
            return fresh;
        }
        const float step = kPinEaseStep * legScale;
        m_jsLyingHangY += glm::clamp(fresh - m_jsLyingHangY, -step, step);
        return m_jsLyingHangY;
    }();
    std::vector<char>& pinned = s.pinned;
    bool& pinsEasing = s.pinsEasing;
    std::array<std::size_t, 4>& kneeFootRows = s.kneeFootRows;
    std::size_t& kneePlaceRow = s.kneePlaceRow;
    bool& kneeHangMerged = s.kneeHangMerged;
    std::vector<char>& handTipHeld = s.handTipHeld;

    for (std::size_t i = 0; i < n; ++i) {
        if (!pinned[i]) {
            m_jsPinTargetValid[i] = 0; // a pin that went away re-seeds if it comes back
            m_jsBallTargetValid[i] = 0;
        }
    }
    m_jsPinsEasing = pinsEasing || m_jsWeightEasing || s.flatEasing; // (... or the weight shift's home still on its way: ikPostureModel; or a foot still rolling onto its top, or back)
    // (A hand whose contact is gone lets go of its fingertip hold: the next landing plants it anew.)
    for (std::size_t i = 0; i < m_jsHandTipValid.size() && i < handTipHeld.size(); ++i) {
        if (!handTipHeld[i]) {
            m_jsHandTipValid[i] = 0;
        }
    }
    // The foot below a dragged knee holds by the PLANT: its rows fall by kKneeLiftDecades as the
    // knee's target is pulled up, and are gone when it is (the leg hangs from the knee).
    if (kneeFootPin >= 0 && plant < 1.0f) {
        if (static_cast<std::size_t>(kneeFootPin) + 1 == pins.size()) {
            kneeFootRows[1] = problem.positions.size();
            kneeFootRows[3] = problem.orientations.size();
        }
        const double fade = plant <= 0.0f ? 0.0 : std::pow(10.0, -kKneeLiftDecades * static_cast<double>(1.0f - plant));
        const float trail = glm::smoothstep(kKneeLiftFull * legScale, kKneeTrailFull * legScale, kneeLift);
        const double placeFade =
            trail >= 1.0f ? 0.0
                          : std::pow(10.0, -kKneePlaceSpringDecades * static_cast<double>(1.0f - plant) -
                                               (kKneeLiftDecades - kKneePlaceSpringDecades) * static_cast<double>(trail));
        for (std::size_t i = kneeFootRows[0]; i < kneeFootRows[1]; ++i) {
            problem.positions[i].weight *= i == kneePlaceRow ? placeFade : fade;
        }
        // The ankle's place and its hang under the knee are ONE row — two springs on one joint
        // are one spring at their weighted mean, and must be given as that: every row's ask of
        // a step is bounded (5cm), so two opposed rows that are both further than that from their
        // targets ask for equal and opposite steps and the model is stationary ANYWHERE between
        // them. The trailing foot stood wherever it had been left — by the spot on the way up, by
        // the knee on the way down — and jumped 9cm when one miss came under the bound.
        if (kneePlaceRow != static_cast<std::size_t>(-1) && kneeAnkle >= 0 &&
            static_cast<std::size_t>(kneeAnkle) < m_jsStartPos.size()) {
            JointPositionTask& place = problem.positions[kneePlaceRow];
            const double spring = place.weight;
            double hang = kKneeHangW * static_cast<double>(1.0f - plant);
            const glm::dvec3 under(ikKneeHangUnder(s));
            // (The PLACE spring's spot swings with the hang, by as much as the shin the drag found
            // was NOT vertical: a kneeling foot's spot is behind the knee on the floor, and held
            // there by the spring — still strong at 20cm of lift — the foot stayed tucked under the
            // hips while the knee went up and forward, the shin folded back; and left to the
            // spring's own fade the foot first followed the found offset UP behind the rising knee,
            // folded to the knee's limit, and flipped 25cm out of that basin in a tick. A standing
            // leg's shin is vertical: its spot stays, and the foot trails off it as it always did.
            // And such a foot's hang pulls kKneeSwingHangScale times harder: it has a swing to
            // make along the floor, against the floor's rows and the tucked toes' — at the
            // standing hang's 100/m^2 it rested 12cm behind the knee.)
            if (!kHangAsFound && static_cast<std::size_t>(kneeAnkle) < m_jsStartPos.size()) {
                const glm::vec3 found = m_jsStartPos[static_cast<std::size_t>(kneeAnkle)] - m_jsStartPos[static_cast<std::size_t>(effector)];
                const float     len = glm::length(found);
                const float     notVertical = len > 1.0e-4f ? 1.0f - glm::clamp(-found.y / len, 0.0f, 1.0f) : 0.0f;
                const float     swung = glm::smoothstep(0.0f, kKneeUnderLift * legScale, kneeLift);
                place.target = glm::mix(place.target, under, static_cast<double>(notVertical * swung));
                hang *= 1.0 + static_cast<double>(kKneeSwingHangScale * notVertical);
            }
            if (spring + hang > 0.0) {
                place.target = (place.target * spring + under * hang) / (spring + hang);
                if (s.lyingKneeFoot) {
                    // (A LYING FOOT UNDER A DRAWN-UP KNEE hangs CLEAR: the ankle's target is never lower
                    // than what puts the foot's lowest joint at its clearance given the foot's shape
                    // NOW — its toes trail behind and below it until the pitch reference has turned it
                    // level, and hung at the standing ankle height meanwhile it came down onto them.)
                    place.target.y = std::max(place.target.y, static_cast<double>(lyingHangY));
                }
                // THE HANG PULLS DOWN TOO (2026-09-22): the row held the ankle in the floor's plane
                // only, its height left to the floor's rows — and a foot that lifts off behind a
                // rising knee had nothing to bring it down: on a half-size character the shin
                // FOLDED up along the thigh as the knee rose (the ankle 47cm up, 30cm behind, the
                // knee at its 155-degree limit), the x/z target met as well from above as from
                // the floor. The hang's target lies on the floor while the knee is lower than a
                // shin's length, straight under it once it is not (kneeHangOffset), so its pull
                // down is the leg's gravity, and safe: it never asks the ankle under the floor.
                // Given at the hang's share of the merged row — none while the planted spring
                // holds (the ankle's height is the ball's, and a tucked foot's ankle stands above
                // its healed spot), all of it once the spring is gone. IK_JS_KNEE_HANG_FLAT is the A/B.
                if (!kKneeHangFlat) {
                    place.axisScale.y = hang / (spring + hang);
                }
            }
            place.weight = spring + hang;
            kneeHangMerged = true;
            if (kKneeTrace) {
                const glm::vec3 ankleNow(m_poseGlobal[static_cast<std::size_t>(kneeAnkle)][3]);
                std::fprintf(stderr, "[knee]   foot: plant %.3f spring %.3g hang %.3g place(%.3f %.3f %.3f) ankle(%.3f %.3f %.3f) under(%.3f %.3f %.3f)", plant, spring, hang,
                             place.target.x, place.target.y, place.target.z, ankleNow.x, ankleNow.y, ankleNow.z, under.x, under.y, under.z);
                std::fputc(10, stderr);
            }
        }
        // (The sole and the toes stay LEVEL through the trail, going with the spring: a foot
        // that comes off the floor flat and lands flat. Let go of with the rest, the foot rode
        // its trailing shin toes-down, the toes dug into the floor's rows, and the foot broke
        // free of them — and, coming back down, caught on them — in one tick: 10cm at the toes.)
        // (... and KEEPS A HOLD at the end of the trail (2026-09-22): faded to exactly zero with
        // the spring, the foot was free the tick the trail completed, and the ankle's posture
        // pulled it back to the kneel's tucked value — the male rig's half kneel landed flat
        // and flipped onto its heel in the next tick, toes 18cm up (a 164mm pop). A share of
        // the hold stays: 400/rad^2 on the toes, enough to keep a hanging foot level and land it
        // flat, nothing beside the floor's rows.)
        static const double kTrailLevel = envOr("IK_JS_KNEE_TRAIL_LEVEL", kKneeTrailLevelShare);
        const double level = static_cast<double>(1.0f - trail) + kTrailLevel * static_cast<double>(trail);
        for (std::size_t i = kneeFootRows[2]; i < kneeFootRows[3]; ++i) {
            problem.orientations[i].weight *= level;
            problem.orientations[i].axisWeights *= level;
        }
    }
    // A dragged elbow's hand tends to stay where it is (when nothing already holds it).
    if (elbowDrag && elbowHeldPin < 0) {
        static const double kHandHold = envOr("IK_JS_ELBOWHOLD", kElbowHandHoldWeight);
        const float     forearm = chainLength(elbowWrist, effector);
        const glm::vec3 place = m_jsStartPos[static_cast<std::size_t>(elbowWrist)];
        const float excess = std::abs(glm::length(elbowGoal - place) - forearm);
        const float bend = effectorFoldAxis >= 0
                               ? std::abs(m_ikStartEuler[static_cast<std::size_t>(effector)][effectorFoldAxis])
                               : 0.0f;
        const double weight =
            kHandHold * static_cast<double>(glm::smoothstep(kElbowHoldBendFrom, kElbowHoldBendFull, bend)) *
            static_cast<double>(1.0f - glm::smoothstep(kElbowHoldFrom * legScale, kElbowHoldGone * legScale, excess));
        if (weight > 0.0) {
            JointPositionTask stay;
            stay.bone = elbowWrist;
            stay.target = glm::dvec3(place);
            stay.weight = weight;
            problem.positions.push_back(stay);
        }
    }

    // ... and a leg that hangs from the dragged knee hangs its ankle under it (a row of its own
    // when there is no place to merge it with: a leg that was already in the air).
    if (kneeDrag && !kneeHangMerged && plant < 1.0f && kneeAnkle >= 0 &&
        static_cast<std::size_t>(kneeAnkle) < m_jsStartPos.size()) {
        JointPositionTask hang;
        hang.bone = kneeAnkle;
        hang.target = glm::dvec3(ikKneeHangUnder(s));
        if (s.lyingKneeFoot) {
            hang.target.y = std::max(hang.target.y, static_cast<double>(lyingHangY)); // (see the merged row)
        }
        hang.weight = kKneeHangW * static_cast<double>(1.0f - plant);
        hang.axisScale = kKneeHangFlat ? glm::dvec3(1.0, 0.0, 1.0) : glm::dvec3(1.0, 1.0, 1.0);
        problem.positions.push_back(hang);
    }
}

} // namespace pose

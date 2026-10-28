/**
 * @file armatureiksolverows.cpp
 * @brief The solve's ROWS (see armatureiksolve.cpp for the map): the pins — user pins, the foot
 *        contact model, live contacts, landed hands, a dragged knee's letting-go foot — the
 *        cursor and its company, the balance row, and the floor's and the body volumes'
 *        one-sided plane rows. What the solver satisfies.
 *
 * Every stage is a member function over the tick's IkSolveScratch (armatureiksolvestate.h) and
 * begins by naming the members it reads and writes. Qt-free (std + GLM).
 */

#include "armature.h"
#include "armatureiksolvestate.h"
#include "armatureiksolvetuning.h"

#include "balancecontroller.h"
#include "ikmath.h"
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
/// @p target pulled back along the geodesic toward @p now so that it leads the hand's rotation now by at
/// most kPalmLeadDeg (IK_JS_PALMLEAD; 0 = uncapped): the palm eases' lead cap, see kPalmLeadDeg.
glm::mat3 capPalmLead(const glm::mat3& now, const glm::mat3& target) {
    static const float kLead = envOr("IK_JS_PALMLEAD", kPalmLeadDeg);
    if (kLead <= 0.0f) {
        return target;
    }
    const glm::quat nowQ = glm::normalize(glm::quat_cast(now));
    glm::quat       to = glm::normalize(glm::quat_cast(target));
    if (glm::dot(nowQ, to) < 0.0f) {
        to = -to;
    }
    const float lead = glm::degrees(2.0f * std::acos(glm::clamp(glm::dot(nowQ, to), -1.0f, 1.0f)));
    if (lead <= kLead) {
        return target;
    }
    return glm::mat3_cast(glm::slerp(nowQ, to, kLead / lead));
}
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
    const glm::vec3 lateral(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
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

/// The PIN ROWS, part one: the per-drag state the pins' rows share (the eased pin targets, the palm and fingertip rows, the ceilings, the rise positions).
// A hand contact's TAUT share (kHandTautReachBand): 0 while its arm is slack by both measures — the
// limb's own stretch (IkRig::liveContactStretch, 1 = a straight arm) and the junction's reach over the
// chain's span — 1 once the arm is dead straight by its stretch, or past kHandSlackShare by the reach.
// A continuous function of the pose: the hold a hand keeps under a rise fades as the arm straightens,
// and comes back if the hand's rise folds it. (IK_JS_HAND_UNLOAD_SWITCH: the switch it replaced.)
static float handTautShare(float stretch, float reachSpan) {
    static const bool kSwitch = std::getenv("IK_JS_HAND_UNLOAD_SWITCH") != nullptr; // A/B probe
    if (kSwitch) {
        return stretch < kHandSlackShare && reachSpan < kHandSlackShare ? 0.0f : 1.0f;
    }
    const float byStretch = glm::smoothstep(kHandSlackShare, 1.0f, stretch);
    const float byReach = glm::smoothstep(kHandSlackShare - kHandTautReachBand, kHandSlackShare, reachSpan);
    return std::max(byStretch, byReach);
}

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
    std::vector<char>& pinned = s.pinned;
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
    bool& pinsEasing = s.pinsEasing;
    std::array<std::size_t, 4>& kneeFootRows = s.kneeFootRows; // its position rows [0,1) and orientation rows [2,3)
    s.kneePlaceRow = static_cast<std::size_t>(-1); // ... and its ankle's place among them
    std::size_t& kneePlaceRow = s.kneePlaceRow;
    s.kneeHangMerged = false;
    bool& kneeHangMerged = s.kneeHangMerged;
    s.lyingKneeFoot = false;
    s.lyingFootPitchExcessDeg = 0.0f;
    s.flatEasing = false;
    if (m_jsKneeHeldDown.size() == n) {
        m_jsKneeHeldDownPrev = m_jsKneeHeldDown;
        std::fill(m_jsKneeHeldDown.begin(), m_jsKneeHeldDown.end(), 0);
    }
    s.handTipHeld.assign(n, 0); // (hands whose fingertip hold stands this tick: see the live contacts)
    std::vector<char>& handTipHeld = s.handTipHeld;
    std::vector<JointOrientationTask>& palmRows = s.palmRows; // (... and their palms' pulls, added once the contact's fade is known)
    std::vector<JointPositionTask>& tipRows = s.tipRows; // (... and their fingertips' heights)
    s.footFreed.assign(pins.size(), 0); // (see THE FEET COME UP: their floor rows return)
    std::vector<char>& footFreed = s.footFreed;
    s.footLying.assign(pins.size(), 0); // (A KNEELING FOOT LIES FLAT: the ankle's and mid-foot's floor rows off)
    std::vector<std::tuple<int, float, float>>& handCeilings = s.handCeilings; // (a slack-armed hand, a loaded knee may not rise off the floor: node, height, weight share — a one-sided row, see the plane source)
    std::vector<std::tuple<std::size_t, int, float>>& kneeCeilings = s.kneeCeilings; // (a loaded knee may not rise either: pin, node, height — added to the ceilings below by its hold)
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

/// The PIN ROWS of ONE pin: a user pin's hold, a standing foot's contact model (ball, heel, sole, toes), a live contact's fading unilateral hold, a landed hand's palm and fingertip, the knee-drag foot's release.
void Armature::ikPinRows(IkSolveScratch& s, std::size_t p) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    const std::vector<IkEffector>& pins = s.pins;
    const float& legScale = s.legScale;
    int& kneeFootPin = s.kneeFootPin;
    float& kneeLift = s.kneeLift;
    JointSolver::Problem& problem = s.problem;
    std::vector<char>& inSpine = s.inSpine;
    std::vector<char>& pinned = s.pinned;
    bool& pinsEasing = s.pinsEasing;
    std::array<std::size_t, 4>& kneeFootRows = s.kneeFootRows;
    std::size_t& kneePlaceRow = s.kneePlaceRow;
    std::vector<char>& handTipHeld = s.handTipHeld;
    std::vector<JointOrientationTask>& palmRows = s.palmRows;
    std::vector<JointPositionTask>& tipRows = s.tipRows;
    std::vector<char>& footFreed = s.footFreed;
    std::vector<std::tuple<int, float, float>>& handCeilings = s.handCeilings;
    std::vector<std::tuple<std::size_t, int, float>>& kneeCeilings = s.kneeCeilings;
    std::vector<glm::vec3>& risePositions = s.risePositions;

    // The arm of a hand contact the rise is UNLOADING is referred to where the unload finds it (the
    // fade block): EVERY TICK the unload is in progress, the channels from the hand up to the limb's
    // junction — in this tick's dofs, and through m_jsPostStepTurned for the rest of the drag. Once
    // per contact it stored strain instead: over the three ticks the hold faded the shoulder pulled
    // the arm dead straight and twisted 23 degrees against references fixed at the first of them,
    // and the tick the pin went the arm sprang back by that much — 131mm at a fingertip. An arm
    // whose hand is letting go has no pose of its own to spring to: it is where the body left it,
    // and the hang takes it from there. (IK_JS_HAND_UNLOAD_SWITCH keeps the drag-start references.)
    const auto rebaseContactArm = [&](std::size_t hand) {
        static const bool kSwitch = std::getenv("IK_JS_HAND_UNLOAD_SWITCH") != nullptr; // A/B probe
        if (kSwitch || hand >= m_jsContactRebased.size()) {
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
                for (JointSolverDof& dof : problem.dofs) {
                    if (dof.bone == bone && dof.axis < 3) { // (the root's translation keeps its home)
                        dof.reference = m_boneEuler[b][dof.axis];
                    }
                }
            };
            for (std::size_t k = 1; k < m_jsSpineChain.size(); ++k) {
                rebase(m_jsSpineChain[k]);
            }
            rebase(root);
        }
    };

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
    JointPositionTask task;
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
    const std::size_t heelTask = problem.positions.size(); // (its target is finished below)
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
    static const bool kKneelFlatTrace = std::getenv("IK_JS_KNEEL_FLAT_TRACE") != nullptr;
    float flat = 0.0f;      // this foot's flat share (m_jsFootFlat, eased)
    float flip = 0.0f;      // how far its lying model has turned back to the standing one (the rise's progress, or the knee's lift)
    bool  flatFoot = false; // its model is the lying one (it is, or was this drag, flat)
    bool  unrolling = false; // let go of by its knee, rolling back onto its toes in place: the ankle rides the shin meanwhile
    bool  toLying = false;   // its knee holds: the foot is lying, or rolling onto its top
    // (The STANDING ball offset from the ankle: the bind's, turned to the foot's heading as the drag
    // found it — its lateral axis's, which a pitch does not move.)
    // The HEADING the drag found the foot at: how far its LATERAL axis — the foot's own, across the
    // ankle-to-ball line at bind, which a pitch about it does not move — has turned about the
    // vertical. (Read off the WORLD's lateral axis until 2026-09-30: a foot stands toed out at
    // bind, the world's x is not its pitch axis, and pitched half a turn — lying on its top behind
    // a kneel — that axis read a heading up to twice the toe-out off. It went unseen while the
    // bones' frames were themselves composed wrongly, the foot's pitch axis standing nearer the
    // world's: with the frames right, a foot rolled back onto its toes was re-seated to stand 35
    // degrees toed out, its heel 7cm inside its ball, and the leg flipped basins to get there —
    // 14cm at a thigh in one tick. IK_JS_FOOT_HEADING_WORLD_X is the A/B.)
    const auto foundFootYaw = [&](int ballBone, float& yaw) {
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
    };
    const auto standingBallOffset = [&](int ballBone) {
        glm::vec3 standing = m_ikBindPos[static_cast<std::size_t>(ballBone)] - m_ikBindPos[node];
        float     yaw = 0.0f;
        if (foundFootYaw(ballBone, yaw)) {
            standing = glm::mat3_cast(glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f))) * standing;
        }
        return standing;
    };
    float rollProgress = 1.0f; // how far the roll onto its top has gone (1: lying, or never rolled)
    bool  lyingKneeFoot = false; // the DRAGGED KNEE's foot, found lying: it rides its shin (below)
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
            std::vector<int> footBelow;
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
            std::vector<int> footBelow;
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
                    const glm::vec3 standing = standingBallOffset(ball);
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
                // fingertip on the third generation. IK_JS_UNROLL_BALL_FREE is the A/B.)
                // (... held DOWN too, behind IK_JS_UNROLL_BALL_DOWN: it measured worse than the plane-only
                // spring on all three rigs it was tried on — the knee's ceiling, below, is what keeps the
                // foot from flying.)
                static const bool kBallDown = std::getenv("IK_JS_UNROLL_BALL_DOWN") != nullptr; // A/B probe
                const float ballFloorY = pin.target.y + standingBallOffset(ball).y;
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
                static const float kBallLift = static_cast<float>(envOr("IK_JS_UNROLL_BALL_LIFT", static_cast<double>(kKneelUnrollBallLift)));
                const float liftY = ballFloorY + kBallLift * rig.sizeScale() * (node < m_jsFootUnrollProgress.size() ? m_jsFootUnrollProgress[node] : 0.0f);
                const float ballY = kBallLift > 0.0f ? std::max(liftY, ballNow.y)
                                    : kBallDown ? std::max(ballFloorY, ballNow.y - kKneelFlatLandStep * rig.sizeScale()) : ballNow.y;
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
                    std::vector<int> footBelow;
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
    // A knee COMING DOWN over its planted foot (the rig's HARD live pin: a kneel) is held hard in
    // its HEIGHT — that is what brings it onto the floor — and only SOFTLY to its place
    // (kKneeLandingHold: enough that it does not wander, a knee left free drifted 12cm inward
    // on 25 degrees of thigh twist) until it has landed, and is planted where it lands
    // (dragIkTick re-seats it then). It was nailed, in full, to the spot it
    // HOVERED over when it crossed the 5cm band: brought 4.5cm straight down there, a tucked
    // foot already at its ankle's limits cannot follow, the knee's pin and the ball's — two
    // 1e8 rows — pulled the leg apart with a force of 47,000 against the cursor's 1,000, and
    // the hips were thrown 20cm down and back in one tick to relieve it, where they stayed.
    static const bool kKneeLandingHeld = std::getenv("IK_JS_KNEE_LANDING_HELD") != nullptr; // A/B probe
    if (!kKneeLandingHeld && rig.pinIsLive(p) && pin.hard && !rig.pinUnderEffector(p)) {
        const float above = m_poseGlobal[node][3].y - pin.target.y;
        const float landed = 1.0f - glm::smoothstep(0.003f * rig.sizeScale(), 0.012f * rig.sizeScale(), above);
        static const double kLandingHold = envOr("IK_JS_KNEELANDHOLD", kKneeLandingHold);
        // (The miss's scale: the square root of the weight's. A friction-like hold once landed —
        // a place hold that gives at the pelvis drag's full pull, so hips pushed lower than rigid
        // thighs allow would slide the knees — was tried and did nothing: the tilt was never
        // about reach or sliding, and a landed knee is nailed.)
        const double place = std::sqrt(glm::mix(kLandingHold, weight, static_cast<double>(landed)) / weight);
        task.axisScale = glm::dvec3(place, 1.0, place);
        if (landed < 1.0f && node < m_jsContactLanding.size()) {
            m_jsContactLanding[node] = 1;
        }
    }
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
                const glm::vec3 lateralNow(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
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
                const glm::vec3 f = fingers / fingersReach;
                glm::vec3       palm = glm::vec3(0.0f, -1.0f, 0.0f) - f * glm::dot(glm::vec3(0.0f, -1.0f, 0.0f), f);
                if (glm::length(palm) > 0.2f) {
                    palm = glm::normalize(palm);
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
                        const glm::quat from = glm::normalize(glm::quat_cast(m_jsPalmFrom[hnode]));
                        glm::quat       to = glm::normalize(glm::quat_cast(lieRot));
                        if (glm::dot(from, to) < 0.0f) {
                            to = -to;
                        }
                        glm::quat now = glm::normalize(glm::quat_cast(glm::mat3(m_poseGlobal[hnode])));
                        if (glm::dot(now, to) < 0.0f) {
                            now = -now;
                        }
                        const float whole = glm::degrees(2.0f * std::acos(glm::clamp(glm::dot(from, to), -1.0f, 1.0f)));
                        const float left = glm::degrees(2.0f * std::acos(glm::clamp(glm::dot(now, to), -1.0f, 1.0f)));
                        const float reached = whole > 1.0e-3f ? glm::clamp(1.0f - left / whole, 0.0f, 1.0f) : 1.0f;
                        m_jsPalmEase[hnode] = whole > kPalmTurnStepDeg
                                                  ? std::min(1.0f, std::max(m_jsPalmEase[hnode], reached) + kPalmTurnStepDeg / whole)
                                                  : 1.0f;
                        targetRot = glm::mat3_cast(glm::slerp(from, to, m_jsPalmEase[hnode]));
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
                    const glm::dvec3 fore(h);
                    flat.frame = glm::dmat3(glm::cross(up, fore), up, fore);
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
                    static const bool kKneeTrace = std::getenv("IK_JS_KNEE_CEILING_TRACE") != nullptr;
                    if (kKneeTrace) {
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
                        kneeCeilings.emplace_back(p, pin.node, pin.target.y);
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
            if (m_jsWristOf.size() == n && m_jsWristOf[node] >= 0 && node < m_jsHandUnload.size()) {
                // (EASED IN TIME, kHandUnloadStep a tick — a hand's hold fades over ten ticks however
                // fast its arm crosses the taut band: at a drag's pace the band is two ticks, and the
                // trunk the hand had been holding down sprang 122mm at the brow in the one tick the
                // hold went. IK_JS_HAND_UNLOAD_SWITCH: unlimited, as before.)
                static const bool kSwitch = std::getenv("IK_JS_HAND_UNLOAD_SWITCH") != nullptr; // A/B probe
                if (!kSwitch) {
                    const float prev = m_jsHandUnload[node];
                    static const float kStep = envOr("IK_JS_HANDUNLOADSTEP", kHandUnloadStep); // (A/B lever)
                    unloadRise = glm::clamp(unloadRise, prev - kStep, prev + kStep);
                }
                m_jsHandUnload[node] = unloadRise;
            }
            unloadRise *= kneeHoldRelease; // (a held knee's unload comes in with the release: the knee-hold block)
            if (!kNoRiseUnload && (!kneeBelowHips || kneeHoldRelease > 1.0e-4f) && unloadRise > 1.0e-4f) {
                gone = std::max(gone, unloadRise);
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
                    m_jsContactRiseUnloaded[node] = unloadRise >= kRiseUnloadedOutrightShare ? 2
                                                    : unloadRise >= kRiseUnloadedShare       ? 1
                                                                                             : 3;
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
            contactGone = gone;
            static const bool kFadeTrace = std::getenv("IK_JS_CONTACT_FADE_TRACE") != nullptr; // which contact is letting go, and why
            if (kFadeTrace && gone > 1.0e-3f) {
                std::fprintf(stderr, "[fade] node=%zu hard=%d rise=%.4f gone=%.3f rootRise=%.2f contactRise=%.2f kneelUp=%.2f armSlack=%d kneeBelowHips=%d y=%.4f target.y=%.4f", node, pin.hard ? 1 : 0, rise, gone, m_jsRootRise, m_jsContactRise, m_jsKneelUpRise, armSlack ? 1 : 0, kneeBelowHips ? 1 : 0, m_poseGlobal[node][3].y, pin.target.y);
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
            for (const auto& [kp, knode, kheight] : kneeCeilings) {
                if (kp == p && gone < 0.999f) {
                    handCeilings.emplace_back(knode, kheight, static_cast<float>(std::pow(10.0, -6.0 * static_cast<double>(gone))));
                }
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
            problem.positions.push_back(tipDown);
        }
        tipRows.clear();
        for (JointOrientationTask& flat : palmRows) {
            flat.axisWeights *= static_cast<double>(1.0f - contactGone);
            problem.orientations.push_back(flat);
        }
        palmRows.clear();
    }
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
    double footFree = 0.0;
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
    if (ball >= 0) {
        // The ball bears, hard, carried with the pin target (a step swings the whole foot)
        // and approached at the pins' pace from where the ball is (see m_jsBallTarget).
        {
            glm::vec3 ballGoal = pin.target + m_jsBallOffset[node];
            if (flatFoot) {
                // (The lying ball, and — once the foot has rolled back onto its toes — the standing
                // model's: the ball where it lay, at its own floor height, the re-seated ankle pin a
                // heel's length behind it.)
                const glm::vec3 standing = standingBallOffset(ball);
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
            static const bool kBallDown2 = std::getenv("IK_JS_UNROLL_BALL_DOWN") != nullptr; // A/B probe
            bear.weight = kKneelFootHold;
            static const bool kBallLifted = envOr("IK_JS_UNROLL_BALL_LIFT", static_cast<double>(kKneelUnrollBallLift)) > 0.0; // (... and UP, off the floor: see the tracking above)
            bear.axisScale = (kBallDown2 || kBallLifted) ? glm::dvec3(1.0, 1.0, 1.0) : glm::dvec3(1.0, 0.0, 1.0);
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
        const glm::dvec3 upAxis(0.0, 1.0, 0.0);
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
                    glm::vec3 lateralNow = target * lateralLocal;
                    lateralNow.y = 0.0f;
                    if (glm::length(lateralNow) > 0.2f) {
                        lateralNow = glm::normalize(lateralNow);
                        const float yaw = std::atan2(glm::cross(lateralLocal, lateralNow).y,
                                                     glm::dot(lateralLocal, lateralNow));
                        const glm::quat flat = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                        target = glm::mat3_cast(glm::slerp(glm::quat_cast(target), flat, soleTurn));
                    }
                }
                hold.target = glm::dmat3(target);
                // (lateral, up made square to it, fore): right-handed like (x, y, z).
                const glm::dvec3 lateral = glm::normalize(glm::dvec3(target * lateralLocal));
                glm::dvec3 upSquare = upAxis - lateral * glm::dot(lateral, upAxis);
                upSquare = glm::length(upSquare) > 1.0e-6 ? glm::normalize(upSquare) : glm::dvec3(0.0, 0.0, 1.0);
                static const double kRoll = envOr("IK_JS_SOLEROLL", kSoleRollWeight);
                hold.perAxis = true;
                hold.frame = glm::dmat3(lateral, upSquare, glm::cross(lateral, upSquare));
                hold.axisWeights = glm::dvec3(kSolePitchWeight, weight * kPinOrientScale * kPinOrientScale, kRoll);
                static const double kKneeHeading = envOr("IK_JS_KNEEHEADING", kKneeFootHeadingWeight);
                if (kneeFoot) {
                    hold.axisWeights[1] = kKneeHeading;
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
                    glm::vec3 lateralNow = toeTarget * lateralLocal;
                    lateralNow.y = 0.0f;
                    if (glm::length(lateralNow) > 0.2f) {
                        lateralNow = glm::normalize(lateralNow);
                        const float yaw = std::atan2(glm::cross(lateralLocal, lateralNow).y,
                                                     glm::dot(lateralLocal, lateralNow));
                        const glm::quat flat = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                        toeTarget = glm::mat3_cast(glm::slerp(glm::quat_cast(toeTarget), flat, flatten));
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
                    static const double kKneeHeading = envOr("IK_JS_KNEEHEADING", kKneeFootHeadingWeight);
                    const glm::dvec3 lateral = glm::normalize(glm::dvec3(toeTarget * lateralLocal));
                    glm::dvec3 upSquare = upAxis - lateral * glm::dot(lateral, upAxis);
                    upSquare = glm::length(upSquare) > 1.0e-6 ? glm::normalize(upSquare) : glm::dvec3(0.0, 0.0, 1.0);
                    toes.perAxis = true;
                    toes.frame = glm::dmat3(lateral, upSquare, glm::cross(lateral, upSquare));
                    toes.axisWeights = glm::dvec3(kToeHoldWeight, kKneeHeading, kToeHoldWeight);
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
        return;
    }
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
                const glm::dvec3 up(0.0, 1.0, 0.0);
                const glm::dvec3 lateral = glm::normalize(hold.target * glm::dvec3(1.0, 0.0, 0.0));
                glm::dvec3 upSquare = up - lateral * glm::dot(lateral, up);
                upSquare = glm::length(upSquare) > 1.0e-6 ? glm::normalize(upSquare) : glm::dvec3(0.0, 0.0, 1.0);
                hold.perAxis = true;
                hold.frame = glm::dmat3(lateral, upSquare, glm::cross(lateral, upSquare));
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
        std::vector<int> foot;
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
            static const double kHang = envOr("IK_JS_KNEEHANG", kKneeAnkleHangWeight);
            JointPositionTask& place = problem.positions[kneePlaceRow];
            const double spring = place.weight;
            double hang = kHang * static_cast<double>(1.0f - plant);
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
                static const bool kHangFlat = std::getenv("IK_JS_KNEE_HANG_FLAT") != nullptr; // A/B probe
                if (!kHangFlat) {
                    place.axisScale.y = hang / (spring + hang);
                }
            }
            place.weight = spring + hang;
            kneeHangMerged = true;
            static const bool kKneeTrace3 = std::getenv("IK_JS_KNEE_TRACE") != nullptr;
            if (kKneeTrace3) {
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
        float forearm = 0.0f;
        for (int cur = elbowWrist; cur >= 0 && cur != effector; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            forearm += glm::length(glm::vec3(m_bones[static_cast<std::size_t>(cur)].localBind[3]));
        }
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
        static const double kHang = envOr("IK_JS_KNEEHANG", kKneeAnkleHangWeight);
        JointPositionTask hang;
        hang.bone = kneeAnkle;
        hang.target = glm::dvec3(ikKneeHangUnder(s));
        if (s.lyingKneeFoot) {
            hang.target.y = std::max(hang.target.y, static_cast<double>(lyingHangY)); // (see the merged row)
        }
        hang.weight = kHang * static_cast<double>(1.0f - plant);
        static const bool kHangFlat2 = std::getenv("IK_JS_KNEE_HANG_FLAT") != nullptr; // A/B probe
        hang.axisScale = kHangFlat2 ? glm::dvec3(1.0, 0.0, 1.0) : glm::dvec3(1.0, 1.0, 1.0);
        problem.positions.push_back(hang);
    }
}

/// The CURSOR's row and its company: the hips drawn under a rising trunk, the sway's level chest, a slid foot's sole, a hand slid along the floor, a suspended body's hang.
void Armature::ikCursorRows(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    const glm::vec3* const dragTarget = s.dragTarget;
    const glm::vec3* const holdTarget = s.holdTarget;
    const float& floorModel = s.floorModel;
    const float& rising = s.rising;
    const float& risen = s.risen;
    int& swayChest = s.swayChest;
    const float& legScale = s.legScale;
    const float& footFloor = s.footFloor;
    const float& slide = s.slide;
    bool& kneeDrag = s.kneeDrag;
    glm::vec3& kneeGoal = s.kneeGoal;
    bool& elbowDrag = s.elbowDrag;
    glm::vec3& elbowGoal = s.elbowGoal;
    JointSolver::Problem& problem = s.problem;
    std::vector<char>& pinned = s.pinned;

    // RISING by the TRUNK (the chest, the head pulled up out of a low pose): the HIPS are drawn
    // under the grabbed joint — where they sit under it in a standing body, turned to the figure's
    // heading; from where they were when the drag began, by the rise — and balance stands down as
    // they are, exactly as it does under a pelvis drag: the user is taking the BODY somewhere.
    // Left to infer the hips from the cursor, the contacts and balance, the solve had no good
    // answer: the knees' support went in one tick and balance hauled the hips back faster than the
    // chest moved (the spine crushed between them, twisting); kept as supports they let the hips
    // drift 15cm forward under an arched back; and whichever way the support was handled, some
    // generation stood up bowed, short of the cursor, or not at all. Dragged by the HIP the same
    // figure had always got up cleanly on every rig — so that is what a rising trunk drag now is.
    static const bool kNoUnder = std::getenv("IK_JS_NO_HIPS_UNDER") != nullptr; // A/B probe
    s.trunkRise = !kNoUnder && rising > 0.0f && effector != root && rig.effectorIsTrunk() &&
                  dragTarget != nullptr;
    const bool& trunkRise = s.trunkRise;
    if (trunkRise) {
        const glm::vec3 upright =
            glm::mat3_cast(glm::angleAxis(m_jsRootHeading, glm::vec3(0.0f, 1.0f, 0.0f))) *
            (m_ikBindPos[static_cast<std::size_t>(effector)] - m_ikBindPos[static_cast<std::size_t>(root)]);
        JointPositionTask under;
        under.bone = root;
        under.target = glm::dvec3(*dragTarget - glm::mix(m_jsStartGrabFromRoot, upright, risen));
        under.weight = kEffectorWeight;
        under.huber = kEffectorPull * static_cast<double>(rising) / (2.0 * kEffectorWeight);
        problem.positions.push_back(under);
    }

    // The cursor (a drag tick) or the released joint held where it was let go (the settle).
    s.goal = dragTarget != nullptr ? dragTarget : holdTarget;
    const glm::vec3*& goal = s.goal;
    s.goalTask = problem.positions.size();
    std::size_t& goalTask = s.goalTask;
    if (goal != nullptr && !pinned[static_cast<std::size_t>(effector)]) {
        JointPositionTask task;
        task.bone = effector;
        task.target = glm::dvec3(kneeDrag ? kneeGoal : elbowDrag ? elbowGoal : *goal);
        m_jsSolveGoal = glm::vec3(task.target);
        task.weight = kEffectorWeight;
        // A PELVIS drag pulls harder: the user is placing the body itself, and what stops the
        // hip is the legs running out (the hard ball pins), not a posture preference.
        static const double kPull = envOr("IK_JS_PULL", kEffectorPull);
        static const double kRootPull = envOr("IK_JS_ROOTPULL", kRootDragPullScale);
        // (A trunk joint's drag keeps the hand's pull. The pelvis's fivefold was tried for it: a
        // crouching figure's chest reached no further once its real blockers were gone — the
        // pelvis's prices under a hinge, the spine's S-curve — a slouched head raised 15cm went
        // 6cm up onto her toes, and with a foot pinned a far chest drag turned the planted one
        // 10-12 degrees.)
        task.huber = kPull * (effector == root ? kRootPull : 1.0) / (2.0 * kEffectorWeight);
        problem.positions.push_back(task);
    } else {
        goalTask = static_cast<std::size_t>(-1);
    }
    // THE GROUP's secondaries (2026-10-27): one row each on its own target — where it stood at the
    // press, carried by the cursor's displacement (Armature::updateGroupTargets) — at the cursor
    // row's weight and bounded pull: with every grabbed joint on its target the pose is the one
    // the posture model finds for them together, and a target out of reach draws its limb only
    // until the posture's gradient matches the pull, as the primary's does.
    for (std::size_t i = 0; i < s.secondaries.size(); ++i) {
        const int sec = s.secondaries[i];
        if (goal == nullptr || sec < 0 || static_cast<std::size_t>(sec) >= n || i >= m_jsGroupTargetValid.size() ||
            !m_jsGroupTargetValid[i] || pinned[static_cast<std::size_t>(sec)]) {
            continue;
        }
        static const double kGroupPull = envOr("IK_JS_PULL", kEffectorPull);
        static const double kGroupRootPull = envOr("IK_JS_ROOTPULL", kRootDragPullScale);
        JointPositionTask task;
        task.bone = sec;
        task.target = glm::dvec3(m_jsGroupTargets[i]);
        task.weight = kEffectorWeight;
        task.huber = kGroupPull * (sec == root ? kGroupRootPull : 1.0) / (2.0 * kEffectorWeight);
        problem.positions.push_back(task);
    }

    // The hip sway's CHEST: level (its roll about the fore axis), as the drag found it.
    if (swayChest >= 0 && static_cast<std::size_t>(swayChest) < m_jsStartRot.size()) {
        static const double kChestLevel = envOr("IK_JS_SWAYCHEST", kSwayChestLevelWeight);
        const glm::dvec3 lateral(std::cos(m_jsRootHeading), 0.0, -std::sin(m_jsRootHeading));
        const glm::dvec3 up(0.0, 1.0, 0.0);
        JointOrientationTask level;
        level.bone = swayChest;
        level.target = glm::dmat3(m_jsStartRot[static_cast<std::size_t>(swayChest)]);
        level.perAxis = true;
        level.frame = glm::dmat3(lateral, up, glm::cross(lateral, up));
        level.axisWeights = glm::dvec3(0.0, 0.0, kChestLevel);
        problem.orientations.push_back(level);
    }

    // A sliding foot's SOLE: held as the drag found it (a foot that began in the air comes down
    // FLAT, its heading kept), softly, by the slide — so the ankle gives as the leg swings out
    // over it. Riding its shin rigidly, a foot slid 40cm sideways stood on its inner edge, and one
    // taken 45cm back stopped 3cm short of the floor: its toes, pointing down the leg's line,
    // reached the floor first.
    if (slide > 0.0f && goalTask != static_cast<std::size_t>(-1)) {
        const std::size_t e = static_cast<std::size_t>(effector);
        // The foot's fore axis at bind: toward the middle of its subtree's leaves (the toe tips).
        glm::vec3 tips(0.0f);
        int tipCount = 0;
        std::vector<int> stack(m_children[e].begin(), m_children[e].end());
        while (!stack.empty()) {
            const int b = stack.back();
            stack.pop_back();
            if (m_children[static_cast<std::size_t>(b)].empty()) {
                tips += m_ikBindPos[static_cast<std::size_t>(b)];
                ++tipCount;
            }
            stack.insert(stack.end(), m_children[static_cast<std::size_t>(b)].begin(),
                         m_children[static_cast<std::size_t>(b)].end());
        }
        glm::vec3 bindFore = tipCount > 0 ? tips / static_cast<float>(tipCount) - m_ikBindPos[e] : glm::vec3(0.0f);
        bindFore.y = 0.0f;
        if (glm::length(bindFore) > 1.0e-4f) {
            const glm::vec3 lateralLocal = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), bindFore));
            glm::mat3 target = m_jsEffectorStartRot;
            const float slideAtStart =
                1.0f - glm::smoothstep(0.03f * legScale, 0.12f * legScale, m_jsEffectorStartPos.y - footFloor);
            glm::vec3 lateralNow = target * lateralLocal;
            lateralNow.y = 0.0f;
            if (slideAtStart < 1.0f && glm::length(lateralNow) > 0.2f) {
                lateralNow = glm::normalize(lateralNow);
                const float yaw = std::atan2(glm::cross(lateralLocal, lateralNow).y, glm::dot(lateralLocal, lateralNow));
                const glm::quat flat = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
                target = glm::mat3_cast(glm::slerp(flat, glm::quat_cast(target), slideAtStart));
            }
            // THE SOLE TURNS FLAT AS THE FOOT COMES DOWN (2026-09-25; IK_JS_NO_SOLE_EASE): the
            // hold's target is eased from the rotation the slide FOUND the foot at — the tick the
            // target came within the slide's band — toward its flat target at kSoleTurnStepDeg a
            // tick, and the ease only ever LEADS (a foot already further along is met there). A
            // foot lifted and carried on a taut leg rides its shin, 50 degrees from flat (once the
            // weight shift took the hips over the standing foot, a foot carried 35cm sideways
            // stood 57cm outside its socket); the hold, at a fiftieth of its weight already
            // twenty times the ankle's posture term, flipped it flat in two ticks: 85mm at a toe,
            // on four of the newest generation's characters and the reference rig alike.
            static const bool kNoSoleEase = std::getenv("IK_JS_NO_SOLE_EASE") != nullptr; // A/B probe
            if (!kNoSoleEase) {
                if (!m_jsSoleEaseValid) {
                    m_jsSoleEaseFrom = glm::mat3(m_poseGlobal[e]);
                    m_jsSoleEase = 0.0f;
                    m_jsSoleEaseValid = true;
                }
                if (m_jsSoleEase < 1.0f) {
                    const glm::quat from = glm::normalize(glm::quat_cast(m_jsSoleEaseFrom));
                    glm::quat       to = glm::normalize(glm::quat_cast(target));
                    if (glm::dot(from, to) < 0.0f) {
                        to = -to;
                    }
                    glm::quat now = glm::normalize(glm::quat_cast(glm::mat3(m_poseGlobal[e])));
                    if (glm::dot(now, to) < 0.0f) {
                        now = -now;
                    }
                    const float whole = glm::degrees(2.0f * std::acos(glm::clamp(glm::dot(from, to), -1.0f, 1.0f)));
                    const float left = glm::degrees(2.0f * std::acos(glm::clamp(glm::dot(now, to), -1.0f, 1.0f)));
                    const float reached = whole > 1.0e-3f ? glm::clamp(1.0f - left / whole, 0.0f, 1.0f) : 1.0f;
                    m_jsSoleEase = whole > kSoleTurnStepDeg
                                       ? std::min(1.0f, std::max(m_jsSoleEase, reached) + kSoleTurnStepDeg / whole)
                                       : 1.0f;
                    if (m_jsSoleEase < 1.0f) {
                        target = glm::mat3_cast(glm::slerp(from, to, m_jsSoleEase));
                        m_jsPinsEasing = true; // (the settle waits for the sole to arrive, as for a pin)
                    }
                }
            }
            const glm::dvec3 upAxis(0.0, 1.0, 0.0);
            const glm::dvec3 lateral = glm::normalize(glm::dvec3(target * lateralLocal));
            glm::dvec3 upSquare = upAxis - lateral * glm::dot(lateral, upAxis);
            upSquare = glm::length(upSquare) > 1.0e-6 ? glm::normalize(upSquare) : glm::dvec3(0.0, 0.0, 1.0);
            static const double kPitch = envOr("IK_JS_SLIDEPITCH", kSlideSolePitchWeight);
            static const double kHeading = envOr("IK_JS_SLIDEHEADING", kSlideSoleHeadingWeight);
            static const double kLevel = envOr("IK_JS_SLIDEROLL", kSlideSoleRollWeight);
            JointOrientationTask sole;
            sole.bone = effector;
            sole.target = glm::dmat3(target);
            sole.perAxis = true;
            sole.frame = glm::dmat3(lateral, upSquare, glm::cross(lateral, upSquare));
            sole.axisWeights = glm::dvec3(kPitch, kHeading, kLevel) * static_cast<double>(slide);
            problem.orientations.push_back(sole);
        }
    }

    // A HAND SLID ALONG THE FLOOR (2026-09-23; IK_JS_NO_HAND_SLIDE): a dragged WRIST whose
    // target lies at the hand's floor height is a hand being placed on the floor — pulled out
    // from under a prone body, walked ahead of the shoulders for a push-up — and it lies on
    // its PALM as it goes, fingers along the way it is dragged. Nothing said so: the cursor
    // pulled the wrist and the arm found its cheapest way there, which from a hand folded
    // under the chest was as often the ELBOW swung out sideways with the forearm pivoting
    // (a chicken wing: the hand ahead, the elbow out on the floor, the wrist at its limits)
    // as the arm extending — two basins a 1e-7 rounding chose between, and from the folded
    // one the push-up that followed left the chest down, 23mm off its cursor. The landed
    // hand's palm-flat frame (the bind's fingers line and palm normal — see the live-contact
    // block), headed along the drag, held at the foot slide's weights by the same share of
    // the target's height: soft beside the cursor's pull, decades over the wrist's posture.
    static const bool kNoHandSlide = std::getenv("IK_JS_NO_HAND_SLIDE") != nullptr; // A/B probe
    if (!kNoHandSlide && dragTarget != nullptr && effector != root && m_jsWristOf.size() == n &&
        m_jsWristOf[static_cast<std::size_t>(effector)] == effector && m_jsHandTip[static_cast<std::size_t>(effector)] >= 0) {
        const std::size_t hnode = static_cast<std::size_t>(effector);
        const int         tip = m_jsHandTip[hnode];
        const float       handFloor = floorModel + rig.floorClearance(effector);
        const float       handSlide = 1.0f - glm::smoothstep(0.03f * legScale, 0.12f * legScale, dragTarget->y - handFloor);
        const glm::vec3   fingers = m_ikBindPos[static_cast<std::size_t>(tip)] - m_ikBindPos[hnode];
        glm::vec3         travel = *dragTarget - m_jsEffectorStartPos;
        travel.y = 0.0f;
        if (handSlide > 1.0e-3f && glm::length(fingers) > 1.0e-4f) {
            // (A hand being PLACED on the floor is landing, and its arm is out of the fold retry as
            // a landed hand's is (JointSolverDof::foldRetry). Kicked, a straight arm reaching the
            // floor from a standing fold flipped its hand between the palm frame's basins every
            // tick of the hold: 133mm at a thumb tip on a heavy character, the grab oscillating
            // 1.8mm; 38 and 0.03 without the kick.)
            if (handSlide > 0.5f) {
                const int junction = rig.limbJunction(effector);
                for (int cur = effector; cur >= 0 && cur != junction && cur != root; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                    for (JointSolverDof& dof : problem.dofs) {
                        if (dof.bone == cur) {
                            dof.foldRetry = false;
                        }
                    }
                }
            }
            const glm::vec3 f = fingers / glm::length(fingers);
            const glm::vec3 down(0.0f, -1.0f, 0.0f);
            glm::vec3       palm = down - f * glm::dot(down, f);
            // (The heading: the drag's own way along the floor once it has gone a few
            // centimetres, the figure's forward until then.)
            const glm::vec3 forward = glm::normalize(glm::vec3(std::sin(m_jsRootHeading), 0.0f, std::cos(m_jsRootHeading)));
            const float     went = glm::length(travel);
            const glm::vec3 h = went > 0.03f * legScale ? glm::normalize(travel) : forward;
            if (glm::length(palm) > 0.2f) {
                palm = glm::normalize(palm);
                const glm::mat3 local(f, palm, glm::cross(f, palm));
                const glm::mat3 world(h, down, glm::cross(h, down));
                glm::mat3 lieRot = world * glm::transpose(local);
                // (Eased in from the rotation the drag found the hand at, kPalmTurnStepDeg a
                // tick, as a landed hand's palm roll is: asked flat at once, a hand folded under
                // the chest swung its forearm 149mm in the drag's second tick.)
                {
                    const glm::quat from = glm::normalize(glm::quat_cast(m_jsEffectorStartRot));
                    glm::quat       to = glm::normalize(glm::quat_cast(lieRot));
                    if (glm::dot(from, to) < 0.0f) {
                        to = -to;
                    }
                    const float whole = glm::degrees(2.0f * std::acos(glm::clamp(glm::dot(from, to), -1.0f, 1.0f)));
                    m_jsHandSlideEase = whole > kPalmTurnStepDeg ? std::min(1.0f, m_jsHandSlideEase + kPalmTurnStepDeg / whole) : 1.0f;
                    lieRot = glm::mat3_cast(glm::slerp(from, to, m_jsHandSlideEase));
                }
                lieRot = capPalmLead(glm::mat3(m_poseGlobal[hnode]), lieRot); // (never more than kPalmLeadDeg ahead of the hand)
                const glm::dvec3 up(0.0, 1.0, 0.0);
                const glm::dvec3 fore(h);
                static const double kPitch = envOr("IK_JS_SLIDEPITCH", kSlideSolePitchWeight);
                static const double kHeading = envOr("IK_JS_SLIDEHEADING", kSlideSoleHeadingWeight);
                static const double kLevel = envOr("IK_JS_SLIDEROLL", kSlideSoleRollWeight);
                JointOrientationTask flat;
                flat.bone = effector;
                flat.target = glm::dmat3(lieRot);
                flat.perAxis = true;
                flat.frame = glm::dmat3(glm::cross(up, fore), up, fore);
                flat.axisWeights = glm::dvec3(kPitch, kHeading, kLevel) * static_cast<double>(handSlide);
                problem.orientations.push_back(flat);
            }
        }
    }

    // A HAND LAID ON THE BODY (2026-09-25; IK_JS_NO_HAND_LAY): a dragged WRIST whose target comes to
    // rest against a BODY VOLUME — the hip, the chest, the head, a knee, the other forearm — is a
    // hand being laid on the body, and it lies on its PALM there as a hand on the floor does.
    // Nothing said so: the wrist rode the forearm at the rotation the drag found it, so a hand
    // brought to the hip stood edge-on beside it, a hand to the chin lay across the face palm
    // down, and the everyday hand-on-hip and hand-on-knee poses needed a manual wrist turn after
    // every drag (the round-96 audit's shots). The surface is the nearest capsule to the TARGET
    // among every volume the rig built, the hand's own arm excepted — the legs included, though a
    // hand is never kept OUT of them (it rests on knees and thighs: the palm turns to the knee the
    // hand is put on) — read as its outward normal at the target's closest point; the lay comes in
    // as the target's clearance of the surface closes on the wrist's own (kHandLayNear/Far over
    // IkRig::volumeClearance: the drag clamp puts a hand pressed to the hip exactly there), and the
    // floor's own share stands it down (a hand laid on the floor is the floor's, above). The palm
    // faces INTO the surface and the fingers keep the heading they have across it — the least turn
    // from where the hand is, and the heading row is soft, so the arm chooses — eased from the
    // rotation the hand had when its target came within the band, kPalmTurnStepDeg a tick, the
    // ease only ever leading. WHICH SIDE lies on the surface — the palm, or the BACK of the hand —
    // is whichever needs the least turn where the target enters the band, kept while it stays
    // (IK_JS_HAND_LAY_PALM_ONLY is the A/B): palm-in everywhere, a hand brought to the small of the
    // back was laid palm-to-sacrum by a wrist flexed to its -90 limit — a forearm cannot supinate
    // that far, and a hand behind the back rests on its back — while a hand brought to the hip,
    // the chest, the head or a knee arrives palm-first and lies on its palm. Its arm STAYS in the
    // fold retry (a hand sliding on the floor takes its arm out of it): taken out, a hand lowered
    // beside the body after a lift — the arm dead straight, the straight limb's saddle — could not
    // unlock its elbow for a cursor 13cm away (the suspension-lower phase: the hand 133mm off).
    static const bool kNoHandLay = std::getenv("IK_JS_NO_HAND_LAY") != nullptr; // A/B probe
    static const bool kHandLayTrace = std::getenv("IK_JS_HAND_LAY_TRACE") != nullptr;
    bool              handLaid = false;
    if (!kNoHandLay && dragTarget != nullptr && effector != root && m_jsWristOf.size() == n &&
        m_jsWristOf[static_cast<std::size_t>(effector)] == effector && m_jsHandTip[static_cast<std::size_t>(effector)] >= 0) {
        const std::size_t hnode = static_cast<std::size_t>(effector);
        const int         tip = m_jsHandTip[hnode];
        const float       scale = rig.sizeScale();
        const glm::vec3   T = *dragTarget;
        // (The floor's own share — the hand slide above.)
        const float handFloor = floorModel + rig.floorClearance(effector);
        const float floorShare = 1.0f - glm::smoothstep(0.03f * legScale, 0.12f * legScale, T.y - handFloor);
        // The hand's own arm has no surface to offer it.
        std::vector<char> ownArm(n, 0);
        {
            const int junction = rig.limbJunction(effector);
            for (int cur = effector; cur >= 0 && cur != junction && cur != root; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                ownArm[static_cast<std::size_t>(cur)] = 1;
            }
        }
        // THE SURFACE IS THE BLEND OF EVERY VOLUME IN REACH (IK_JS_HAND_LAY_NEAREST is the nearest alone):
        // each volume within the band weighs in with its own share, and the lie's normal is their
        // share-weighted sum. A hand at the hip sits between the pelvis band's capsule and the thigh's,
        // about equally near, and read off the NEAREST alone the lie flipped between the two surfaces'
        // normals — 43 degrees apart — tick to tick as the nearest changed, and the arm with it (61-63mm
        // in a tick on the second generation, at rest). Blended, the normal is a continuous function
        // of the target, as everything about the lay must be.
        static const bool  kNearestOnly = std::getenv("IK_JS_HAND_LAY_NEAREST") != nullptr; // A/B probe
        static const float kNear = envOr("IK_JS_HANDLAYNEAR", kHandLayNear);
        static const float kFar = envOr("IK_JS_HANDLAYFAR", kHandLayFar);
        const float clearance = rig.volumeClearance(effector);
        const auto  pos = [&](int i) { return glm::vec3(m_poseGlobal[static_cast<std::size_t>(i)][3]); };
        float     bestGap = 1.0e9f;
        float     bestShare = 0.0f;
        glm::vec3 bestNormal(0.0f);
        glm::vec3 blendNormal(0.0f);
        int       bestA = -1;
        int       bestB = -1;
        for (const BodyVolume& v : rig.bodyVolumes()) {
            if (v.a < 0 || v.b < 0 || v.a >= static_cast<int>(n) || v.b >= static_cast<int>(n) ||
                ownArm[static_cast<std::size_t>(v.a)] || ownArm[static_cast<std::size_t>(v.b)]) {
                continue;
            }
            const VolumeAxis ax = volumeAxis(v, static_cast<int>(n), pos);
            if (!ax.ok) {
                continue;
            }
            const float     t = ax.ab2 > 1.0e-12f ? glm::clamp(glm::dot(T - ax.A, ax.ab) / ax.ab2, 0.0f, 1.0f) : 0.0f;
            const glm::vec3 away = T - (ax.A + ax.ab * t);
            const float     d = glm::length(away);
            if (d < 1.0e-4f) {
                continue;
            }
            float gap = d - volumeRadiusAt(v, t);
            // ... or the FOREARM's touch (IK_JS_HAND_LAY_WRIST_ONLY): the drag clamp keeps the forearm
            // segment's flesh out of the torso, and where the forearm comes to the body first the wrist
            // is held off it by the forearm's own thickness — on the newest generation a hand brought to
            // the hip rests 9cm off the pelvis capsule, its forearm against the hip, and read at the wrist
            // alone it never lay (the palm 53 degrees off). The segment's gap is read as if it were the
            // wrist's: its flesh touching counts as the wrist at its clearance.
            static const bool kWristOnly = std::getenv("IK_JS_HAND_LAY_WRIST_ONLY") != nullptr; // A/B probe
            const int   hpar = m_bones[hnode].parent;
            const float segFlesh = kVolumeSegmentFlesh * rig.volumeSegmentRadius(effector);
            if (!kWristOnly && hpar >= 0 && segFlesh > 0.0f) {
                float sPar = 0.0f;
                float tPar = 0.0f;
                const float dSeg = closestSegmentPoints(pos(hpar), T, ax.A, ax.B, sPar, tPar);
                if (sPar > 0.05f && sPar < 0.95f) {
                    gap = std::min(gap, dSeg - volumeRadiusAt(v, tPar) - segFlesh + clearance);
                }
            }
            // ... or the RAW CURSOR's (the target before the drag clamp pushed it out of the volumes):
            // a cursor inside the body is a hand on it, however far the clamp holds the wrist off —
            // a hand that arrives fingers-first is held a hand's length out by the riders' clearance
            // (the newest generation's hand on the hip rested 9cm off, its fingers pointing at the hip,
            // and read at the wrist it never lay). A function of the target, like the wrist's gap. (The
            // FINGERTIPS' own touch was tried first, the tip carried rigidly from where the hand stands:
            // it fed back — the palm turning moved the tip, the tip's gap swung centimetres a tick, and
            // the oldest generation's hand on the chest fell into a growing limit cycle in the hold.)
            if (!kWristOnly && m_jsRawTargetValid) {
                const glm::vec3& R = m_jsRawTarget;
                const float      tRaw = ax.ab2 > 1.0e-12f ? glm::clamp(glm::dot(R - ax.A, ax.ab) / ax.ab2, 0.0f, 1.0f) : 0.0f;
                const float      dRaw = glm::length(R - (ax.A + ax.ab * tRaw));
                gap = std::min(gap, dRaw - volumeRadiusAt(v, tRaw) + clearance);
            }
            const float share = 1.0f - glm::smoothstep(clearance + kNear * scale, clearance + kFar * scale, std::max(gap, 0.0f));
            blendNormal += (away / d) * share;
            if (gap < bestGap) {
                bestGap = gap;
                bestShare = share;
                bestNormal = away / d;
                bestA = v.a;
                bestB = v.b;
            }
        }
        // (The nearest volume's share is the lay's: the blend only turns the normal. Two surfaces facing
        // each other — a hand between the thighs — cancel, and the nearest alone stands in.)
        if (!kNearestOnly && glm::length(blendNormal) > 0.3f) {
            bestNormal = glm::normalize(blendNormal);
        }
        float lay = bestA >= 0 ? bestShare : 0.0f;
        lay *= 1.0f - floorShare;
        // ... and only as the target SLOWS (kHandLaySpeedFrom/To): a hand lies on what it rests against,
        // not on what it passes. Engaged at any speed a hand dragged past the body was turned onto each
        // surface it brushed, and where the forearm's twist ran out mid-pass the arm flipped basins.
        static const bool  kAnySpeed = std::getenv("IK_JS_HAND_LAY_ANY_SPEED") != nullptr; // A/B probe
        static const float kSpeedFrom = envOr("IK_JS_HANDLAYSPEEDFROM", kHandLaySpeedFrom);
        static const float kSpeedTo = envOr("IK_JS_HANDLAYSPEEDTO", kHandLaySpeedTo);
        const float speed = m_jsPrevGoalValid ? glm::length(T - m_jsPrevGoal) : 0.0f;
        const float still = kAnySpeed ? 1.0f : 1.0f - glm::smoothstep(kSpeedFrom * scale, kSpeedTo * scale, speed);
        lay *= still;
        const glm::vec3 fingers = m_ikBindPos[static_cast<std::size_t>(tip)] - m_ikBindPos[hnode];
        if (kHandLayTrace && lay <= 1.0e-3f && bestA >= 0) {
            std::fprintf(stderr, "[js-lay] %s near no surface: nearest %s-%s, gap %.1fmm (clearance %.1f, band to %.1f); target (%.3f %.3f %.3f)\n",
                         m_boneNames[hnode].c_str(), m_boneNames[static_cast<std::size_t>(bestA)].c_str(),
                         m_boneNames[static_cast<std::size_t>(bestB)].c_str(), bestGap * 1000.0f, clearance * 1000.0f,
                         (clearance + kFar * scale) * 1000.0f, T.x, T.y, T.z);
        }
        if (lay > 1.0e-3f && glm::length(fingers) > 1.0e-4f) {
            handLaid = true;
            const glm::vec3 f = fingers / glm::length(fingers);
            const glm::vec3 down(0.0f, -1.0f, 0.0f);
            glm::vec3       palm = down - f * glm::dot(down, f);
            if (glm::length(palm) > 0.2f) {
                palm = glm::normalize(palm);
                const glm::mat3 now(m_poseGlobal[hnode]);
                const glm::vec3 nOut = bestNormal;
                // The fingers' heading ACROSS the surface: where they point now, projected onto it
                // (pointing straight into or out of it, the palm's own direction across instead; a
                // hand that is degenerate both ways takes any tangent).
                glm::vec3 h = now * f;
                h -= nOut * glm::dot(h, nOut);
                if (glm::length(h) < 0.2f) {
                    h = now * palm;
                    h -= nOut * glm::dot(h, nOut);
                }
                if (glm::length(h) < 0.2f) {
                    h = glm::cross(nOut, glm::vec3(0.0f, 1.0f, 0.0f));
                    if (glm::length(h) < 0.2f) {
                        h = glm::cross(nOut, glm::vec3(1.0f, 0.0f, 0.0f));
                    }
                }
                h = glm::normalize(h);
                const glm::mat3 local(f, palm, glm::cross(f, palm));
                static const bool kPalmOnly = std::getenv("IK_JS_HAND_LAY_PALM_ONLY") != nullptr; // A/B probe
                // (While the lay is under kHandLayEngage the start rotation and the side are re-read every
                // tick: the rows are all but weightless there, and an ease that ran on from a stale start
                // stood at the full lie by the time they came in — see the tuning note.)
                if (!m_jsHandLayValid || lay < kHandLayEngage) {
                    m_jsHandLayValid = lay >= kHandLayEngage;
                    m_jsHandLayFrom = now;
                    m_jsHandLayEase = 0.0f;
                    // THE SIDE, by the forearm's TWIST ROOM (kHandLayBackMarginDeg): the elbow is the
                    // first fold joint up from the wrist, the forearm's axis runs from it to the wrist,
                    // and each side's lie asks a rotation of the hand whose part about that axis is
                    // pronation the twist channels between the wrist and the elbow must have room for.
                    static const double kBackMargin = envOr("IK_JS_HANDLAYBACKMARGIN", kHandLayBackMarginDeg);
                    int elbow = -1;
                    {
                        const int junction = rig.limbJunction(effector);
                        for (int cur = m_bones[hnode].parent; cur >= 0 && cur != root && cur != junction;
                             cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                            const Bone& bone = m_bones[static_cast<std::size_t>(cur)];
                            bool        folds = false;
                            for (int a = 0; a < 3 && !folds; ++a) {
                                const float range = bone.rotMax[a] - bone.rotMin[a];
                                folds = bone.rotLimited[a] && range >= kFoldRangeDeg && bone.rotMin[a] < 0.0f && bone.rotMax[a] > 0.0f &&
                                        std::min(-bone.rotMin[a], bone.rotMax[a]) <= kFoldNarrowFraction * std::max(-bone.rotMin[a], bone.rotMax[a]);
                            }
                            if (folds) {
                                elbow = cur;
                                break;
                            }
                        }
                    }
                    const int       hpar = m_bones[hnode].parent;
                    const glm::vec3 wristAt(m_poseGlobal[hnode][3]);
                    const glm::vec3 elbowAt(m_poseGlobal[static_cast<std::size_t>(elbow >= 0 ? elbow : (hpar >= 0 ? hpar : effector))][3]);
                    glm::vec3       forearm = wristAt - elbowAt;
                    float           need[2] = {0.0f, 0.0f};
                    float           room[2] = {0.0f, 0.0f};
                    float           deficit[2] = {0.0f, 0.0f};
                    if (glm::length(forearm) > 1.0e-4f) {
                        forearm = glm::normalize(forearm);
                        for (int side = 0; side < 2; ++side) {
                            const glm::vec3 ln = -nOut * (side == 0 ? 1.0f : -1.0f);
                            const glm::mat3 worldSide(h, ln, glm::cross(h, ln));
                            const glm::mat3 lieSide = worldSide * glm::transpose(local);
                            glm::quat       dq = glm::normalize(glm::quat_cast(lieSide * glm::transpose(now)));
                            if (dq.w < 0.0f) {
                                dq = -dq;
                            }
                            const float     ang = 2.0f * std::acos(glm::clamp(dq.w, -1.0f, 1.0f));
                            const glm::vec3 dv(dq.x, dq.y, dq.z);
                            const glm::vec3 ax = glm::length(dv) > 1.0e-6f ? glm::normalize(dv) : glm::vec3(0.0f);
                            const float     twist = glm::degrees(ang * glm::dot(ax, forearm)); // signed, about the forearm
                            float           have = 0.0f;
                            for (int cur = hpar; cur >= 0; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                                const std::size_t cb = static_cast<std::size_t>(cur);
                                const int         a = cb < m_jsTwistAxis.size() ? m_jsTwistAxis[cb] : -1;
                                if (a >= 0) {
                                    const Bone& bone = m_bones[cb];
                                    // (the channel's axis as it stands: the rotation-order prefix applied)
                                    glm::mat3 prefix(1.0f);
                                    for (const char ch : bone.rotationOrder) {
                                        const int axis = ch == 'X' ? 0 : ch == 'Y' ? 1 : ch == 'Z' ? 2 : -1;
                                        if (axis < 0 || axis == a) {
                                            break;
                                        }
                                        glm::vec3 e(0.0f);
                                        e[axis] = 1.0f;
                                        prefix = prefix * glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(m_boneEuler[cb][axis]), e));
                                    }
                                    glm::vec3 e(0.0f);
                                    e[a] = 1.0f;
                                    const int       cpar = bone.parent;
                                    const glm::mat3 parentRot = cpar >= 0 ? glm::mat3(m_poseGlobal[static_cast<std::size_t>(cpar)]) : glm::mat3(1.0f);
                                    const glm::vec3 axisWorld = parentRot * glm::mat3(bone.orient) * prefix * e;
                                    const float     along = glm::dot(axisWorld, forearm);
                                    if (std::abs(along) > 0.5f) {
                                        const bool  positive = twist * along > 0.0f;
                                        const float at = m_boneEuler[cb][a];
                                        const float channelRoom = !bone.rotLimited[a] ? 360.0f : (positive ? bone.rotMax[a] - at : at - bone.rotMin[a]);
                                        have += std::max(channelRoom, 0.0f) * std::abs(along);
                                    }
                                }
                                if (cur == elbow) {
                                    break;
                                }
                            }
                            need[side] = twist;
                            room[side] = have;
                            deficit[side] = std::max(0.0f, std::abs(twist) - have);
                        }
                    }
                    // In front of the body the palm is preferred, behind it the back of the hand.
                    const glm::vec3 forward(std::sin(m_jsRootHeading), 0.0f, std::cos(m_jsRootHeading));
                    const bool      behind = glm::dot(T - glm::vec3(m_poseGlobal[static_cast<std::size_t>(root)][3]), forward) < -0.05f * scale;
                    const int       preferred = behind ? 1 : 0;
                    const int       other = 1 - preferred;
                    const int       chosen = (kPalmOnly || deficit[preferred] <= deficit[other] + static_cast<float>(kBackMargin)) ? preferred : other;
                    m_jsHandLaySide = chosen == 0 ? 1.0f : -1.0f;
                    if (kPalmOnly) {
                        m_jsHandLaySide = 1.0f;
                    }
                    if (kHandLayTrace) {
                        std::fprintf(stderr, "[js-lay] %s enters the band on %s-%s%s: palm needs %.0f deg of forearm twist (room %.0f), the back %.0f (room %.0f) -> %s\n",
                                     m_boneNames[hnode].c_str(), m_boneNames[static_cast<std::size_t>(bestA)].c_str(),
                                     m_boneNames[static_cast<std::size_t>(bestB)].c_str(), behind ? " (behind the body)" : "",
                                     need[0], room[0], need[1], room[1], m_jsHandLaySide > 0.0f ? "palm" : "back");
                    }
                }
                // (The back of the hand on the surface: the palm faces OUT along the normal, and the
                // fingers' line is mirrored across it so the frame stays right-handed.)
                const glm::vec3 lieNormal = -nOut * m_jsHandLaySide;
                const glm::mat3 world(h, lieNormal, glm::cross(h, lieNormal));
                glm::mat3       lieRot = world * glm::transpose(local);
                if (m_jsHandLayEase < 1.0f) {
                    const glm::quat from = glm::normalize(glm::quat_cast(m_jsHandLayFrom));
                    glm::quat       to = glm::normalize(glm::quat_cast(lieRot));
                    if (glm::dot(from, to) < 0.0f) {
                        to = -to;
                    }
                    glm::quat nowQ = glm::normalize(glm::quat_cast(now));
                    if (glm::dot(nowQ, to) < 0.0f) {
                        nowQ = -nowQ;
                    }
                    const float whole = glm::degrees(2.0f * std::acos(glm::clamp(glm::dot(from, to), -1.0f, 1.0f)));
                    const float left = glm::degrees(2.0f * std::acos(glm::clamp(glm::dot(nowQ, to), -1.0f, 1.0f)));
                    const float reached = whole > 1.0e-3f ? glm::clamp(1.0f - left / whole, 0.0f, 1.0f) : 1.0f;
                    static const float kLayStep = envOr("IK_JS_HANDLAYSTEP", kHandLayTurnStepDeg);
                    const float        step = kLayStep * lay; // (the ease advances WITH the lay)
                    m_jsHandLayEase = whole > step
                                          ? std::min(1.0f, std::max(m_jsHandLayEase, reached) + step / whole)
                                          : 1.0f;
                    lieRot = glm::mat3_cast(glm::slerp(from, to, m_jsHandLayEase));
                }
                lieRot = capPalmLead(now, lieRot); // (never more than kPalmLeadDeg ahead of the hand)
                static const double kPitch = envOr("IK_JS_HANDLAYPITCH", kHandLayPitchWeight);
                static const double kLevel = envOr("IK_JS_HANDLAYLEVEL", kHandLayLevelWeight);
                static const double kHeading = envOr("IK_JS_HANDLAYHEADING", kHandLayHeadingWeight);
                JointOrientationTask lie;
                lie.bone = effector;
                lie.target = glm::dmat3(lieRot);
                lie.perAxis = true;
                const glm::dvec3 up(nOut);
                const glm::dvec3 fore(h);
                lie.frame = glm::dmat3(glm::cross(up, fore), up, fore);
                lie.axisWeights = glm::dvec3(kPitch, kHeading, kLevel) * static_cast<double>(lay);
                problem.orientations.push_back(lie);
                if (kHandLayTrace) {
                    const glm::vec3 palmNow = now * palm;
                    std::fprintf(stderr, "[js-lay] %s on %s-%s (%s): gap %.1fmm speed %.1fmm/tick lay %.2f ease %.2f side-to-surface %.0f deg\n",
                                 m_boneNames[hnode].c_str(), m_boneNames[static_cast<std::size_t>(bestA)].c_str(),
                                 m_boneNames[static_cast<std::size_t>(bestB)].c_str(), m_jsHandLaySide > 0.0f ? "palm" : "back",
                                 bestGap * 1000.0f, speed * 1000.0f, lay, m_jsHandLayEase,
                                 glm::degrees(std::acos(glm::clamp(glm::dot(palmNow * m_jsHandLaySide, -nOut), -1.0f, 1.0f))));
                }
            }
        }
    }
    if (!handLaid) {
        m_jsHandLayValid = false; // (the hand has left the band: the next approach eases from where it then is)
    }

    // SUSPENDED (IkRig::suspended: a sustained beyond-reach lift released the feet): the root
    // HANGS the grab-to-root chain length below the cursor, so the body dangles under the
    // raised arm; without it the posture term simply floated the figure up with its arm at its
    // side. Lowered back, the feet meet the floor's rows and the rig lands them (takeLanded).
    if (rig.suspended() && dragTarget != nullptr && effector != root) {
        JointPositionTask hang;
        hang.bone = root;
        hang.target = glm::dvec3(*dragTarget) - glm::dvec3(0.0, rig.suspendHang(), 0.0) + glm::dvec3(m_jsHangDeficit);
        hang.weight = kSuspendHangWeight;
        problem.positions.push_back(hang);
    }
}

/// The BALANCE row: the support polygon (grown into, a stepping foot kept in it), the slack ramp, the hinge's and the follow's release, the seat that keeps no balance.
void Armature::ikBalanceRow(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    const float& rising = s.rising;
    float& pelvisBalance = s.pelvisBalance;
    float& hingeSlack = s.hingeSlack;
    const float& slide = s.slide;
    int& kneeFootPin = s.kneeFootPin;
    float& plant = s.plant;
    JointSolver::Problem& problem = s.problem;
    const bool& trunkDrag = s.trunkDrag;
    const bool& trunkRise = s.trunkRise;

    if (m_ikScope == IkScope::Chain) {
        return; // (a SCOPED drag keeps no balance: the chain alone moves, and the body it hangs from stays where it was)
    }

    // Balance — not under a pelvis drag (the user is driving the pelvis).
    static const double kBalance = envOr("IK_JS_BALANCE", kBalanceWeight);
    // The SUPPORT: the rig's (the planted contacts) — plus a SLIDING foot: a foot slid along the
    // floor bears weight, and the body's weight may come between the feet (it must, for a
    // stride). Its footprint joins the support BY THE SLIDE — drawn out from the middle of the
    // rig's support to where the foot is as the foot comes down — because a support that arrives
    // in one tick is a jump: a foot lifted, carried 35cm sideways and set down had the hips sink
    // 12cm on the way down (balance holding the weight over the standing foot while the leg
    // reached) and spring 6cm back up in the tick the foot joined the support.
    s.support = rig.supportHull();
    std::vector<glm::vec2>& support = s.support;
    bool footInSupport = false;
    // A foot in a STEP stays in it, where its swing has it (the rig's own polygon drops it: "the
    // foot in flight supports nothing"). A step is a fall caught by the foot that takes it, not a
    // balance on the other one: held over the standing foot alone by a hard row, the centre of
    // mass was hauled 10cm sideways in the twelve ticks of a swing — the spine twisted to its
    // limits to do it, the arms swinging 20cm a tick — and let go again at the landing.
    static const bool kNoSwingSupport = std::getenv("IK_JS_NO_SWING_SUPPORT") != nullptr; // A/B probe
    // (Not under a pelvis drag: its balance is the hinge's, fore and aft only, and the hinge is
    // being let go of as the walk begins.)
    if (!kNoSwingSupport && effector != root && rig.steppingPin() >= 0) {
        support = BalanceController::supportPolygon(rig.supportPoints(-1));
    }
    if (slide > 0.0f && !support.empty()) {
        glm::vec2 middle(0.0f);
        for (const glm::vec2& v : support) {
            middle += v;
        }
        middle /= static_cast<float>(support.size());
        std::vector<int> stack{effector};
        while (!stack.empty()) {
            const int b = stack.back();
            stack.pop_back();
            const glm::vec3 at(m_poseGlobal[static_cast<std::size_t>(b)][3]);
            support.push_back(glm::mix(middle, glm::vec2(at.x, at.z), slide));
            stack.insert(stack.end(), m_children[static_cast<std::size_t>(b)].begin(),
                         m_children[static_cast<std::size_t>(b)].end());
        }
        support = BalanceController::supportPolygon(std::move(support));
        footInSupport = true;
    }
    // ... and the foot below a dragged knee LEAVES it by the plant, the same way round: its
    // footprint drawn in to the middle of the others' as it lets go. (The balance base below is
    // measured without it from the start, as for a dragged foot: a leg lifted by its knee has
    // never taken the body's weight over the other one either.)
    std::vector<glm::vec2> othersHull;
    if (kneeFootPin >= 0) {
        std::vector<glm::vec2> others = rig.supportPoints(kneeFootPin);
        if (!others.empty()) {
            glm::vec2 middle(0.0f);
            for (const glm::vec2& v : others) {
                middle += v;
            }
            middle /= static_cast<float>(others.size());
            othersHull = BalanceController::supportPolygon(others);
            if (plant < 1.0f) {
                for (const glm::vec2& v : rig.pinFootprintPoints(static_cast<std::size_t>(kneeFootPin))) {
                    others.push_back(glm::mix(middle, v, plant));
                }
                support = BalanceController::supportPolygon(std::move(others));
                footInSupport = true;
            }
        }
    }
    // A FOLLOWED trunk drag is the user taking the body somewhere, as a pelvis drag is: what
    // answers hips that outrun their feet is a step, and until it lands the body is as far off
    // its support as the follow has taken it — let go of as SLACK (the lean's own share stays
    // balanced). Held to the support's edge instead, the hips waited there for every step, the
    // steps came 6cm long, and a chest dragged at walking pace trailed 18cm behind its cursor.
    const float followSlack = glm::length(m_jsTrunkFollow) * m_jsTrunkWalk;
    // THE SEAT (IkRig::seatPin: a user pin on the pelvis girdle) keeps no balance: what holds a
    // pinned pelvis up is whatever the user sat her on, and the row knows only the feet. With it
    // on, a seated figure — her weight a forearm's length behind her feet, as posed — could not
    // recline at all (the chest pulled 12cm back went 3: every centimetre made the "imbalance"
    // worse) and leaned forward only as far as kept it no worse.
    // (... and A PELVIS ON THE FLOOR — IkRig::floorSeat, a live contact of the girdle — is a seat too:
    // there is nowhere to fall from a sit on the floor, and reclining onto her back takes the
    // weight behind every support she has until the back itself lands. Balanced, she would not
    // recline at all: the chest stopped 49cm from its cursor.)
    static const bool kNoFloorSeat = std::getenv("IK_NO_FLOOR_SEAT") != nullptr; // A/B probe
    // (... and A FIGURE ON HER KNEES whose TRUNK is dragged keeps no balance either: a kneeling
    // body cannot step, and her trunk taken forward past her knees is going onto her hands —
    // which is what the knees are for. Balanced over knees and toes, the row's answer to a
    // chest taken forward from a kneel was to haul the hips back and UP: the knees, unilateral
    // contacts, lifted 7-8cm off the floor as the shins came up onto the toes, and when the
    // hands landed the body dropped back onto them — 104-144mm at the head in one tick.)
    static const bool kNoKneelBalanceOff = std::getenv("IK_JS_KNEEL_BALANCED") != nullptr; // A/B probe
    const bool seated = rig.seatPin() >= 0 || (!kNoFloorSeat && rig.floorSeat()) ||
                        (!kNoKneelBalanceOff && m_jsKneelStart && trunkDrag);
    if ((effector != root || pelvisBalance > 0.0f) && !seated && !support.empty() && kBalance > 0.0) {
        problem.masses = &rig.masses();
        problem.supportHull = &support;
        problem.balanceMargin = rig.balanceMargin();
        // (Balance is BLIND to the idle arms' hang — hangIdleArms: the centre of mass is shifted
        // back by what the hang has moved it, for this solve.)
        static const bool kComTrace = std::getenv("IK_ARM_HANG_TRACE") != nullptr;
        if (balanceIsBlind() || kComTrace) { // (... and to the head's righting)
            std::vector<glm::vec3> real(n);
            std::vector<int>       parentOf(n);
            for (std::size_t i = 0; i < n; ++i) {
                real[i] = glm::vec3(m_poseGlobal[i][3]);
                parentOf[i] = m_bones[i].parent;
            }
            const glm::vec3 hung = BalanceController::centerOfMass(real, parentOf, rig.masses());
            const glm::vec3 rides = BalanceController::centerOfMass(balancePositions(), parentOf, rig.masses());
            problem.comShift = glm::dvec2(rides.x - hung.x, rides.z - hung.z);
            static const bool kHangTrace = std::getenv("IK_ARM_HANG_TRACE") != nullptr;
            if (kHangTrace) {
                const glm::vec2 at(rides.x, rides.z);
                const glm::vec2 to = BalanceController::closestBalancedPoint(support, at, rig.balanceMargin());
                std::fprintf(stderr, "[hang] com hung(%.4f %.4f) rides(%.4f %.4f) shift(%.4f %.4f) need %.4f slack %.4f+%.4f",
                             hung.x, hung.z, rides.x, rides.z, problem.comShift.x, problem.comShift.y,
                             glm::length(to - at), m_jsBalanceSlack, followSlack);
                std::fputc(10, stderr);
            }
        }
        problem.balanceWeight = effector == root ? kBalance * static_cast<double>(pelvisBalance) : kBalance;
        if (effector == root) {
            // (Fore and aft only: a hip HINGE. Sideways is the sway's — on the generations that
            // stand with their feet together an 8cm sway takes the centre of mass off the
            // support, and a sideways lean against the level chest it holds trembled 8cm.)
            problem.balanceAxis = glm::dvec2(std::sin(m_jsRootHeading), std::cos(m_jsRootHeading));
        }
        // The ramp: a changed support polygon (keyed by its vertex sum) starts with the
        // centre of mass' current distance as free slack, taken back a centimetre a tick.
        // (Keyed by the RIG's polygon and whether the dragged foot stands in the support — not by
        // that foot's place, which changes every tick of a slide: a key that changed every tick
        // reset the slack every tick, and balance never engaged.)
        float key = static_cast<float>(rig.supportHull().size()) + (footInSupport ? 1000.0f : 0.0f);
        for (const glm::vec2& v : rig.supportHull()) {
            key += v.x * 1.37f + v.y * 0.73f;
        }
        // (A foot in a step never left the solve's support, and lands where its swing already
        // had it: neither end of a step is a change to ramp in.)
        const bool swinging = !kNoSwingSupport && effector != root && rig.steppingPin() >= 0;
        if ((swinging || m_jsWasSwinging) && m_jsHullKey >= 0.0f) {
            m_jsHullKey = key;
        }
        m_jsWasSwinging = swinging;
        // A support that GROWS — a step landing under a pelvis drag, a hand come down — grows over
        // at kSupportGrowStep a tick: the new polygon drawn out from the middle of the one it replaces. A
        // one-sided row has nothing to say once it is met, so a body that LEANED to keep its
        // balance over one foot (the hip hinge through a step) was given its whole lean back the
        // tick the other foot landed: hips taken 25cm back, the head went 9-10cm in that tick.
        // (Until the balance point became the true inset polygon's, see closestBalancedPoint, its
        // oblique pull left a need standing after the landing, and that — by accident — was what
        // had eased this.)
        static const bool kNoSupportGrow = std::getenv("IK_JS_NO_SUPPORT_GROW") != nullptr; // A/B probe
        if (key != m_jsHullKey && m_jsHullKey >= 0.0f) {
            bool grew = !kNoSupportGrow && support.size() >= 3 && !m_jsLastSupport.empty();
            for (std::size_t i = 0; i < m_jsLastSupport.size() && grew; ++i) {
                const glm::vec2& v = m_jsLastSupport[i];
                grew = glm::length(BalanceController::closestBalancedPoint(support, v, 0.0f) - v) < 0.003f * rig.sizeScale();
            }
            m_jsGrown = 0.0f;
            m_jsGrowFrom.clear();
            if (grew) {
                m_jsGrowFrom = m_jsLastSupport;
            }
        }
        m_jsLastSupport = support;
        if (!m_jsGrowFrom.empty()) {
            // (Each new vertex grows out from the NEAREST point of the polygon it joins, not from
            // its middle: drawn out from the middle, a hand landed 26cm ahead of the knees was
            // inside the old polygon for its first 22 ticks — the support did not move at all —
            // while the drag took the weight on forward, and the balance row's answer to a chest
            // held on its cursor was to swing the hips UP about it: the knees lifted 25cm off the
            // floor and she went onto all fours on her toes, hips at standing height.)
            static const bool kGrowFromMiddle = std::getenv("IK_JS_GROW_FROM_MIDDLE") != nullptr; // A/B probe
            glm::vec2 centre(0.0f);
            for (const glm::vec2& v : m_jsGrowFrom) {
                centre += v;
            }
            centre /= static_cast<float>(m_jsGrowFrom.size());
            m_jsGrown += kSupportGrowStep * rig.sizeScale();
            float                  farthest = 0.0f;
            std::vector<glm::vec2> points = m_jsGrowFrom;
            for (const glm::vec2& v : support) {
                const glm::vec2 from = kGrowFromMiddle ? centre : BalanceController::closestBalancedPoint(m_jsGrowFrom, v, 0.0f);
                const float     d = glm::length(v - from);
                farthest = std::max(farthest, d);
                points.push_back(d > m_jsGrown ? from + (v - from) * (m_jsGrown / d) : v);
            }
            if (m_jsGrown >= farthest) {
                m_jsGrowFrom.clear(); // grown
            } else {
                support = BalanceController::supportPolygon(std::move(points));
            }
        }
        if (key != m_jsHullKey) {
            m_jsHullKey = key;
            const std::vector<glm::vec3> positions = balancePositions(); // (blind to the arms' hang)
            std::vector<int> parents(n);
            for (std::size_t i = 0; i < n; ++i) {
                parents[i] = m_bones[i].parent;
            }
            glm::vec2 need(0.0f);
            BalanceController::balanceCorrection(positions, parents, rig.masses(), support,
                                                 rig.balanceMargin(), need);
            if (effector == root) {
                const glm::vec2 axis(std::sin(m_jsRootHeading), std::cos(m_jsRootHeading));
                need = axis * glm::dot(need, axis);
            }
            // (What the hinge has already let go of is slack in its own right: counted twice, a
            // support that changed gave the rest of the lean back at once.)
            m_jsBalanceSlack = std::max(m_jsBalanceSlack, glm::length(need) - hingeSlack - followSlack);
            if (m_jsBalanceBase < 0.0f) {
                // The drag's first solve: whatever imbalance the pose STARTS with is the user's
                // (the drag-start pose is the contract) and stays free for the whole drag — a
                // figure posed leaning must not straighten itself under a still cursor.
                // (Measured against the RIG's support, the dragged foot not in it: a foot lifted
                // off the floor has never taken the body's weight over the other one — the drag
                // begins with the weight between the feet, and that is the imbalance it keeps.)
                // (... unless THE WEIGHT SHIFT is on — the drag began on two planted feet, one of
                // which it can lift (m_jsWeightShift, beginIkDrag): then the weight DOES go over
                // the standing foot. The hips' home takes it most of the way, and the row polices
                // the rest — the lifted leg's own mass, which a home read at the press cannot
                // know: a knee held at its cursor 24cm outside its shifted socket carries the
                // weight 7cm back toward the lifted side, and forgiven it stayed there, 12cm from
                // the standing ankle. Measured against the support AS IT STOOD at the press, both
                // feet in it, the base is what it truly was: nothing.)
                static const bool kWeightForgiven = std::getenv("IK_JS_WEIGHT_FORGIVEN") != nullptr; // A/B probe: the old base
                const bool weightShifts = !kWeightForgiven && glm::dot(m_jsWeightShift, m_jsWeightShift) > 0.0f;
                glm::vec2 rigNeed(0.0f);
                BalanceController::balanceCorrection(positions, parents, rig.masses(),
                                                     weightShifts ? support : (othersHull.empty() ? rig.supportHull() : othersHull),
                                                     rig.balanceMargin(), rigNeed);
                m_jsBalanceBase = std::max(m_jsBalanceSlack, glm::length(rigNeed));
            }
        } else if (m_jsGrowFrom.empty()) {
            // (Not while the support is still growing under it: taken back at its own pace the
            // slack ran out before a landed foot's polygon had reached the weight, and the row
            // bit — a lean of 2 degrees a joint for two ticks, the head 7cm, given back as the
            // next step lifted off.)
            m_jsBalanceSlack = std::max(m_jsBalanceBase, m_jsBalanceSlack - kBalanceRampStep);
        }
        problem.balanceSlack = m_jsBalanceSlack + hingeSlack + followSlack;
        static const bool kHingeTrace = std::getenv("IK_JS_HINGE_TRACE") != nullptr;
        static const bool kHingeTraceAll = kHingeTrace && std::getenv("IK_JS_HINGE_TRACE")[0] == '2'; // (=2: every drag)
        if (kHingeTrace && (effector == root || kHingeTraceAll)) {
            std::vector<glm::vec3> at(n);
            std::vector<int> parentOf(n);
            for (std::size_t i = 0; i < n; ++i) {
                at[i] = glm::vec3(m_poseGlobal[i][3]);
                parentOf[i] = m_bones[i].parent;
            }
            glm::vec2 needNow(0.0f);
            BalanceController::balanceCorrection(at, parentOf, rig.masses(), support, rig.balanceMargin(), needNow);
            std::fprintf(stderr, "[hinge] gate %.3f slack %.4f base %.4f release %.4f steps %d stepping %d hull %zu grown %.3f need %.4f\n",
                         pelvisBalance, m_jsBalanceSlack, m_jsBalanceBase, hingeSlack, rig.stepsTaken(),
                         rig.steppingPin(), support.size(), m_jsGrowFrom.empty() ? -1.0f : m_jsGrown,
                         glm::length(needNow));
            {
                const glm::vec3 c = BalanceController::centerOfMass(at, parentOf, rig.masses());
                float z0 = 1e9f, z1 = -1e9f, x0 = 1e9f, x1 = -1e9f;
                for (const glm::vec2& v : support) {
                    x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
                    z0 = std::min(z0, v.y); z1 = std::max(z1, v.y);
                }
                std::fprintf(stderr, "[hinge]   com(%.3f %.3f) support x[%.3f %.3f] z[%.3f %.3f] margin %.3f need(%.3f %.3f)\n",
                             c.x, c.z, x0, x1, z0, z1, rig.balanceMargin(), needNow.x, needNow.y);
            }
        }
        if (trunkRise) {
            problem.balanceWeight *= static_cast<double>((1.0f - rising) * (1.0f - rising));
        }
    }
}

/// The FLOOR's and the BODY VOLUMES' one-sided rows: the exemptions, a letting-go foot's scale and allowance, the ceilings, and the plane source the solver regenerates at every linearization.
void Armature::ikPlaneRows(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const JointSolver& solver = s.solver;
    const std::vector<IkEffector>& pins = s.pins;
    const float& legScale = s.legScale;
    int& kneeFootPin = s.kneeFootPin;
    float& plant = s.plant;
    float& kneeLift = s.kneeLift;
    JointSolver::Problem& problem = s.problem;
    std::vector<char>& footFreed = s.footFreed;
    std::vector<std::tuple<int, float, float>>& handCeilings = s.handCeilings;

    // The FLOOR and the BODY VOLUMES: one-sided rows, regenerated from the current frames at
    // every linearization (the contact normal follows a joint around a capsule). A pin's own
    // subtree is exempt from the floor (a planted foot's toes are the heel lift's business),
    // and everything the rig exempts from a volume stays exempt (IkRig::dragVolumes: the
    // pairs masked at bind and at drag start, the fingers against the limbs).
    static const bool kNoPlanes = std::getenv("IK_JS_NO_PLANES") != nullptr; // A/B probe
    std::vector<char> floorExempt(n, 0);
    std::vector<float> floorScale(n, 1.0f); // a joint's floor rows at less than their weight: a foot LETTING GO (below)
    std::vector<float> floorAllow(n, 0.0f); // ... and forgiven the depth they were FOUND at (m): a row that comes in on a joint already through its surface asks only that it go no deeper
    for (std::size_t p = 0; p < pins.size(); ++p) {
        const IkEffector& pin = pins[p];
        if (rig.pinIsLive(p) && !pin.hard) {
            continue; // a unilateral contact: the floor row IS its vertical hold
        }
        if (rig.pinIsLive(p)) {
            // (A HARD live contact — a knee planted over its foot — holds its own joint and nothing
            // below it: the shin and the foot keep their floor rows. Its whole subtree was exempt,
            // and a foot let go of behind it — THE FEET COME UP — had no floor at all: the newest
            // generation's toes went 44mm through it as she lay down on her belly.)
            floorExempt[static_cast<std::size_t>(pin.node)] = 1;
            continue;
        }
        if (static_cast<int>(p) == kneeFootPin && plant < 1.0f) {
            // Letting go: a foot coming off the floor, and the floor is the floor — but the floor's
            // rows come IN with the release, at (1 - plant) of their weight, not all at once: a
            // kneeling foot stands on its tucked toe tips, which by their flesh's measure are 2-5mm
            // "through" the floor, and the moment its exemption dropped (the lift's first
            // centimetre) a dozen 1e7 rows landed on it at once — a cost of 800 from nowhere, and
            // the knee shoved 8cm sideways and back (75-135mm in a tick, on every rig, worst on
            // the three whose kneel lies flattest). IK_JS_KNEE_FLOOR_SWITCH restores the switch.
            static const bool kFloorSwitch = std::getenv("IK_JS_KNEE_FLOOR_SWITCH") != nullptr; // A/B probe
            if (!kFloorSwitch) {
                std::vector<int> footSubtree;
                collectSubtree(pin.node, footSubtree);
                // ... AND FORGIVEN THE DEPTH THEY ARE FOUND AT (2026-09-23; IK_JS_KNEE_FLOOR_NO_ALLOW):
                // the share brings the rows in gently, but a row's residual is the whole depth
                // from its first tick, and a foot whose tucked toe tips stand 15-18mm through the
                // floor by their flesh's measure (the third generation's; 2-5mm on the others)
                // was pushed out of it at a fiftieth of 1e7 already — the foot rolled and yawed to
                // its limits, the shin's side channel too, and the thigh abducted 15 degrees in
                // one tick: the knee shot 8cm out and back (84mm at the shin). A row that comes
                // in on a joint ALREADY through its surface asks only that it go no deeper —
                // the depth at the release's first tick is its allowance, a constant of the
                // drag, given back over the TRAIL (kKneeLiftFull..kKneeTrailFull of lift, by
                // when the foot is in the air and level).
                static const bool kNoAllow = std::getenv("IK_JS_KNEE_FLOOR_NO_ALLOW") != nullptr; // A/B probe
                if (!kNoAllow && !m_jsFootAllowValid) {
                    m_jsFootAllow.assign(n, 0.0f);
                    for (const int b : footSubtree) {
                        const float clearance = rig.floorClearance(b);
                        if (clearance <= 0.0f) {
                            continue;
                        }
                        const float depth = -m_transform[3][1] + clearance - m_poseGlobal[static_cast<std::size_t>(b)][3].y;
                        m_jsFootAllow[static_cast<std::size_t>(b)] = std::max(0.0f, depth);
                    }
                    m_jsFootAllowValid = 1;
                }
                const float trailNow = glm::smoothstep(kKneeLiftFull * legScale, kKneeTrailFull * legScale, kneeLift);
                for (const int b : footSubtree) {
                    floorScale[static_cast<std::size_t>(b)] = 1.0f - plant;
                    if (!kNoAllow && m_jsFootAllowValid && m_jsFootAllow.size() == n) {
                        floorAllow[static_cast<std::size_t>(b)] = m_jsFootAllow[static_cast<std::size_t>(b)] * (1.0f - trailNow);
                    }
                }
            }
            continue;
        }
        if (p < s.footLying.size() && s.footLying[p]) {
            // (A KNEELING FOOT LIES FLAT: the foot-class joints' clearances are their BIND heights —
            // the standing model's — and a lying ankle sits at the shin's radius, its mid-foot
            // joint beside it: their rows would hold the foot level 5cm off the floor. Exempt from
            // the ankle down to the ball's parent; the ball's and the toes' rows, a toe's thickness
            // over the floor, stay and stop the foot's top there.)
            const int ball = m_jsBall[static_cast<std::size_t>(pin.node)];
            for (int cur = ball >= 0 ? m_bones[static_cast<std::size_t>(ball)].parent : pin.node; cur >= 0; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                floorExempt[static_cast<std::size_t>(cur)] = 1;
                if (cur == pin.node) {
                    break;
                }
            }
            continue;
        }
        if (p < footFreed.size() && footFreed[p]) {
            continue; // (a foot behind a planted knee the hips have gone ahead of: its height rows
                      // are gone, and only the floor keeps it up — exempt, two characters' feet
                      // went 36-45mm through it as she lay down)
        }
        std::vector<int> subtree;
        collectSubtree(pin.node, subtree);
        for (const int b : subtree) {
            floorExempt[static_cast<std::size_t>(b)] = 1;
        }
        if (m_jsBall[static_cast<std::size_t>(pin.node)] >= 0) {
            floorExempt[static_cast<std::size_t>(pin.node)] = 0; // a soft heel must not sink
        }
    }
    // (An arm that HANGS — hangIdleArms — keeps itself out of the BODY with its own stop, and is
    // none of the solve's unknowns: a volume row on one of its joints could only be answered by
    // moving the body away from the arm. A chest pulled sideways on a narrow-stanced rig, its
    // hanging arm brushing the hip, threw the head 10cm in a tick. The FLOOR's rows stay: a body
    // that comes down takes its hanging hands to the floor, and they must stop it there — exempt,
    // the fingers went 4-7cm through it before the hand's contact formed.)
    std::vector<char> volumeExempt(n, 0);
    // (A hand planted on the floor under the body keeps the torso's rows on its riders: exempting
    // them — the torso lies on the hand, and the capsule cannot know the flesh gives — was tried
    // for the prone hold's hand twitch and dropped, a finger jumping 126mm mid-drag on the base
    // rig once the torso could lie through the hand.)
    for (const HangArm& arm : m_jsHangArms) {
        bool held = false;
        for (const IkEffector& pin : pins) {
            for (int cur = pin.node; cur >= 0 && !held; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                held = cur == arm.socket;
            }
        }
        if (!held) {
            std::vector<int> subtree;
            collectSubtree(arm.socket, subtree);
            for (const int b : subtree) {
                volumeExempt[static_cast<std::size_t>(b)] = 1;
                // (While THE SEAT ROLLS the hang keeps the arm off the floor itself — hangIdleArms —
                // and the floor's rows on it, which only the BODY can answer, stand down: a little
                // finger 16mm low, the tick the hang fell behind, shoved the whole figure 3cm up
                // the floor as she was laid down.)
                if (m_jsSeatRolls) {
                    floorExempt[static_cast<std::size_t>(b)] = 1;
                }
            }
        }
    }
    // (... and so, while the seat rolls, does the HEAD the righting has: its neck is none of the
    // solve's unknowns either, the righting turns it a tick AFTER the solve has answered for where
    // it was, and a skull that reaches the floor as she is laid down closed that loop — the floor's
    // row tipped the body, the tilt changed the righting, the righting moved the skull: a 2-tick
    // cycle under a still cursor, 8-10cm at the brow, on the generation whose head comes down.)
    static const bool kHeadRows = std::getenv("IK_JS_HEAD_FLOOR_ROWS") != nullptr; // A/B probe
    if (m_jsSeatRolls && !kHeadRows) {
        for (const int bone : m_jsHeadFree) {
            std::vector<int> subtree;
            collectSubtree(bone, subtree);
            for (const int b : subtree) {
                floorExempt[static_cast<std::size_t>(b)] = 1;
            }
        }
    }
    const float floorY = -m_transform[3][1];
    if (!kNoPlanes) {
        problem.planeSource = [this, &rig, &solver, floorExempt, floorScale, floorAllow, volumeExempt, floorY, root, n, handCeilings](
                                  const JointSolver::Frames& f, std::vector<JointPlaneTask>& out) {
            const int count = static_cast<int>(n);
            for (const auto& [node, height, share] : handCeilings) {
                JointPlaneTask ceiling;
                ceiling.bone = node;
                ceiling.planePoint = glm::dvec3(0.0, static_cast<double>(height), 0.0);
                ceiling.normal = glm::dvec3(0.0, -1.0, 0.0);
                ceiling.weight = kPlaneWeight * static_cast<double>(share);
                static const bool kCeilingTrace = std::getenv("IK_JS_CEILING_TRACE") != nullptr; // the hand / knee ceilings and where their joints stand
                if (kCeilingTrace) {
                    std::fprintf(stderr, "[js-ceiling] %s at %.4f share %.3f: joint y %.4f (%+.1fmm)", m_boneNames[static_cast<std::size_t>(node)].c_str(), height, share,
                                 f.pos[static_cast<std::size_t>(node)].y, (f.pos[static_cast<std::size_t>(node)].y - static_cast<double>(height)) * 1000.0);
                    std::fputc(10, stderr);
                }
                out.push_back(ceiling);
            }
            for (int i = 0; i < count; ++i) {
                const float clearance = rig.floorClearance(i);
                if (clearance <= 0.0f || floorExempt[static_cast<std::size_t>(i)] ||
                    !solver.isAncestorOrSelf(root, i)) {
                    continue;
                }
                // A joint whose clearance is FITTED TO ITS SKIN is stopped by the skin itself: the
                // rows are on its flesh's lowest points, rigid with the bone, so the model knows
                // what turning the bone does to them. The clearance is a constant of the tick —
                // right for a pelvis, which turns a degree a tick under a hard load, and a limit
                // cycle for a HAND: resting lightly on the floor under a forearm coming down, its
                // wrist turned 40 degrees in a tick, its clearance was 3cm other the next, the
                // floor's row threw it back up, and the wrist turned back — solves that ran out of
                // iterations, 12-16cm at the fingertips tick after tick while a sitting figure was
                // laid down on her back.
                static const bool kNoFleshRows = std::getenv("IK_JS_NO_FLESH_ROWS") != nullptr; // A/B probe
                static const bool kDropBorrowed = std::getenv("IK_JS_DROP_BORROWED") != nullptr; // probe
                if (kDropBorrowed && rig.fleshBorrowed(i)) {
                    continue; // (the neighbour's own rows stop this flesh)
                }
                const std::vector<glm::vec3>& flesh = rig.fleshPoints(i);
                if (!kNoFleshRows && !flesh.empty()) {
                    const std::size_t at = static_cast<std::size_t>(i);
                    if (f.pos[at].y - static_cast<double>(clearance) - 0.30 > static_cast<double>(floorY) + kPlaneMargin) {
                        continue; // (nowhere near: no flesh reaches 30cm further than it does now)
                    }
                    const glm::dmat3& rot = f.rot[at];
                    // The lowest point, and the low point furthest from it across the floor (a
                    // bone lying on the floor rests on two: one row is a pivot).
                    int    lowest = -1;
                    double lowestY = 0.0;
                    for (std::size_t k = 0; k < flesh.size(); ++k) {
                        const double y = f.pos[at].y + (rot * glm::dvec3(flesh[k])).y;
                        if (lowest < 0 || y < lowestY) {
                            lowest = static_cast<int>(k);
                            lowestY = y;
                        }
                    }
                    const double radius = 0.015 * static_cast<double>(rig.sizeScale()); // (the joint itself: never less)
                    const bool   jointLower = f.pos[at].y - radius < lowestY;
                    if (std::min(lowestY, f.pos[at].y - radius) >= static_cast<double>(floorY) + kPlaneMargin) {
                        continue;
                    }
                    static const bool kFleshTrace = std::getenv("IK_JS_PLANE_TRACE") != nullptr;
                    if (kFleshTrace && std::min(lowestY, f.pos[at].y - radius) < static_cast<double>(floorY) + 0.001) {
                        std::fprintf(stderr, "[js-plane] %s's flesh on the floor: %.1fmm under it (joint %.1fmm up)\n",
                                     m_boneNames[at].c_str(), (static_cast<double>(floorY) - std::min(lowestY, f.pos[at].y - radius)) * 1000.0,
                                     (f.pos[at].y - static_cast<double>(floorY)) * 1000.0);
                    }
                    JointPlaneTask row;
                    row.bone = i;
                    row.normal = glm::dvec3(0.0, 1.0, 0.0);
                    row.weight = kPlaneWeight * static_cast<double>(floorScale[at]);
                    if (jointLower) {
                        row.planePoint = glm::dvec3(0.0, static_cast<double>(floorY) + radius, 0.0);
                        out.push_back(row);
                        continue;
                    }
                    row.planePoint = glm::dvec3(0.0, static_cast<double>(floorY), 0.0);
                    // The lowest point, then — lowest first — the low points that lie well away
                    // from every one taken (a bone resting on the floor rests on several: one row
                    // is a pivot, and the lowest ALONE of a pelvis's two buttocks is whichever a
                    // rounding error says, a symmetric sit solved askew).
                    const double apart = 0.04 * static_cast<double>(rig.sizeScale());
                    std::vector<std::pair<double, int>> low;
                    for (std::size_t k = 0; k < flesh.size(); ++k) {
                        const double y = f.pos[at].y + (rot * glm::dvec3(flesh[k])).y;
                        if (y < static_cast<double>(floorY) + kPlaneMargin) {
                            low.emplace_back(y, static_cast<int>(k));
                        }
                    }
                    std::sort(low.begin(), low.end());
                    // (On a CENTRE bone a point brings its MIRROR TWIN with it, if that is low too:
                    // four taken lowest-first from a symmetric set are left, right, a midline point
                    // and then a LEFT one whose twin no longer fits, and pressed hard into the floor
                    // the odd row out is a roll: IK_JS_FLESH_NO_TWINS is the A/B.)
                    static const bool kNoTwins = std::getenv("IK_JS_FLESH_NO_TWINS") != nullptr;
                    const bool        centreBone = !kNoTwins && std::abs(m_bones[at].inverseBind[3][0]) < 0.01f * rig.sizeScale();
                    std::vector<glm::dvec3> taken;
                    std::vector<int>        takenIndex;
                    const auto take = [&](int index, const glm::dvec3& world) {
                        taken.push_back(world);
                        takenIndex.push_back(index);
                        row.offset = glm::dvec3(flesh[static_cast<std::size_t>(index)]);
                        out.push_back(row);
                    };
                    const auto clearOf = [&](const glm::dvec3& world) {
                        for (const glm::dvec3& t : taken) {
                            if (std::hypot(world.x - t.x, world.z - t.z) < apart) {
                                return false;
                            }
                        }
                        return true;
                    };
                    for (const auto& candidate : low) {
                        const glm::vec3& local = flesh[static_cast<std::size_t>(candidate.second)];
                        const glm::dvec3 w = rot * glm::dvec3(local);
                        if (std::find(takenIndex.begin(), takenIndex.end(), candidate.second) != takenIndex.end() || !clearOf(w)) {
                            continue;
                        }
                        take(candidate.second, w);
                        if (centreBone && std::abs(static_cast<double>(local.x)) > 0.5 * apart) {
                            const glm::vec3 mirrored(-local.x, local.y, local.z);
                            int             twin = -1;
                            float           nearest = 0.01f * rig.sizeScale();
                            for (const auto& other : low) {
                                const float d = glm::length(flesh[static_cast<std::size_t>(other.second)] - mirrored);
                                if (other.second != candidate.second && d < nearest) {
                                    nearest = d;
                                    twin = other.second;
                                }
                            }
                            if (twin >= 0 && std::find(takenIndex.begin(), takenIndex.end(), twin) == takenIndex.end()) {
                                const glm::dvec3 twinWorld = rot * glm::dvec3(flesh[static_cast<std::size_t>(twin)]);
                                if (clearOf(twinWorld)) {
                                    take(twin, twinWorld);
                                }
                            }
                        }
                        if (taken.size() >= 4) {
                            break;
                        }
                    }
                    continue;
                }
                const double level = static_cast<double>(floorY) + clearance - static_cast<double>(floorAllow[static_cast<std::size_t>(i)]);
                if (f.pos[static_cast<std::size_t>(i)].y < level + kPlaneMargin) {
                    static const bool kFloorTrace = std::getenv("IK_JS_PLANE_TRACE") != nullptr;
                    if (kFloorTrace && f.pos[static_cast<std::size_t>(i)].y < level + 0.001) {
                        std::fprintf(stderr, "[js-plane] %s on the floor: %.1fmm under its clearance (%.1fmm; y %.4f floor %.4f)\n",
                                     m_boneNames[static_cast<std::size_t>(i)].c_str(),
                                     (level - f.pos[static_cast<std::size_t>(i)].y) * 1000.0, clearance * 1000.0f, f.pos[static_cast<std::size_t>(i)].y, static_cast<double>(floorY));
                    }
                    JointPlaneTask row;
                    row.bone = i;
                    row.planePoint = glm::dvec3(0.0, level, 0.0);
                    row.normal = glm::dvec3(0.0, 1.0, 0.0);
                    row.weight = kPlaneWeight * static_cast<double>(floorScale[static_cast<std::size_t>(i)]);
                    out.push_back(row);
                }
            }
            const auto pos = [&f](int i) { return glm::vec3(f.pos[static_cast<std::size_t>(i)]); };
            for (const BodyVolume& v : rig.dragVolumes()) {
                const VolumeAxis ax = volumeAxis(v, count, pos);
                if (!ax.ok) {
                    continue;
                }
                const std::size_t ref = static_cast<std::size_t>(v.a);
                const glm::dmat3 refInv = glm::transpose(f.rot[ref]);
                const auto emit = [&](int bone, const glm::dvec3& offset, const glm::vec3& point,
                                      const glm::vec3& onAxis, float radius, double share = 1.0) {
                    const glm::vec3 away = point - onAxis;
                    const float dist = glm::length(away);
                    if (dist < 1e-5f || radius - dist < -static_cast<float>(kPlaneMargin)) {
                        return; // well clear (or degenerate: on the axis, no normal to push along)
                    }
                    static const bool kPlaneTrace = std::getenv("IK_JS_PLANE_TRACE") != nullptr;
                    if (kPlaneTrace && radius - dist > -0.001f) {
                        std::fprintf(stderr, "[js-plane] %s against %s-%s: %.1fmm inside\n",
                                     m_boneNames[static_cast<std::size_t>(bone)].c_str(),
                                     m_boneNames[static_cast<std::size_t>(v.a)].c_str(),
                                     v.b >= 0 ? m_boneNames[static_cast<std::size_t>(v.b)].c_str() : "-",
                                     (radius - dist) * 1000.0f);
                    }
                    const glm::dvec3 nrm(away / dist);
                    const glm::dvec3 surface = glm::dvec3(onAxis) + nrm * static_cast<double>(radius);
                    JointPlaneTask row;
                    row.bone = bone;
                    row.offset = offset;
                    row.refBone = v.a;
                    row.planePoint = refInv * (surface - f.pos[ref]);
                    row.normal = refInv * nrm;
                    row.weight = kPlaneWeight * share * share; // (the share is the DEPTH's: the row's residual goes as it)
                    out.push_back(row);
                };
                for (int i = 0; i < count; ++i) {
                    const char applies = v.applies[static_cast<std::size_t>(i)];
                    if (!applies || volumeExempt[static_cast<std::size_t>(i)]) {
                        continue; // (a hanging arm's joint — see above)
                    }
                    const glm::vec3 P = pos(i);
                    const float t = ax.ab2 > 1e-12f
                                        ? glm::clamp(glm::dot(P - ax.A, ax.ab) / ax.ab2, 0.0f, 1.0f)
                                        : 0.0f;
                    const float clearance =
                        applies == kVolumeAppliesRider ? rig.riderClearance() : rig.volumeClearance(i);
                    const float segR = rig.volumeSegmentRadius(i);
                    const int par = m_bones[static_cast<std::size_t>(i)].parent;
                    // (A joint that ENDS a fleshy segment is as wide as the segment's end — the knee
                    // is the thigh's cap: its own row keeps the segment's flesh margin, not the bare
                    // joint clearance. The segment row below stands down within 2% of its ends, and
                    // held there at 1.5cm the model's margin dropped 2cm the moment two crossed
                    // thighs touched at the knee: on three characters with fuller thighs the legs
                    // crossed with the knees 13-14mm inside each other by the harness's measure,
                    // which counts the segment's flesh right to its end. IK_JS_BARE_JOINT_CAPS)
                    static const bool kBareCaps = std::getenv("IK_JS_BARE_JOINT_CAPS") != nullptr; // A/B probe
                    const bool capped = !kBareCaps && applies != kVolumeAppliesRider && volumeAppliesSegment(v, applies) &&
                                        segR > 0.0f && par >= 0;
                    emit(i, glm::dvec3(0.0), P, ax.A + ax.ab * t,
                         volumeRadiusAt(v, t) + (capped ? std::max(kVolumeSegmentFlesh * segR, clearance) : clearance));
                    // The SEGMENT into the joint (two limbs cross at their middles): its closest
                    // point rides the parent bone.
                    if (applies != kVolumeAppliesRider && volumeAppliesSegment(v, applies) &&
                        segR > 0.0f && par >= 0) {
                        float sPar = 0.0f;
                        float tPar = 0.0f;
                        closestSegmentPoints(pos(par), P, ax.A, ax.B, sPar, tPar);
                        if (sPar > 0.02f && sPar < 0.98f) { // the ends are the joint tests'
                            // (... and the row COMES IN from each end over kSegmentRowRamp of the
                            // segment, 2026-09-28: cut in at full strength 2% along, it made the
                            // cost of a pose DISCONTINUOUS — an upper arm lying against the chest
                            // with its closest point at the socket, whose joint the chest has no
                            // row for, cost nothing, and a hair further along 19 (1.4mm through at
                            // 1e7). Judged by its own rows (JointSolver: a trial is judged by the
                            // rows it makes) no step across that line is downhill, however small:
                            // the male rig's prone descent stood still for twelve ticks under a
                            // leaving cursor and let go 13cm at once. IK_JS_SEGMENT_ROWS_CUT.)
                            static const bool kSegmentRowsCut = std::getenv("IK_JS_SEGMENT_ROWS_CUT") != nullptr; // A/B probe
                            const double ramp = kSegmentRowsCut
                                                    ? 1.0
                                                    : static_cast<double>(glm::smoothstep(0.02f, kSegmentRowRamp, sPar) *
                                                                          (1.0f - glm::smoothstep(1.0f - kSegmentRowRamp, 0.98f, sPar)));
                            const glm::vec3 onSeg = glm::mix(pos(par), P, sPar);
                            const std::size_t pb = static_cast<std::size_t>(par);
                            emit(par, glm::transpose(f.rot[pb]) * (glm::dvec3(onSeg) - f.pos[pb]), onSeg,
                                 ax.A + ax.ab * tPar,
                                 volumeRadiusAt(v, tPar) + std::max(kVolumeSegmentFlesh * segR, clearance), ramp);
                        }
                    }
                }
            }
        };
    }
}

} // namespace pose

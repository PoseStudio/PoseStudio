/**
 * @file armatureikpost.cpp
 * @brief The drag tick's POST-STEPS, run after each solve (dragIkTick, armatureiksolve.cpp):
 *        IDLE ARMS HANG (hangIdleArms) and THE HEAD STAYS UP (rightIdleHead) — and what they
 *        mean for balance, which is blind to both (balancePositions, balanceIsBlind).
 *
 * Neither is part of the solve: the arms and the head are turned on the solved pose, at a
 * bounded rate, and stopped at the body volumes and the floor like any FK rotation. Each
 * section's comment says why it is a post-step and what was tried inside the solve first.
 * Qt-free (std + GLM).
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
#include <functional>
#include <tuple>

namespace pose {

// --- IDLE ARMS HANG ------------------------------------------------------------------------------
//
// An arm nothing has hold of — not the dragged limb, no pin or floor contact in it — that HUNG at
// its owner's side when the drag began (the upper arm within kArmHangCone of straight down, the
// elbow all but straight) keeps hanging as the trunk moves: after each tick's solve the shoulder is
// turned so the upper arm keeps the direction the drag found it in — its ELEVATION, that is: the
// direction is carried round with the trunk's heading, so a body that turns takes its arms round
// with it, and one that folds, leans or sits does not take them up with it. Riding the chest
// rigidly, as every idle limb does, a bowing figure held both arms stiffly back along her body,
// the hands above her hips like a ski jumper's — the first thing the in-app screenshots showed.
//
// A POST-STEP, not part of the solve. Three attempts inside it were backed out (2026-09-20): a soft
// hold of the elbow's place pulled against the cursor (3.5mm); a posture reference for the
// shoulder's whole world rotation asks the bend bone for a twist its locked channel cannot make
// (the arm swung backward); and a reference for its DIRECTION alone was right, but any unknown is a
// lever for the balance row — at a limb's price both arms were swung 50 degrees back as
// counterweights. Here the shoulder is no unknown: the solve sees the arms' weight where the last
// tick hung them (its centre-of-mass model reads the pose as it stands) and cannot move it. The
// turn is rate-limited (kArmHangStepDeg a tick) and stopped at the body volumes like any FK
// rotation (fkVolumeDepth: a side bend must not swing an arm into the hip) and at the floor.
// Posed arms — raised, akimbo, a hand on a hip — are not hanging and ride as they always did.
//
// And BALANCE IS BLIND TO THE HANG (balancePositions): the solve's balance row, its slack capture
// and the rig's step policy all weigh the arms where they would RIDE. A bowing figure's hanging
// arms carry a tenth of her weight 15cm further forward than riding ones, and weighed there every
// fold met balance's wall 2-5cm sooner — the bow rested 36mm behind its cursor, a crouching
// lean 34, the un-bow left the pelvis pitched — for a centimetre and a half of centre of mass,
// half the margin the support is inset by. The arms are for the eye; balance keeps the books it
// kept, and every reach, step and gate is what it was.
constexpr float kArmHangCone = 0.5f;      // cos 60 deg: an A-pose arm stands 46 degrees off the vertical
// ... and an arm within this many degrees of the elevation it was BOUND at is unposed too, while that
// bind stands under kArmHangBindMaxDeg from straight down (2026-11-03: an imported rig's A-pose may
// stand 63 degrees off the vertical, three outside the cone, and every idle arm then rode as posed;
// a T-pose, 90 degrees, stays posed — the manual says to lower the arms first). The reference rigs
// bind at 46 degrees, inside the cone, so nothing changes there.
constexpr float kArmHangBindSlackDeg = 12.0f;
constexpr float kArmHangBindMaxDeg = 75.0f;
constexpr float kArmHangElbowDeg = 35.0f; // more bend than this at the press is a POSED arm
constexpr float kArmHangStepDeg = 4.0f;   // per tick, the largest channel (a fingertip 4cm: the jump gates)
constexpr float kArmHangMinLength = 0.12f; // socket to fold joint: an upper arm, not a finger's first bone
constexpr float kArmHangDamping = 3.0e-5f;   // on the hang step's normal matrix (per-degree columns)
// While the seat rolls (or the hips go down) an idle arm keeps this far over its floor clearances.
// A CENTIMETRE CLEAR OF THE RIG'S SEED BAND (2026-10-28; IkRig's kSeedContactBand is 2cm): the rig
// seeds a joint within that band of its clearance as a live contact at the next press, and with
// the floor-avoid turn made continuous (the bisection below) the arm rests EXACTLY at this margin
// — at 2cm it sat on the seed band's edge, and a hand a millimetre lower was planted at the next
// press: stood up out of a sit by the hips, the floor gallery's hands were seeded, the trunk's
// home froze on the planted hands and no step came — she stood up folded in half with her hands
// on the floor, 30cm short. (The quantized turn had rested the arm up to 4.7 degrees above the
// margin, so nothing hangs higher than it did.) Two measures of one fact must not meet at the edge.
constexpr float kArmHangFloorMargin = 0.03f;
constexpr float kWristRelaxStepDeg = 4.0f;   // a lifted hand's wrist eases toward neutral this fast (per channel, per tick)
constexpr float kFingerRelaxShare = 0.25f;    // an idle hand's fingers curl to this share of each joint's flexion range ...
constexpr float kFingerRelaxStepDeg = 3.0f;   // ... this fast (per channel, per tick) ...
constexpr float kFingerRelaxMinRangeDeg = 30.0f; // ... on the joints that span at least this (the carpals do not)
constexpr float kFingerUnposedDeg = 0.5f;     // a hand whose finger channels all lie within this of the bind at the press is UNPOSED
constexpr float kFingerOpenHeight = 0.15f;    // a hand this close to the floor (its lowest part, figure-scaled) OPENS before it lands
// IDLE ELBOWS SOFTEN (2026-11-02): a hanging arm whose elbow the hang OWNS bends this much at the
// elbow -- a relaxed arm hangs with a soft elbow, never ramrod straight -- at this pace (a fingertip
// 4mm a tick: under every jump gate); an elbow within kArmIdleElbowUnposedDeg of the bind at the
// press, or of where the hang itself left it last (m_hangElbowDeg), is OURS. A posed elbow is not.
constexpr float kArmIdleElbowDeg = 12.0f;
constexpr float kArmIdleElbowStepDeg = 0.5f;
constexpr float kArmIdleElbowUnposedDeg = 0.5f;
constexpr float kArmIdleElbowClear = 0.01f;    // the bend stops this far SHORT of touching anything (the other forearm, the hip): a cosmetic bend makes no contact
constexpr float kArmHangSettle = 0.0015f;    // the stop lets a hanging arm settle this much deeper into a volume than the press had it
// THE STRIDE SWING (2026-10-27): a hanging arm swings AGAINST the stance — forward on the side of
// the trailing foot, back on the side of the leading one, as a walker's do — by the standing feet's
// fore-aft split (the same-side foot ahead of the other, metres), saturating toward kArmSwingMaxDeg,
// faded as the hips come down (a lunge is a stride with the hips low, and no walk). In every walking
// frame the arms hung straight down like a mannequin's.
constexpr float kArmSwingPerMetre = 1.0f;  // radians of swing per metre of split beyond the dead zone: a 50cm stride swings an arm ~24 degrees
constexpr float kArmSwingDeadZone = 0.04f; // of split (figure-scaled): a stance from here. Without it a hair of heading yaw across a
                                           // 12cm-wide stance read as a split of a fifth of a millimetre, the forward arm's elbow took a
                                           // hundredth of a degree of it, and a chaotic reach to the floor flipped into its other basin.
constexpr float kArmSwingMaxDeg = 32.0f;   // ... saturating toward this
constexpr float kArmSwingElbowDeg = 18.0f; // the forward-swung arm's elbow flexes up to this, at the full swing
constexpr float kArmSwingLowFrom = 0.08f;  // the swing fades as the hips drop below standing: from this far (figure-scaled) ...
constexpr float kArmSwingLowGone = 0.25f;  // ... gone by this
constexpr float kArmHangOutMost = 0.21f;     // a blocked hang asks for the same hang turned OUT, up to this (radians: 12 degrees) ...
constexpr int   kArmHangOutBisections = 8;   // ... found by bisection to this many halvings (0.05 degrees) ...
constexpr float kArmHangOutThrough = 0.9f;   // ... the least angle whose turn goes this far through

bool Armature::hangArmHeld(const HangArm& arm) const {
    if (!m_ikRig) {
        return false;
    }
    // (Through hangIdleArms — whose parts ask this several times an arm a tick, and whose inputs,
    // the pins, their shadows and the unloaded contacts, do not change meanwhile — the answer per
    // arm of m_jsHangArms is cached: m_jsHangHeld.)
    if (m_jsHangHeldCached && !m_jsHangArms.empty() && std::less_equal<const HangArm*>()(m_jsHangArms.data(), &arm) &&
        std::less<const HangArm*>()(&arm, m_jsHangArms.data() + m_jsHangArms.size())) {
        return m_jsHangHeld[static_cast<std::size_t>(&arm - m_jsHangArms.data())] != 0;
    }
    const std::vector<IkEffector>& pins = m_ikRig->pins();
    for (std::size_t p = 0; p < pins.size(); ++p) {
        const IkEffector& pin = pins[p];
        static const bool kShadowHolds = std::getenv("IK_SHADOW_HOLDS_ARM") != nullptr; // A/B probe
        if (!kShadowHolds && (m_ikRig->pinIsShadow(p) ||
                              (pin.node >= 0 && static_cast<std::size_t>(pin.node) < m_jsContactUnloaded.size() && m_jsContactUnloaded[static_cast<std::size_t>(pin.node)]))) {
            continue; // (a shadow of a support, or a contact the solve has unloaded, holds nothing: the arm is the hang's)
        }
        for (int cur = pin.node; cur >= 0; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            if (cur == arm.socket) {
                return true;
            }
        }
    }
    return false;
}

bool Armature::balanceIsBlind() const {
    for (const HangArm& arm : m_jsHangArms) {
        const std::size_t s = static_cast<std::size_t>(arm.socket);
        if (!hangArmHeld(arm) && glm::length(m_boneEuler[s] - m_ikStartEuler[s]) >= 1.0e-3f) {
            return true;
        }
    }
    for (const int bone : m_jsHeadFree) { // (the bones the righting HAS: under a head drag the neck's are the solve's)
        const std::size_t b = static_cast<std::size_t>(bone);
        if (glm::length(m_boneEuler[b] - m_ikStartEuler[b]) >= 1.0e-3f) {
            return true;
        }
    }
    return false;
}

std::vector<glm::vec3> Armature::balancePositions() const {
    const std::size_t n = m_bones.size();
    std::vector<glm::vec3> positions(n);
    for (std::size_t i = 0; i < n; ++i) {
        positions[i] = glm::vec3(m_poseGlobal[i][3]);
    }
    // The joints a post-step turned, put back at the rotation the drag found them with: a hung
    // arm's shoulder, the righted neck's bones. Everything below keeps the local pose it has
    // (children follow parents in the bone order).
    std::vector<char> putBack(n, 0);
    bool              any = false;
    const auto mark = [&](int bone) {
        const std::size_t b = static_cast<std::size_t>(bone);
        if (m_bones[b].parent >= 0 && glm::length(m_boneEuler[b] - m_ikStartEuler[b]) >= 1.0e-3f) {
            putBack[b] = 1;
            any = true;
        }
    };
    for (const HangArm& arm : m_jsHangArms) {
        // (A hand on the floor WHEN THE DRAG BEGAN is a contact: that arm is the solve's, and weighed
        // where it is. One that LANDS during the drag stays unweighed for the rest of it: balance had
        // been blind to it all the way down, and seen the tick its hand touched the floor a tenth of
        // her weight arrived 40cm ahead of where the books had it - 4cm of centre of mass in one
        // tick, against a support that grows in at a centimetre a tick. IK_ARM_LANDED_WEIGHED.)
        static const bool kLandedWeighed = std::getenv("IK_ARM_LANDED_WEIGHED") != nullptr; // A/B probe
        if (!hangArmHeld(arm) || (arm.landed && !kLandedWeighed)) {
            mark(arm.socket);
            // (... and its ELBOW, where the soften has turned it — IDLE ELBOWS SOFTEN, 2026-11-02: the
            // idle bend carries both forearms and hands 5cm forward, 2-3mm of centre of mass, and
            // weighed it leaned every hip hinge 13mm less at the head. The arms are for the eye.
            // IK_ARM_HANG_ELBOW_WEIGHED.)
            static const bool kElbowWeighed = std::getenv("IK_ARM_HANG_ELBOW_WEIGHED") != nullptr; // A/B probe
            if (!kElbowWeighed && arm.elbow >= 0 && static_cast<std::size_t>(arm.elbow) < m_jsPostStepTurned.size() &&
                m_jsPostStepTurned[static_cast<std::size_t>(arm.elbow)]) {
                mark(arm.elbow);
            }
        }
    }
    static const bool kHeadSeen = std::getenv("IK_HEAD_RIGHT_NOT_BLIND") != nullptr; // A/B probe
    for (const int bone : m_jsHeadFree) {
        if (!kHeadSeen) {
            mark(bone);
        }
    }
    // (... and to the FINGERS the relax has turned — IDLE HANDS RELAX: every post-step-turned bone
    // below a wrist. Their mass is a token, but a token is not nothing to a threshold: the relaxing
    // fingers' hair of centre-of-mass shift tipped a knife-edge step start on one character, and a
    // walking bow let go of four ticks into its hold had no step in flight. IK_FINGERS_WEIGHED.)
    static const bool kFingersWeighed = std::getenv("IK_FINGERS_WEIGHED") != nullptr; // A/B probe
    for (std::size_t b = 0; !kFingersWeighed && b < n && b < m_jsPostStepTurned.size() && b < m_jsWristOf.size(); ++b) {
        if (m_jsPostStepTurned[b] && m_jsWristOf[b] >= 0 && m_jsWristOf[b] != static_cast<int>(b)) {
            mark(static_cast<int>(b));
        }
    }
    if (!any) {
        return positions;
    }
    std::vector<glm::mat4> ride(n);
    std::vector<char>      rides(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const int p = m_bones[i].parent;
        if (p < 0 || (!putBack[i] && !rides[static_cast<std::size_t>(p)])) {
            continue;
        }
        const Bone& bone = m_bones[i];
        glm::mat4   local = bone.poseLocal;
        if (putBack[i]) {
            local = bone.localBind * bone.orient * eulerMatrix(m_ikStartEuler[i], bone.rotationOrder) * bone.invOrient;
            local[3] += glm::vec4(m_boneTranslation[i], 0.0f);
        }
        const std::size_t par = static_cast<std::size_t>(p);
        ride[i] = (rides[par] ? ride[par] : m_poseGlobal[par]) * local;
        rides[i] = 1;
        positions[i] = glm::vec3(ride[i][3]);
    }
    return positions;
}

namespace {

/// An upper arm's fore-aft angle from straight down about @p fore (degrees, + forward).
float sagittalDeg(const glm::vec3& dir, const glm::vec3& fore) {
    return glm::degrees(std::atan2(glm::dot(dir, fore), -dir.y));
}

} // namespace

/// THE STRIDE SWING (kArmSwingPerMetre): the stance's fore-aft SPLIT for the arm on side
/// @p armSide (+1 her left, -1 her right) — the same-side foot ahead of the other along
/// @p fore, metres — read off the two standing feet: the rig's pins (a stepping foot's target
/// walks through its swing, so the arms swing through a walk) and the DRAGGED foot where the
/// effector is one, by its own position on the floor, fading as it lifts (a foot slid forward
/// is a stride; one carried in the air is no stance). A lifted or a live foot leaves no
/// stance to read. False without exactly one foot a side.
bool Armature::hangStanceSplit(float armSide, const glm::vec3& fore, float& split) const {
    const IkRig&      rig = *m_ikRig;
    const std::size_t n = m_bones.size();
    const int         root = rig.rootNode();
    const float       floorModelY = -m_transform[3][1];
    const std::vector<IkEffector>& pins = rig.pins();
    const float scale = rig.sizeScale();
    glm::vec3   same(0.0f);
    glm::vec3   other(0.0f);
    float       sameWeight = 0.0f;
    float       otherWeight = 0.0f;
    int         sameCount = 0;
    int         otherCount = 0;
    const auto  count = [&](int node, const glm::vec3& at, float weight) {
        const float footSide = m_ikBindPos[static_cast<std::size_t>(node)].x - m_ikBindPos[static_cast<std::size_t>(root)].x;
        if (std::abs(footSide) < 0.01f * scale) {
            return;
        }
        if ((footSide > 0.0f) == (armSide > 0.0f)) {
            same = at;
            sameWeight = weight;
            ++sameCount;
        } else {
            other = at;
            otherWeight = weight;
            ++otherCount;
        }
    };
    for (std::size_t p = 0; p < pins.size(); ++p) {
        const int node = pins[p].node;
        if (node < 0 || static_cast<std::size_t>(node) >= n || rig.pinIsLive(p) || rig.pinIsUser(p) ||
            !rig.isFootClass(node)) {
            continue;
        }
        count(node, pins[p].target, 1.0f);
    }
    const int effector = rig.dragEffector();
    if (effector >= 0 && static_cast<std::size_t>(effector) < n && effector != root &&
        rig.isFootClass(effector)) {
        const glm::vec3 at(m_poseGlobal[static_cast<std::size_t>(effector)][3]);
        const float     lift = at.y - (m_ikBindPos[static_cast<std::size_t>(effector)].y + floorModelY);
        count(effector, at, 1.0f - glm::smoothstep(0.02f * scale, 0.12f * scale, lift));
    }
    if (sameCount != 1 || otherCount != 1) {
        return false;
    }
    split = glm::dot(same - other, fore) * sameWeight * otherWeight;
    return true;
}

/// ... and the swing that split gives an arm, degrees, + forward (the same-side foot ahead: the
/// arm swings BACK), faded as the hips come down below standing — a lunge is no walk —
/// saturating toward kArmSwingMaxDeg.
float Armature::hangSwingDegOf(float split) const {
    const IkRig&      rig = *m_ikRig;
    const int         root = rig.rootNode();
    const float       floorModelY = -m_transform[3][1];
    const float scale = rig.sizeScale();
    const float drop = (m_ikBindPos[static_cast<std::size_t>(root)].y + floorModelY) -
                       m_poseGlobal[static_cast<std::size_t>(root)][3].y;
    const float low = 1.0f - glm::smoothstep(kArmSwingLowFrom * scale, kArmSwingLowGone * scale, drop);
    const float cap = glm::radians(kArmSwingMaxDeg);
    const float beyond = std::max(0.0f, std::abs(split) - kArmSwingDeadZone * scale) * (split < 0.0f ? -1.0f : 1.0f);
    return glm::degrees(cap * std::tanh(-kArmSwingPerMetre * beyond * low / cap));
}

/// True once no pin (but a shadow of one) stands in @p arm: its hand has really lifted.
bool Armature::hangHandLifted(const HangArm& arm) const {
    const IkRig&      rig = *m_ikRig;
    const std::vector<IkEffector>& pins = rig.pins();
    for (std::size_t p = 0; p < pins.size(); ++p) {
        if (!rig.pinIsShadow(p) && boneUnder(pins[p].node, arm.socket)) {
            return false;
        }
    }
    return true;
}

/// The least swing, on top of the rotation the drag found, that points the upper arm at @p wanted
/// (world = parent . orient . R . orient^-1; the twist channel is none of its business).
bool Armature::hangTurnFor(const HangArm& arm, const glm::vec3& wanted, HangTurn& turn, float stepDeg) const {
    const float pace = stepDeg > 0.0f ? stepDeg : kArmHangStepDeg;
    const std::size_t s = static_cast<std::size_t>(arm.socket);
    const std::size_t par = static_cast<std::size_t>(m_bones[s].parent);
    const glm::mat3   parentNow(m_poseGlobal[par]);
    const glm::mat3   toLocal = glm::transpose(glm::mat3(m_bones[s].orient));
    const glm::vec3   rides = glm::normalize(toLocal * glm::transpose(m_jsStartRot[par]) * arm.direction);
    const glm::vec3   hangsAt = glm::normalize(toLocal * glm::transpose(parentNow) * wanted);
    // The shoulder's FREE channels are turned toward it by a Gauss-Newton step on the
    // direction itself, from where they stand — no Euler angles are read off a matrix: an
    // arm posed forward (an all-fours pre-pose has the shoulder's middle channel at -90, its
    // gimbal lock) came back from the extraction on another branch, the locked twist channel's
    // share was dropped, and the arm flew up over her back.
    const glm::vec3 before = m_boneEuler[s];
    const glm::vec3 atZero = glm::transpose(glm::mat3(eulerMatrix(m_ikStartEuler[s], m_bones[s].rotationOrder))) * rides;
    const auto pointsAt = [&](const glm::vec3& euler) {
        return glm::normalize(glm::mat3(eulerMatrix(euler, m_bones[s].rotationOrder)) * atZero);
    };
    const glm::vec3 miss = hangsAt - pointsAt(before);
    glm::vec3       goal = before;
    float           farthest = 0.0f;
    if (glm::length(miss) > 1.0e-5f) {
        // J (3 x free channels) by differences, then (J^T J + lambda) d = J^T miss.
        glm::vec3 column[3];
        bool      isFree[3];
        // (Not the shoulder's TWIST channel, where it has one of its own: turning an arm about
        // itself points it nowhere — but beside a bent channel it does, a little, and on the one
        // generation whose upper arm is a single three-channel bone the step went down that
        // road: under a deep fold the arm was twisted 78 degrees, ran a swing channel into its
        // limit and stood out sideways, stepping 4 degrees one way and the other every tick.)
        static const bool kHangTwists = std::getenv("IK_ARM_HANG_TWISTS") != nullptr; // A/B probe
        const int freeCount = freeChannelCount(static_cast<int>(s));
        const int twistAxis = (!kHangTwists && freeCount == 3 && s < m_jsTwistAxis.size()) ? m_jsTwistAxis[s] : -1;
        for (int a = 0; a < 3; ++a) {
            const Bone& bone = m_bones[s];
            isFree[a] = !(bone.rotLimited[a] && bone.rotMax[a] - bone.rotMin[a] < kLockedRangeDeg) && a != twistAxis;
            glm::vec3 nudged = before;
            nudged[a] += 0.5f;
            column[a] = isFree[a] ? (pointsAt(nudged) - pointsAt(before)) / 0.5f : glm::vec3(0.0f);
        }
        glm::mat3 normal(0.0f);
        glm::vec3 rhs(0.0f);
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                normal[b][a] = glm::dot(column[a], column[b]);
            }
            // (Damped: the columns are per degree, 3e-4 on the diagonal; at 1e-8 two channels
            // that point the arm nearly the same way were played against each other.)
            normal[a][a] += isFree[a] ? kArmHangDamping : 1.0f;
            rhs[a] = glm::dot(column[a], miss);
        }
        const glm::vec3 step = glm::inverse(normal) * rhs;
        for (int a = 0; a < 3; ++a) {
            const Bone& bone = m_bones[s];
            if (!isFree[a]) {
                continue;
            }
            goal[a] = before[a] + step[a];
            if (bone.rotLimited[a]) {
                goal[a] = glm::clamp(goal[a], bone.rotMin[a], bone.rotMax[a]);
            }
            farthest = std::max(farthest, std::abs(goal[a] - before[a]));
        }
    }
    if (farthest < 0.01f) {
        return false;
    }
    turn = HangTurn{&arm, s, before, (goal - before) * std::min(1.0f, pace / farthest), wanted};
    return true;
}

/// How far @p t's arm stands inside the body volumes or under the floor as the pose stands.
float Armature::hangTurnDepth(const HangTurn& t) const {
    const IkRig& rig = *m_ikRig;
    return std::max(rig.bodyVolumes().empty() ? 0.0f : fkVolumeDepth(t.arm->socket), hangTurnFloorDepth(t));
}

float Armature::hangTurnFloorDepth(const HangTurn& t) const {
    const IkRig&      rig = *m_ikRig;
    const std::size_t n = m_bones.size();
    const float       floorModel = -m_transform[3][1];
    float worst = -1.0e9f;
    for (std::size_t i = t.s; i < n; ++i) {
        if (boneUnder(static_cast<int>(i), t.arm->socket)) {
            // (The clearance AS THE BONE IS TURNED NOW, the hang having just turned it, which
            // is what the solve's floor rows, on the flesh itself, hold it to: read off the
            // tick's first rotation the two disagreed by millimetres, and a hanging hand at
            // the floor under a held sit was stepped back and forth, 4cm at a fingertip.)
            const float clearance = rig.floorClearanceAlong(
                static_cast<int>(i), glm::transpose(glm::mat3(m_poseGlobal[i])) * glm::vec3(0.0f, -1.0f, 0.0f));
            worst = std::max(worst, floorModel + clearance - m_poseGlobal[i][3].y);
        }
    }
    return worst;
}

/// Turns @p t's shoulder @p fraction of the way along its turn.
void Armature::hangTurnTo(const HangTurn& t, float fraction) {
    m_boneEuler[t.s] = t.before + t.delta * fraction;
    applyBoneEuler(t.arm->socket);
}

/// How deep a turn may leave its arm: where the drag found it, kArmHangSettle deeper at most.
float Armature::hangTurnAllow(const HangTurn& t) {
    return std::max({t.depthBefore, 0.002f, t.arm->depthAtPress + kArmHangSettle});
}

/// THE STOP for one turn: the FK collision stop at the body volumes and the floor — the turn in
/// full, else bisected back to the largest fraction that goes no deeper than hangTurnAllow.
void Armature::hangStopFraction(HangTurn& t) {
    const IkRig&      rig = *m_ikRig;
    const std::size_t n = m_bones.size();
    const float       floorModel = -m_transform[3][1];
    // (... and an arm may SETTLE kArmHangSettle deeper into a volume than the drag FOUND it —
    // no deeper: a swing is stopped exactly where the press had it otherwise, and a chest is
    // wider lower down. An arm coming down beside it from a forward hang — a figure getting
    // up off all fours by the chest, her collars shrugged forward and the mid-upper-arm point
    // 2mm inside the chest's volume — deepened that by microns with every degree of swing,
    // and the stop refused the whole of it: she came up with both arms held straight out in
    // front of her, tick after tick. Read off the PRESS, not the tick — a tolerance a tick
    // is a creep: an arm hung into a hip by a side bend would go in 0.3mm a tick for as
    // long as the bend is held.)
    const float allow = hangTurnAllow(t);
    // (... and never deeper INTO THE FLOOR than the tick found the arm, 2026-10-29: the tolerance
    // above is the volumes', and against the floor it was a RATCHET — the solve's floor rows on a
    // hanging arm's fingers stay, and only the body can answer them: an arm pushed 2mm into the floor
    // by its turn lifted the whole body 2mm at the next solve, and the turn went 2mm in again — a
    // prone figure's hips climbed 1.3mm a tick through a still hold once her hands had let go.)
    const float floorAllow = std::max(t.floorDepthBefore, 0.0f);
    t.fraction = 1.0f;
    hangTurnTo(t, 1.0f);
    static const bool kStopTrace = std::getenv("IK_ARM_HANG_TRACE") != nullptr;
    if (kStopTrace) {
        float floorWorst = -1.0e9f;
        int   floorJoint = -1;
        for (std::size_t i = t.s; i < n; ++i) {
            if (boneUnder(static_cast<int>(i), t.arm->socket)) {
                const float clearance = rig.floorClearanceAlong(
                    static_cast<int>(i), glm::transpose(glm::mat3(m_poseGlobal[i])) * glm::vec3(0.0f, -1.0f, 0.0f));
                const float d = floorModel + clearance - m_poseGlobal[i][3].y;
                if (d > floorWorst) {
                    floorWorst = d;
                    floorJoint = static_cast<int>(i);
                }
            }
        }
        std::fprintf(stderr, "[hang-stop] %s: before %.2f allow %.2f (press %.2f) full turn: volumes %.2f floor %.2f at %s",
                     m_boneNames[t.s].c_str(), t.depthBefore * 1000.0f, allow * 1000.0f, t.arm->depthAtPress * 1000.0f,
                     (rig.bodyVolumes().empty() ? 0.0f : fkVolumeDepth(t.arm->socket)) * 1000.0f, floorWorst * 1000.0f,
                     floorJoint >= 0 ? m_boneNames[static_cast<std::size_t>(floorJoint)].c_str() : "-");
        std::fputc(10, stderr);
    }
    const auto tooDeep = [&]() { return hangTurnDepth(t) > allow || hangTurnFloorDepth(t) > floorAllow; };
    if (tooDeep()) {
        float lo = 0.0f;
        float hi = 1.0f;
        for (int k = 0; k < 6; ++k) {
            const float mid = 0.5f * (lo + hi);
            hangTurnTo(t, mid);
            (tooDeep() ? hi : lo) = mid;
        }
        t.fraction = lo;
    }
    hangTurnTo(t, 0.0f);
}

/// The search for the arms that HANG (see the file note), with what they want and carry: once per
/// drag, at the first tick — and then every tick for an arm whose hand LANDS mid-drag.
void Armature::findHangArms() {
    const IkRig&      rig = *m_ikRig;
    const std::size_t n = m_bones.size();
    const int         root = rig.rootNode();
    // (ONCE per drag — and then EVERY TICK for an arm whose hand lands, 2026-10-29: a riding arm whose
    // hand becomes a LIVE contact is the hang's once that contact lifts, exactly as an arm whose hand
    // was on the floor at the press is. Not listed, the heavy male's arms rode where a prone descent's
    // release left them — straight and horizontal, the wrist flexed, the fingertips on the floor: a
    // strut under the shoulder, the hips 10cm above his belly; his hands stood a hair above the rig's
    // seed band at the press and were pinned two ticks in.)
    const bool firstFind = !m_jsHangArmsFound;
    if (firstFind) {
        m_jsHangArmsFound = true;
        m_jsHangArms.clear();
    }
    {
        for (std::size_t e = 0; e < n; ++e) {
            const Bone& bone = m_bones[e];
            const int   foldAxis = foldAxisOf(static_cast<int>(e));
            if (foldAxis < 0 || static_cast<int>(e) == root) {
                continue; // an ELBOW: a fold joint ...
            }
            bool leg = false; // ... with no foot below it (that is a knee)
            for (std::size_t i = 0; i < n && !leg; ++i) {
                leg = rig.isFootClass(static_cast<int>(i)) && boneUnder(static_cast<int>(i), static_cast<int>(e));
            }
            int socket = bone.parent;
            while (socket >= 0 && socket != root && freeChannelCount(socket) < 2) {
                socket = m_bones[static_cast<std::size_t>(socket)].parent; // (past the upper arm's twist bone)
            }
            if (leg || socket < 0 || socket == root || m_bones[static_cast<std::size_t>(socket)].parent < 0) {
                continue;
            }
            // Not the limb the user has hold of (the solve's effector, or the bone that was grabbed).
            if (boneUnder(rig.dragEffector(), socket) || (m_selectedBone >= 0 && boneUnder(m_selectedBone, socket))) {
                continue;
            }
            // (... nor a limb THE GROUP has hold of: a second hand pulled up with the first is no
            // hanging arm, and turned back toward hanging every tick it would fight its own row.)
            bool grouped = false;
            for (const int sec : rig.secondaryEffectors()) {
                grouped = grouped || boneUnder(sec, socket);
            }
            if (grouped) {
                continue;
            }
            bool landedNow = false; // (a later tick: the hand is a live contact now — see above)
            if (!firstFind) {
                bool listed = false;
                for (const HangArm& have : m_jsHangArms) {
                    listed = listed || have.socket == socket;
                }
                if (listed) {
                    continue;
                }
                for (std::size_t p = 0; p < rig.pins().size(); ++p) {
                    const int node = rig.pins()[p].node;
                    landedNow = landedNow || (rig.pinIsLive(p) && node != static_cast<int>(e) && boneUnder(node, static_cast<int>(e)));
                }
                if (!landedNow) {
                    continue;
                }
            }
            const glm::vec3 offset = m_jsStartPos[e] - m_jsStartPos[static_cast<std::size_t>(socket)];
            const float     length = glm::length(offset);
            static const bool kFindTrace = std::getenv("IK_ARM_HANG_TRACE") != nullptr;
            if (kFindTrace) {
                std::fprintf(stderr, "[hang] candidate %s (socket %s): down share %.2f, elbow %.1f deg",
                             m_boneNames[e].c_str(), m_boneNames[static_cast<std::size_t>(socket)].c_str(),
                             length > 1.0e-4f ? -offset.y / length : 0.0f, m_ikStartEuler[e][foldAxis]);
                const glm::vec3 live = glm::vec3(m_poseGlobal[e][3]) - glm::vec3(m_poseGlobal[static_cast<std::size_t>(socket)][3]);
                std::fprintf(stderr, " | start offset (%.3f %.3f %.3f) live offset (%.3f %.3f %.3f)", offset.x, offset.y, offset.z,
                             live.x, live.y, live.z);
                std::fputc(10, stderr);
            }
            if (length < kArmHangMinLength * rig.sizeScale()) {
                continue; // a FINGER's middle joint is a fold joint too (found hanging, and turned, until the trace showed them)
            }
            // (A figure SEATED ON THE FLOOR whose trunk is dragged: EVERY idle arm is the hang's,
            // bent or not — it is carried clear of the floor as she is rolled onto her back, where
            // riding it would go through it, or, held as a contact, prop her up: IkRig::
            // seedPoseContacts.)
            const bool seatedTrunkDrag =
                m_jsFloorSeatStart && rig.dragEffector() != root && rig.effectorIsTrunk() &&
                m_ikBindPos[static_cast<std::size_t>(rig.dragEffector())].y > m_ikBindPos[static_cast<std::size_t>(root)].y + 0.05f * rig.sizeScale();
            // (... and an arm whose hand is ON THE FLOOR at the press — a contact, the solve's until
            // it lifts — hangs once it does, whatever its elbow reads: on all fours the oldest
            // generation's hands stand on their fingertips close together under the chest, the
            // elbows bent 45 degrees, and read as POSED its arms rode the trunk up off all fours
            // and ended 46 degrees from plumb. The bend test is for a standing start, where an arm
            // bent past kArmHangElbowDeg is a hand on a hip.)
            bool landedAtPress = landedNow;
            for (const IkEffector& pin : rig.pins()) {
                landedAtPress = landedAtPress || (boneUnder(pin.node, static_cast<int>(e)) && pin.node != static_cast<int>(e));
            }
            // (... READ OFF THE POSE as well as the pins, 2026-10-29: a hand the previous drag left a
            // hair above the rig's seed band is no pin at the press and a live contact two ticks in,
            // and when a prone descent released it as a trailing hand NOTHING had the arm — not a
            // hang arm, it rode where the release left it, straight and horizontal, the wrist flexed
            // and the fingertips on the floor: a strut under the shoulder, the heavy male's hips 10cm
            // above his belly. A hand on the floor at the press is a hand on the floor.)
            ensureHandMaps();
            int handAtPress = -1;
            for (std::size_t i = e + 1; i < n && handAtPress < 0 && m_jsWristOf.size() == n; ++i) {
                if (m_jsWristOf[i] == static_cast<int>(i) && boneUnder(static_cast<int>(i), static_cast<int>(e))) {
                    handAtPress = static_cast<int>(i);
                }
            }
            landedAtPress = landedAtPress || (handAtPress >= 0 && rig.restsOnFloor(handAtPress, m_jsStartPos));
            static const bool kBentLandedRides = std::getenv("IK_ARM_HANG_BENT_LANDED_RIDES") != nullptr; // A/B probe
            // (... whatever its DIRECTION reads, too, 2026-10-27: a prone figure's arms stand on
            // their hands BEHIND the shoulders, the upper arms pointing back along the body — outside
            // the cone, so read as posed — and when the hips went up and the hands let go the arms
            // rode where the release left them, the hands hovering a palm off the floor ahead of
            // her. A hand on the floor is a support, not a pose; once it lifts the arm hangs.)
            bool nearBind = false;
            {
                const glm::vec3 bindOffset = m_ikBindPos[e] - m_ikBindPos[static_cast<std::size_t>(socket)];
                const float     bindLength = glm::length(bindOffset);
                if (bindLength > 1.0e-4f && length > 1.0e-4f) {
                    const float bindElev = glm::degrees(std::acos(std::max(-1.0f, std::min(1.0f, -bindOffset.y / bindLength))));
                    const float liveElev = glm::degrees(std::acos(std::max(-1.0f, std::min(1.0f, -offset.y / length))));
                    nearBind = bindElev <= kArmHangBindMaxDeg && std::abs(liveElev - bindElev) <= kArmHangBindSlackDeg;
                }
            }
            const bool hungByPose = !((offset.y / length > -kArmHangCone && !nearBind) || std::abs(m_ikStartEuler[e][foldAxis]) > kArmHangElbowDeg);
            const bool hung = hungByPose || (!kBentLandedRides && landedAtPress);
            if (kFindTrace) {
                std::fprintf(stderr, "[hang] candidate %s: hungByPose=%d landedAtPress=%d (hand %s, rests on the floor at the press: %d, landed now: %d) -> %s",
                             m_boneNames[e].c_str(), hungByPose ? 1 : 0, landedAtPress ? 1 : 0,
                             handAtPress >= 0 ? m_boneNames[static_cast<std::size_t>(handAtPress)].c_str() : "-",
                             handAtPress >= 0 && rig.restsOnFloor(handAtPress, m_jsStartPos) ? 1 : 0, landedNow ? 1 : 0,
                             seatedTrunkDrag || hung ? "hangs" : "rides");
                std::fputc(10, stderr);
            }
            if (!seatedTrunkDrag && !hung) {
                m_hangBaseValid[static_cast<std::size_t>(socket)] = 0; // (posed: its swing base is forgotten)
                if (static_cast<std::size_t>(socket) < m_hangElbowValid.size()) {
                    m_hangElbowValid[static_cast<std::size_t>(socket)] = 0; // (... and its elbow's)
                }
                continue; // not hanging: a raised arm, a hand on a hip — posed, and it rides
            }
            HangArm arm{socket, static_cast<int>(e), offset / length, offset / length};
            arm.foldAxis = foldAxis;
            arm.handLandedAtPress = landedAtPress;
            arm.depthAtPress = rig.bodyVolumes().empty() ? 0.0f : fkVolumeDepth(socket);
            arm.foldSign = -bone.rotMin[foldAxis] > bone.rotMax[foldAxis] ? -1.0f : 1.0f;
            {
                // IDLE ELBOWS SOFTEN: the elbow is OURS when the press finds it at the bind (every
                // figure imports with straight arms), or where the hang itself left it at the last
                // drag (m_hangElbowDeg: the idle bend, the stride's, the landing's). An elbow the
                // user turned, or the solve left bent, is a posed elbow and keeps its bend.
                const std::size_t si = static_cast<std::size_t>(socket);
                const float       atPress = e < m_ikStartEuler.size() ? m_ikStartEuler[e][foldAxis] : 1.0e9f;
                arm.elbowOurs = std::abs(atPress) < kArmIdleElbowUnposedDeg ||
                                (si < m_hangElbowValid.size() && m_hangElbowValid[si] != 0 &&
                                 std::abs(atPress - m_hangElbowDeg[si]) < kArmIdleElbowUnposedDeg);
            }
            ensureHandMaps();
            for (std::size_t i = e + 1; i < n && arm.hand < 0 && m_jsWristOf.size() == n; ++i) {
                if (m_jsWristOf[i] == static_cast<int>(i) && boneUnder(static_cast<int>(i), static_cast<int>(e))) {
                    arm.hand = static_cast<int>(i);
                }
            }
            if (!hungByPose) {
                // (Found lying beside a figure on her back, say, or standing on its hand: what it
                // wants is to hang by her side — down, a little out — as soon as the floor lets it.
                // Kept as found, a figure sat up from her back held both arms straight out in front
                // of her.)
                glm::vec3 out = m_jsStartPos[static_cast<std::size_t>(socket)] - m_jsStartPos[static_cast<std::size_t>(root)];
                const glm::vec3 lateral(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
                out = lateral * (glm::dot(out, lateral) < 0.0f ? -1.0f : 1.0f);
                arm.wants = glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f) + 0.25f * out);
            }
            {
                // THE STRIDE SWING's BASE for this arm — the un-swung hang the swing is applied to:
                // remembered from the last drag that hung it (m_hangBase, carried to this press's
                // heading), else the found direction LESS the swing the stance it stands in would
                // give (a loaded stride pose with plumb arms keeps them plumb until the stance
                // changes; a standing figure's base is the found hang itself).
                const glm::vec3 lateralStart(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
                const glm::vec3 foreStart = glm::normalize(glm::cross(lateralStart, glm::vec3(0.0f, 1.0f, 0.0f)));
                const glm::vec3 side = m_jsStartPos[static_cast<std::size_t>(socket)] - m_jsStartPos[static_cast<std::size_t>(root)];
                const float     armSide = glm::dot(side, lateralStart) < 0.0f ? -1.0f : 1.0f;
                float           splitPress = 0.0f;
                arm.hasSwing = hangStanceSplit(armSide, foreStart, splitPress);
                const std::size_t si = static_cast<std::size_t>(socket);
                if (!m_hangBaseValid[si]) {
                    // (No memory: the found pose is taken to carry its stance's swing.)
                    m_hangSwingDeg[si] = arm.hasSwing ? hangSwingDegOf(splitPress) : 0.0f;
                    m_hangBaseValid[si] = 1;
                }
                if (hung && std::abs(m_hangSwingDeg[si]) > 1.0e-3f) {
                    // (The found direction tilted BACK by the swing it carries: minus a forward
                    // swing is a turn about plus the lateral.)
                    arm.wants = glm::normalize(glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(m_hangSwingDeg[si]), lateralStart)) * arm.wants);
                }
            }
            m_jsHangArms.push_back(arm);
        }
    }
}

/// THE STRIDE SWING this tick, per hanging arm (degrees, + forward): see the body.
std::vector<float> Armature::hangArmSwings() const {
    const IkRig&      rig = *m_ikRig;
    const std::size_t n = m_bones.size();
    const int         root = rig.rootNode();
    // THE STRIDE SWING this tick, per arm: the swing the stance gives it now (degrees, + forward),
    // absolute on the arm's remembered base. The SPLIT is read along the HIPS' heading now — the
    // stance is the legs': read along the CHEST's, a chest that turned as one hand reached the
    // floor made two symmetric feet a split of centimetres, and the other arm swung by a fraction
    // of a degree through a reach that has no stance in it (the all-fours gallery moved 0.8% of
    // its pixels on a route whose feet never moved). IK_ARM_HANG_NO_SWING is the A/B;
    // IK_ARM_HANG_TRACE prints each arm's split and swing per tick.
    static const bool kNoSwing = std::getenv("IK_ARM_HANG_NO_SWING") != nullptr; // A/B probe
    static const bool kSwingTrace = std::getenv("IK_ARM_HANG_TRACE") != nullptr;
    std::vector<float> swings(m_jsHangArms.size(), 0.0f);
    {
        const glm::vec3 lateralStart(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
        const glm::mat3 rootTurned = glm::mat3(m_poseGlobal[static_cast<std::size_t>(root)]) *
                                     glm::transpose(m_jsStartRot[static_cast<std::size_t>(root)]);
        glm::vec3 hipsLateral = rootTurned * lateralStart;
        hipsLateral.y = 0.0f;
        const bool      hipsLevel = glm::length(hipsLateral) > 0.2f;
        const glm::vec3 hipsFore = hipsLevel ? glm::normalize(glm::cross(glm::normalize(hipsLateral), glm::vec3(0.0f, 1.0f, 0.0f))) : glm::vec3(0.0f);
        for (std::size_t ai = 0; ai < m_jsHangArms.size() && !kNoSwing && hipsLevel; ++ai) {
            const HangArm&    arm = m_jsHangArms[ai];
            const std::size_t s = static_cast<std::size_t>(arm.socket);
            if (!arm.hasSwing || hangArmHeld(arm)) {
                continue;
            }
            const glm::vec3 side = m_jsStartPos[s] - m_jsStartPos[static_cast<std::size_t>(root)];
            float           splitNow = 0.0f;
            if (hangStanceSplit(glm::dot(side, lateralStart) < 0.0f ? -1.0f : 1.0f, hipsFore, splitNow)) {
                swings[ai] = hangSwingDegOf(splitNow);
            }
            if (kSwingTrace) {
                std::fprintf(stderr, "[hang-swing] %s: split %.4f m -> swing %.3f deg (carried %.3f)\n", m_boneNames[s].c_str(), splitNow,
                             swings[ai], s < m_hangSwingDeg.size() ? m_hangSwingDeg[s] : 0.0f);
            }
        }
    }
    return swings;
}

/// THE ELBOW SOFTENS as the hand nears the floor, and bends with the forward swing. True if an elbow turned.
bool Armature::softenHangElbows(const std::vector<float>& swings) {
    const IkRig&      rig = *m_ikRig;
    const std::size_t n = m_bones.size();
    bool changed = false;
    // THE ELBOW SOFTENS as the hand nears the floor (kArmLandSoftenDeg): by the fingertip's height
    // over its clearance, eased. Only ever MORE flexion than the drag found, and given back as the
    // hand comes away again. (... and the FORWARD-swung arm's elbow flexes with its swing, up to
    // kArmSwingElbowDeg: a walker's leading arm is bent, the trailing one straight.)
    static const bool kNoSoften = std::getenv("IK_ARM_HANG_NO_SOFTEN") != nullptr; // A/B probe
    // IDLE ELBOWS SOFTEN (kArmIdleElbowDeg, 2026-11-02; IK_ARM_HANG_NO_IDLE_ELBOW): an arm the hang
    // OWNS (HangArm::elbowOurs) bends kArmIdleElbowDeg at the elbow, at kArmIdleElbowStepDeg a
    // tick, the way an idle hand's fingers curl -- a relaxed arm hangs with a soft elbow, and every
    // hanging arm in every gallery shot was dead straight, a mannequin's. The landing's soften and
    // the stride's bend still win where they ask MORE; and for an owned elbow the flexion the drag
    // found is NO floor, so the stride's 18 degrees relax back to the idle bend as the stance
    // closes (a posed elbow keeps its bend, as ever). Balance is blind to it as to the whole hang.
    static const bool kNoIdleElbow = std::getenv("IK_ARM_HANG_NO_IDLE_ELBOW") != nullptr; // A/B probe
    struct ElbowSoft {
        const HangArm* arm;
        std::size_t    e;           // the elbow bone
        float          before;      // its fold channel as the solve left it
        float          delta;       // the soften's whole ask (channel units)
        float          fraction;    // how much of it the stop lets through
        float          depthBefore; // how deep the forearm stood in the volumes before
    };
    std::vector<ElbowSoft> softs;
    for (const HangArm& arm : m_jsHangArms) {
        if (arm.hand < 0 || hangArmHeld(arm)) {
            continue;
        }
        const std::size_t ai = static_cast<std::size_t>(&arm - m_jsHangArms.data());
        const float       swingShare = ai < swings.size() ? glm::clamp(swings[ai] / kArmSwingMaxDeg, 0.0f, 1.0f) : 0.0f;
        const float       swingWanted = kArmSwingElbowDeg * swingShare;
        const float       idleWanted = (kNoIdleElbow || !arm.elbowOurs) ? 0.0f : kArmIdleElbowDeg;
        if (kNoSoften && swingWanted <= 0.0f && idleWanted <= 0.0f) {
            continue;
        }
        float lowest = 1.0e9f;
        for (std::size_t i = static_cast<std::size_t>(arm.hand); i < n; ++i) {
            if (boneUnder(static_cast<int>(i), arm.hand)) {
                lowest = std::min(lowest, m_poseGlobal[i][3].y + m_transform[3][1] - rig.floorClearance(static_cast<int>(i)));
            }
        }
        const float       scale = rig.sizeScale();
        const float       landWanted = kNoSoften ? 0.0f : kArmLandSoftenDeg * (1.0f - glm::smoothstep(0.02f * scale, kArmLandSoftenFrom * scale, lowest));
        const float       wanted = std::max({landWanted, swingWanted, idleWanted});
        const std::size_t e = static_cast<std::size_t>(arm.elbow);
        // (The flexion the drag FOUND is the floor of the goal for an arm that hung at the press; an
        // arm that stood on its hand — handLandedAtPress — found its bend on the floor, a strut's,
        // and once the hand lifts it hangs straight: folded back to the strut's 71 degrees at the
        // soften's pace, a figure kneeling up off a sphinx hung her forearms up like paws.)
        // (... and NO floor at all for an elbow the hang owns: what the hang bent, the hang may
        // straighten -- the stride's bend relaxes back to the idle one as the stance closes.)
        const float       found = (arm.handLandedAtPress || arm.elbowOurs) ? 0.0f : m_ikStartEuler[e][arm.foldAxis] * arm.foldSign; // (its flexion as the drag found it)
        const float       now = m_boneEuler[e][arm.foldAxis] * arm.foldSign;
        const float       goal = std::max(found, wanted);
        // (The idle bend comes in at its own gentle pace; the landing's and the stride's bends come at
        // theirs, and so does every STRAIGHTENING -- a released strut arm straightens toward the idle
        // bend as fast as it straightened to nothing before: at the gentle pace two hands released
        // a few ticks apart were still 40mm lopsided at the end of the third generation's rise.)
        const float       pace = (goal < now || wanted > idleWanted) ? kArmLandSoftenStepDeg : kArmIdleElbowStepDeg;
        const float       next = now + glm::clamp(goal - now, -pace, pace);
        if (std::abs(next - now) > 1.0e-3f) {
            softs.push_back({&arm, e, m_boneEuler[e][arm.foldAxis], next * arm.foldSign - m_boneEuler[e][arm.foldAxis], 1.0f, 0.0f});
        }
    }
    // THE STOP (2026-11-02): the soften is FK of the forearm and stops at the body volumes as every FK
    // turn does (the hang's turn, a Transform dial) -- ORDER-INDEPENDENTLY, the hang stop's way: each
    // arm's fraction is measured against the pose as the solve left it (the arm put back before the
    // next is measured), all are applied, and two forearms that have turned INTO each other give way
    // together. Unstopped, the released arms of the third generation's all-fours rise bent their
    // forearms into each other under the chest, and the hang's own stop then blocked one arm's
    // swing and not the other's: 40mm lopsided at the hands (the symmetry gate's catch).
    const bool volumes = !rig.bodyVolumes().empty();
    const auto softTo = [&](const ElbowSoft& sft, float fraction) {
        m_boneEuler[sft.e][sft.arm->foldAxis] = sft.before + sft.delta * fraction;
        applyBoneEuler(sft.arm->elbow);
    };
    // (The bend may come no nearer than kArmIdleElbowClear of touching -- and no nearer than it stands,
    // where that is nearer already. Allowed to TOUCH, the third generation's released forearms met
    // under the chest after its all-fours rise, each hand inside the other forearm's capsule, and the
    // hang's own stop then jittered in a two-cycle at the contact whose outward turns resolved one
    // arm out and the other not: 40mm lopsided at the hands. A cosmetic bend makes no contact.)
    const auto allowOf = [](const ElbowSoft& sft) { return std::max(sft.depthBefore, -kArmIdleElbowClear); };
    for (ElbowSoft& sft : softs) {
        if (!volumes) {
            continue;
        }
        sft.depthBefore = fkVolumeDepth(sft.arm->elbow);
        softTo(sft, 1.0f);
        if (fkVolumeDepth(sft.arm->elbow) > allowOf(sft)) {
            float lo = 0.0f;
            float hi = 1.0f;
            for (int k = 0; k < 8; ++k) {
                const float mid = 0.5f * (lo + hi);
                softTo(sft, mid);
                (fkVolumeDepth(sft.arm->elbow) > allowOf(sft) ? hi : lo) = mid;
            }
            sft.fraction = lo;
        }
        softTo(sft, 0.0f); // (put back: the next arm is measured against the solve's pose)
    }
    if (!softs.empty()) {
        const auto together = [&](float share) {
            for (const ElbowSoft& sft : softs) {
                softTo(sft, sft.fraction * share);
            }
            if (!volumes) {
                return true;
            }
            for (const ElbowSoft& sft : softs) {
                if (fkVolumeDepth(sft.arm->elbow) > allowOf(sft)) {
                    return false;
                }
            }
            return true;
        };
        if (!together(1.0f) && softs.size() > 1) {
            float lo = 0.0f;
            float hi = 1.0f;
            for (int k = 0; k < 6; ++k) {
                const float mid = 0.5f * (lo + hi);
                (together(mid) ? lo : hi) = mid;
            }
            together(lo);
            for (ElbowSoft& sft : softs) {
                sft.fraction *= lo;
            }
        }
        for (const ElbowSoft& sft : softs) {
            if (std::abs(sft.delta * sft.fraction) > 1.0e-4f) {
                if (sft.e < m_jsPostStepEuler.size()) {
                    m_jsPostStepEuler[sft.e] = m_boneEuler[sft.e];
                    m_jsPostStepTurned[sft.e] = 1;
                }
                changed = true;
            }
        }
    }
    for (const HangArm& arm : m_jsHangArms) {
        if (arm.hand < 0 || hangArmHeld(arm)) {
            continue;
        }
        if (arm.elbowOurs && static_cast<std::size_t>(arm.socket) < m_hangElbowValid.size()) {
            // (Where the hang leaves an elbow it owns is what the next press reads as still ours.)
            const std::size_t e = static_cast<std::size_t>(arm.elbow);
            m_hangElbowDeg[static_cast<std::size_t>(arm.socket)] = m_boneEuler[e][arm.foldAxis];
            m_hangElbowValid[static_cast<std::size_t>(arm.socket)] = 1;
        }
    }
    return changed;
}

/// THE WRIST RELAXES once a hand that was on the floor at the press has lifted. True if one turned.
bool Armature::relaxLiftedWrists() {
    const IkRig&      rig = *m_ikRig;
    bool changed = false;
    // THE WRIST RELAXES once a hand that was on the floor at the press is unloaded or lifted: its
    // channels ease toward neutral, kWristRelaxStepDeg a tick (within the limits). Left at its
    // posture reference - the all-fours hand, bent back 80 degrees to lie flat under a vertical
    // forearm - a hand lifted off all fours by a kneel-up hung flat, held out like a paw.
    // (Once LIFTED — the pin gone or a shadow — not once its rows have faded: the fade goes to zero
    // for a tick as a planted hand bobs under a held prone, and a wrist turned 4 degrees then
    // re-planted fought its palm rows — a 224mm pop on the newest generation; and on the rig whose
    // carpals are two contacts a hand, one faded before the other and the hands relaxed lopsided.)
    static const bool kNoWristRelax = std::getenv("IK_ARM_HANG_NO_WRIST_RELAX") != nullptr; // A/B probe
    for (const HangArm& arm : m_jsHangArms) {
        if (kNoWristRelax || arm.hand < 0 || !arm.handLandedAtPress || !hangHandLifted(arm)) {
            continue;
        }
        const std::size_t h = static_cast<std::size_t>(arm.hand);
        const Bone&       bone = m_bones[h];
        glm::vec3         next = m_boneEuler[h];
        bool              moved = false;
        for (int a = 0; a < 3; ++a) {
            const float step = glm::clamp(-next[a], -kWristRelaxStepDeg, kWristRelaxStepDeg);
            if (std::abs(step) > 1.0e-3f) {
                next[a] += step;
                if (bone.rotLimited[a]) {
                    next[a] = glm::clamp(next[a], bone.rotMin[a], bone.rotMax[a]);
                }
                moved = true;
            }
        }
        if (moved) {
            m_boneEuler[h] = next;
            applyBoneEuler(arm.hand);
            if (h < m_jsPostStepEuler.size()) {
                m_jsPostStepEuler[h] = m_boneEuler[h];
                m_jsPostStepTurned[h] = 1;
            }
            changed = true;
        }
    }
    return changed;
}

/// IDLE HANDS RELAX, and a hand coming down on the floor opens. True if a finger turned.
bool Armature::relaxIdleFingers() {
    const IkRig&      rig = *m_ikRig;
    bool changed = false;
    // IDLE HANDS RELAX (2026-09-25; IK_ARM_HANG_NO_FINGER_RELAX): the fingers of an arm that HANGS
    // — nothing has hold of it, its hand is not on the floor — and whose hand is UNPOSED (every
    // finger joint's widest channel within kFingerUnposedDeg of the bind at the press: the flat,
    // spread hand every figure imports with) curl to a relaxed hand — each finger joint's widest
    // channel to kFingerRelaxShare of its flexion side, kFingerRelaxStepDeg a tick, within the
    // limits. A POSED hand — fingers the user turned, or a hand an earlier drag relaxed — is left
    // as it is; Reset Pose flattens it, and the next drag of anything relaxes it again. The
    // mannequin hands were the most visible thing left in every gallery shot once the arms hung
    // and the head stayed up: a person's idle hand is never flat with the fingers spread.
    // ... and A HAND COMING DOWN ON THE FLOOR OPENS again: a relaxed hand within kFingerOpenHeight
    // of the floor, or on it — a live contact, the dragged hand's fingertip hold — flattens at the
    // same pace (a hand put down on the floor opens onto its palm; landed on curled fingers, the
    // oldest generation's all-fours stood its wrists 113mm up where flat palms stand 90). It opens
    // as it NEARS the floor: opened only once its fingertip was held there, the fingers moved under
    // the hold and the third generation's prone popped 157mm. Only hands that are OURS — unposed at
    // the press, or at the relaxed hand an earlier drag left them with — are touched either way.
    static const bool kNoFingerRelax = std::getenv("IK_ARM_HANG_NO_FINGER_RELAX") != nullptr; // A/B probe
    for (std::size_t w = 0; !kNoFingerRelax && w < m_jsWristOf.size(); ++w) {
        if (m_jsWristOf[w] != static_cast<int>(w)) {
            continue; // a wrist only
        }
        const HangArm* hang = nullptr;
        for (const HangArm& arm : m_jsHangArms) {
            if (arm.hand == static_cast<int>(w)) {
                hang = &arm;
            }
        }
        bool onFloor = w < m_jsHandTipValid.size() && m_jsHandTipValid[w];
        {
            const std::vector<IkEffector>& pins = rig.pins();
            for (std::size_t p = 0; p < pins.size() && !onFloor; ++p) {
                onFloor = rig.pinIsLive(p) && !rig.pinIsShadow(p) && pins[p].node >= 0 &&
                          m_jsWristOf.size() > static_cast<std::size_t>(pins[p].node) &&
                          m_jsWristOf[static_cast<std::size_t>(pins[p].node)] == static_cast<int>(w);
            }
        }
        const bool hanging = hang != nullptr && !hang->landed && !hangArmHeld(*hang) && hangHandLifted(*hang);
        bool nearFloor = false;
        {
            // (The hand's lowest part over the floor: the wrist and every joint below it, each against
            // its own floor clearance, in world height — as the hang's own floor stop reads it.)
            float            lowest = 1.0e9f;
            std::vector<int> below{static_cast<int>(w)};
            while (!below.empty()) {
                const int b = below.back();
                below.pop_back();
                lowest = std::min(lowest, m_poseGlobal[static_cast<std::size_t>(b)][3].y + m_transform[3][1] - rig.floorClearance(b));
                below.insert(below.end(), m_children[static_cast<std::size_t>(b)].begin(), m_children[static_cast<std::size_t>(b)].end());
            }
            nearFloor = lowest < kFingerOpenHeight * rig.sizeScale();
        }
        const bool opens = onFloor || nearFloor;
        if (!opens && !hanging) {
            continue;
        }
        // The fingers: every descendant of the hand with a channel that spans kFingerRelaxMinRangeDeg,
        // each with its widest channel (the bend) and the sign of that channel's wide side (flexion).
        struct FingerJoint {
            std::size_t bone;
            int         axis;
            float       relaxed;
        };
        std::vector<FingerJoint> joints;
        std::vector<int>         stack(m_children[w].begin(), m_children[w].end());
        bool ours = true;
        while (!stack.empty()) {
            const int b = stack.back();
            stack.pop_back();
            const std::size_t bi = static_cast<std::size_t>(b);
            stack.insert(stack.end(), m_children[bi].begin(), m_children[bi].end());
            const Bone& fb = m_bones[bi];
            int   widest = -1;
            float widestRange = 0.0f;
            for (int a = 0; a < 3; ++a) {
                const float range = fb.rotLimited[a] ? fb.rotMax[a] - fb.rotMin[a] : 0.0f;
                if (range > widestRange) {
                    widestRange = range;
                    widest = a;
                }
            }
            if (widest < 0 || widestRange < kFingerRelaxMinRangeDeg) {
                continue; // a carpal, a locked joint
            }
            const float wide = fb.rotMax[widest] > -fb.rotMin[widest] ? fb.rotMax[widest] : fb.rotMin[widest];
            const float relaxed = wide * kFingerRelaxShare;
            joints.push_back({bi, widest, relaxed});
            const float atPress = bi < m_ikStartEuler.size() ? m_ikStartEuler[bi][widest] : 1.0e9f;
            ours = ours && (std::abs(atPress) < kFingerUnposedDeg || std::abs(atPress - relaxed) < 3.0f * kFingerUnposedDeg);
        }
        if (!ours) {
            continue;
        }
        std::sort(joints.begin(), joints.end(), [](const FingerJoint& x, const FingerJoint& y) { return x.bone < y.bone; }); // parents first
        for (const FingerJoint& j : joints) {
            const Bone& fb = m_bones[j.bone];
            float&      value = m_boneEuler[j.bone][j.axis];
            const float target = opens ? 0.0f : j.relaxed;
            const float step = glm::clamp(target - value, -kFingerRelaxStepDeg, kFingerRelaxStepDeg);
            if (std::abs(step) <= 1.0e-3f) {
                continue;
            }
            value = glm::clamp(value + step, fb.rotMin[j.axis], fb.rotMax[j.axis]);
            applyBoneEuler(static_cast<int>(j.bone));
            if (j.bone < m_jsPostStepEuler.size()) {
                m_jsPostStepEuler[j.bone] = m_boneEuler[j.bone];
                m_jsPostStepTurned[j.bone] = 1;
            }
            changed = true;
        }
    }
    return changed;
}

/// Every hanging arm's turn, FOUND on the pose as the solve left it (and only then stopped and
/// applied: stopHangTurns, applyHangTurns): where each arm wants to point this tick — carried round
/// with the trunk's heading, plumb under a tilted trunk, never asked further into the body, kept off
/// the floor while the seat rolls or the hips go down, tilted by the stride swing — and the turn.
void Armature::findHangTurns(const std::vector<float>& swings, std::vector<HangTurn>& turns) {
    const IkRig&      rig = *m_ikRig;
    const std::size_t n = m_bones.size();
    const int         root = rig.rootNode();
    for (const HangArm& arm : m_jsHangArms) {
        const std::size_t s = static_cast<std::size_t>(arm.socket);
        const std::size_t par = static_cast<std::size_t>(m_bones[s].parent);
        if (hangArmHeld(arm)) {
            continue; // a pin or a floor contact in the arm: the solve has it now
        }
        // The heading the trunk has turned by since the press: the parent's lateral axis, levelled.
        const glm::mat3 parentNow(m_poseGlobal[par]);
        const glm::mat3 turned = parentNow * glm::transpose(m_jsStartRot[par]);
        glm::vec3       lateral = turned * glm::vec3(1.0f, 0.0f, 0.0f);
        lateral.y = 0.0f;
        glm::vec3 wanted = arm.wants;
        if (glm::length(lateral) > 0.2f) {
            lateral = glm::normalize(lateral);
            const float yaw = std::atan2(-lateral.z, lateral.x);
            wanted = glm::mat3(glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f))) * arm.wants;
        }
        // ... and under a trunk that has TILTED it hangs PLUMB: straight down, a little out. The
        // elevation it was found at is the rest pose's - an A-pose arm stands 46 degrees out from
        // the vertical - and kept through a deep fold it took a figure onto all fours with her
        // hands a metre apart, fingers pointing sideways. By the tilt the trunk has GAINED since
        // the press (none of it under 25 degrees, all from 70: a bow keeps the arms as they were,
        // nothing pops at the press, and a drag brought home brings them home).
        static const bool kNoPlumb = std::getenv("IK_ARM_HANG_NO_PLUMB") != nullptr; // A/B probe
        const glm::vec3 lateralStart(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
        const glm::vec3 side = m_jsStartPos[s] - m_jsStartPos[static_cast<std::size_t>(root)];
        const glm::vec3 out = glm::length(lateral) > 0.2f ? lateral * (glm::dot(side, lateralStart) < 0.0f ? -1.0f : 1.0f) : glm::vec3(0.0f);
        if (!kNoPlumb && glm::length(lateral) > 0.2f) {
            const glm::vec3 upNow = turned * glm::vec3(0.0f, 1.0f, 0.0f);
            const float     tilted = glm::degrees(std::acos(glm::clamp(upNow.y, -1.0f, 1.0f)));
            // (THE DANGLE, 2026-11-02: a body hanging from the held hand has its free arm plumb too —
            // kept at the A-pose's 46 degrees it stood out from the hanging body like a signpost.)
            const float     plumbShare = std::max(glm::smoothstep(25.0f, 70.0f, tilted), m_jsDangle);
            if (plumbShare > 0.0f) {
                const glm::vec3 plumb = glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f) + 0.12f * out);
                wanted = glm::normalize(glm::mix(wanted, plumb, plumbShare));
            }
        }
        // An arm RESTING AGAINST THE BODY is never asked further IN. The plumb's "a little out" is
        // less out than an arm stands on all fours (the collars shrugged forward, the mid-upper-arm
        // point 2mm inside the chest's volume), and a turn is ONE rotation: asked in toward the
        // chest as it was asked down, the stop below blocked the whole of it, and a figure got up
        // off all fours by the chest with both arms held straight out in front of her, tick after
        // tick (IK_FK_DEPTH_TRACE: the twist joint 2.2mm inside the chest, 2.4 after any step). Its
        // outward share is kept as it stands; the rest of the direction is the hang's.
        static const bool kAsksIn = std::getenv("IK_ARM_HANG_ASKS_IN") != nullptr; // A/B probe
        if (!kAsksIn && glm::length(out) > 0.5f && !rig.bodyVolumes().empty() && fkVolumeDepth(arm.socket) > 0.5e-3f) {
            const glm::vec3 nowDir = glm::normalize(glm::vec3(m_poseGlobal[static_cast<std::size_t>(arm.elbow)][3]) - glm::vec3(m_poseGlobal[s][3]));
            const float     outNow = glm::dot(nowDir, out);
            const float     outWanted = glm::dot(wanted, out);
            if (outNow > outWanted && outNow < 0.999f) {
                const glm::vec3 rest = wanted - out * outWanted;
                const float     restLen = glm::length(rest);
                const float     restWanted = std::sqrt(std::max(0.0f, 1.0f - outNow * outNow));
                wanted = glm::normalize(out * outNow + (restLen > 1.0e-5f ? rest * (restWanted / restLen) : glm::vec3(0.0f, -restWanted, 0.0f)));
            }
        }
        // While THE SEAT ROLLS (solveIk: a figure sitting on the floor, let down onto her back or
        // brought back up) the arm keeps OFF THE FLOOR: its direction is turned from the hang
        // toward the one it would RIDE the chest at — which, as she goes down, is along the floor
        // toward her feet — by just as much as clears it. Left to hang, the hands came down on the
        // floor halfway, were planted there as contacts, and the arms — the solve's now, a planted
        // hand under a shoulder on its way to the floor — were folded one way and the other tick
        // after tick (solves that ran out of iterations, 12-16cm at the fingertips). She is laid
        // down with her arms beside her. (A hand the user PUT on the floor is a contact from the
        // press, and props her as it did.)
        static const bool kNoHangFloor = std::getenv("IK_NO_ARM_HANG_FLOOR") != nullptr; // A/B probe
        // ... AND WHILE THE HIPS ARE TAKEN DOWN (2026-10-27; IK_ARM_HANG_FLOOR_SEAT_ONLY): a figure
        // sat down onto the floor by the hips hangs her hands beside her hips, and whether their
        // fingertips come within the contact band of the floor was a knife edge — with the
        // rotation order composed the format's way the third generation's hands touched down a
        // centimetre lower in its floor sit, were planted as live contacts, and with the hips
        // still going down the trunk folded forward between them, 38 degrees; the sit-up that
        // followed read a trunk 51 degrees from upright and SAT her up (74cm short) where it had
        // stood her. A person sitting down keeps her hands off the floor, or rests them on her
        // thighs — the hang turns them clear, as it does while the seat rolls.
        static const bool kFloorSeatOnly = std::getenv("IK_ARM_HANG_FLOOR_SEAT_ONLY") != nullptr; // A/B probe: the seat roll alone
        static const bool kFloorAnyDrag = std::getenv("IK_ARM_HANG_FLOOR_ANY_DRAG") != nullptr;  // A/B probe: under every drag (tried 2026-10-29)
        const bool hipsDown = !kFloorSeatOnly && rig.dragEffector() == rig.rootNode() && m_jsTargetDown > 0.0f;
        // (Under the seat roll and a pelvis drag going down — NOT every drag: made unconditional for
        // an hour on 2026-10-29, it kept the hands of a figure going onto all fours by the CHEST
        // three centimetres off the floor they were coming down to land on. An arm hanging from a
        // prone trunk under a hand drag rests on the floor instead, which the stop's floor rule —
        // never deeper than found — keeps from ratcheting the body up.)
        if ((kFloorAnyDrag || m_jsSeatRolls || hipsDown) && !kNoHangFloor) {
            const glm::vec3 ridesWorld = glm::normalize(parentNow * glm::transpose(m_jsStartRot[par]) * arm.direction);
            const float     floorAt = -m_transform[3][1];
            const float     margin = kArmHangFloorMargin * rig.sizeScale();
            // (... AND THE VOLUMES, 2026-10-29: a direction that clears the floor but lies inside the
            // hip's capsule is one the stop blocks outright — the lying body's arm, left where the
            // sinking trunk took it, propped the hips on its fingertips' floor rows.)
            const float volumeBefore = rig.bodyVolumes().empty() ? 0.0f : fkVolumeDepth(arm.socket);
            const float volumeAllow = std::max(volumeBefore, kArmHangSettle * rig.sizeScale());
            const auto  clears = [&](const glm::vec3& direction) {
                // (THE TURN ITSELF — through the shoulder's channels, hangTurnFor — measured as the stop
                // measures it (hangTurnFloorDepth), 2026-10-29. Carried RIGIDLY from where it points, as
                // this read it until then, the arm differs from the real turn by a twist about its own
                // axis: a thumb the rigid reading cleared by the margin stood 5mm into the floor by the
                // turn, the stop refused every step toward the direction the avoid had chosen, and the
                // arm never left the floor — the heavy male's released arms rested on their thumbs,
                // which propped his chest. ITERATED to the direction in steps of ten degrees: hangTurnFor
                // is ONE Gauss-Newton step, right for the hang's four degrees a tick and a linear guess
                // for a right angle — taken whole, the guess stood the third generation's hanging arms
                // through the floor as she lay down, 294mm at a fingertip.)
                const glm::vec3 before = m_boneEuler[s];
                HangTurn        step;
                for (int k = 0; k < 24 && hangTurnFor(arm, direction, step, 10.0f); ++k) {
                    hangTurnTo(step, 1.0f);
                }
                const HangTurn probeTurn{&arm, s, m_boneEuler[s], glm::vec3(0.0f), direction};
                const float    depth = hangTurnFloorDepth(probeTurn);
                const float    volume = rig.bodyVolumes().empty() ? 0.0f : fkVolumeDepth(arm.socket);
                m_boneEuler[s] = before;
                applyBoneEuler(arm.socket);
                return depth < -margin && volume <= volumeAllow;
            };
            // (The end is ALWAYS of the trunk-axis family, 2026-10-29 — the ride direction taken when
            // it cleared flipped the end by 27 degrees the tick it did, as a lying trunk settled: the
            // ride direction of a supine trunk IS along the trunk toward the feet, and of a seated
            // one straight down — the family holds them both, continuously.)
            glm::vec3 end = ridesWorld;
            {
                // ALONG THE TRUNK, OUTSIDE THE BODY (2026-10-29): the trunk's down axis as posed — the
                // chest top's -up: down for a seated trunk, backward-horizontal for a prone one — read
                // as the heading's backward horizontal until this day, which swung a seated figure's
                // arms up behind her; turned OUT by the least share that clears the floor and the
                // volumes (0.12 out was inside a lying body's hip capsule, and the stop blocked it).
                const int       chestTop = rig.neckBase() >= 0 ? m_bones[static_cast<std::size_t>(rig.neckBase())].parent : -1;
                const glm::mat3 trunkRot = chestTop >= 0 ? glm::mat3(m_poseGlobal[static_cast<std::size_t>(chestTop)]) : parentNow;
                glm::vec3       alongBody = trunkRot * glm::vec3(0.0f, -1.0f, 0.0f);
                if (glm::length(alongBody) < 1.0e-3f) {
                    alongBody = glm::vec3(end.x, 0.0f, end.z);
                }
                alongBody = glm::normalize(alongBody);
                // (The share is BISECTED to the boundary, as the arc is: chosen in steps of 0.12 it
                // flipped from 0.36 to 0.12 as a lying body settled, and the arm swung in 27 degrees
                // at the hang's pace through the lay-down's hold — 46mm a tick on two rigs.)
                const auto lieAt = [&](float share) { return alongBody + share * out; };
                // (AS FAR OUT AS THE ARM HANGS, at least — the hang direction's own outward share, a
                // function of the stance and so history-free (an in-step clamp from the share the arm
                // HAD was tried for an hour and gained the sit-up 14mm of left/right asymmetry, each
                // arm keeping its own history). From 0.12 the arc swept an A-pose arm (0.64 out) INWARD
                // across the hip's capsule as well as forward, the first clearing angle stood a third
                // of the way along it the tick the avoid came on, and the hang turned at its full pace:
                // 57-68mm a tick at a fingertip through every recline and lay-down, 19-32 before.)
                const float hangOut = glm::clamp(glm::dot(wanted, out) / std::max(glm::dot(wanted, alongBody), 0.3f), 0.12f, 0.60f);
                float       lo = hangOut; // taken when it clears
                float       hi = -1.0f;                                          // the first scanned share that clears (-1: none)
                if (glm::length(lieAt(lo)) > 1.0e-3f && !clears(glm::normalize(lieAt(lo)))) {
                    for (float share = lo + 0.12f; share <= 0.60f + 1.0e-4f; share += 0.12f) {
                        if (glm::length(lieAt(share)) > 1.0e-3f && clears(glm::normalize(lieAt(share)))) {
                            hi = share;
                            break;
                        }
                        lo = share;
                    }
                    if (hi > 0.0f) {
                        for (int i = 0; i < 8; ++i) {
                            const float mid = 0.5f * (lo + hi);
                            if (glm::length(lieAt(mid)) > 1.0e-3f && clears(glm::normalize(lieAt(mid)))) {
                                hi = mid;
                            } else {
                                lo = mid;
                            }
                        }
                        lo = hi;
                    }
                }
                if (glm::length(lieAt(lo)) > 1.0e-3f) {
                    end = glm::normalize(lieAt(lo));
                }
            }
            const float between = std::acos(glm::clamp(glm::dot(wanted, end), -1.0f, 1.0f));
            glm::vec3   about = glm::cross(wanted, end);
            static const bool kAvoidTrace = std::getenv("IK_ARM_HANG_AVOID_TRACE") != nullptr; // the avoid's search, per arm and tick
            const bool        hangClears = clears(wanted);
            if (kAvoidTrace) {
                std::fprintf(stderr, "[hang-avoid] %s: hang (%.2f %.2f %.2f) clears %d | end (%.2f %.2f %.2f) between %.1f deg",
                             m_boneNames[s].c_str(), wanted.x, wanted.y, wanted.z, hangClears ? 1 : 0, end.x, end.y, end.z, glm::degrees(between));
                std::fputc(10, stderr);
            }
            if (between > 1.0e-3f && glm::length(about) > 1.0e-5f && !hangClears) {
                about = glm::normalize(about);
                const auto along = [&](float angle) {
                    return glm::vec3(glm::mat3(glm::rotate(glm::mat4(1.0f), angle, about)) * wanted);
                };
                // The least turn that clears: a scan of sixteen steps along the arc brackets it, and
                // THE BOUNDARY IS BISECTED (2026-10-28; IK_ARM_HANG_FLOOR_NO_BISECT): taken at the
                // first clearing step alone, the direction was quantized in steps of between/16 —
                // 4.7 degrees of a 75-degree arc — and as the chest rolled down the clearing step's
                // index changed once, the wanted direction jumped by a whole step and the hang took
                // its full 4 degrees in one tick: 46mm at a fingertip on a long-armed character, one
                // tick into a still hold (every lay-down on every rig had the jump, hidden in the
                // move's gate). Eight halvings make it 0.02 degrees: a continuous function of the
                // pose, which the arm follows in small steps.
                static const bool kNoFloorBisect = std::getenv("IK_ARM_HANG_FLOOR_NO_BISECT") != nullptr; // A/B probe
                float lo = 0.0f; // does not clear (the hang itself)
                float hi = -1.0f; // the first scanned angle that clears (-1: none, the ride direction is taken)
                for (int k = 1; k <= 16; ++k) {
                    const float angle = between * static_cast<float>(k) / 16.0f;
                    if (clears(along(angle))) {
                        hi = angle;
                        break;
                    }
                    lo = angle;
                }
                if (kAvoidTrace) {
                    std::fprintf(stderr, "[hang-avoid]   first clearing angle %.1f deg (lo %.1f)", hi < 0.0f ? -1.0f : glm::degrees(hi), glm::degrees(lo));
                    std::fputc(10, stderr);
                }
                if (hi < 0.0f) {
                    wanted = end; // (nothing on the arc clears: the end of it, as near the floor as it is)
                } else {
                    if (!kNoFloorBisect) {
                        for (int i = 0; i < 8; ++i) {
                            const float mid = 0.5f * (lo + hi);
                            if (clears(along(mid))) {
                                hi = mid;
                            } else {
                                lo = mid;
                            }
                        }
                    }
                    wanted = along(hi);
                }
            }
        }
        // THE STRIDE SWING, last: the wanted direction tilted fore or aft by the arm's swing (found
        // above) about the trunk's lateral axis — down turned toward fore is a turn about minus the
        // lateral. The un-swung target is kept on the turn: what the arm actually ends up carrying,
        // measured against it below, is what the next press undoes (m_hangSwingDeg).
        const glm::vec3   unswung = wanted;
        const std::size_t ai = static_cast<std::size_t>(&arm - m_jsHangArms.data());
        if (ai < swings.size() && std::abs(swings[ai]) > 1.0e-3f && glm::length(lateral) > 0.2f) {
            wanted = glm::normalize(glm::mat3(glm::rotate(glm::mat4(1.0f), -glm::radians(swings[ai]), lateral)) * wanted);
        }
        HangTurn turn;
        if (hangTurnFor(arm, wanted, turn)) {
            turn.out = out;
            turn.unswung = unswung;
            turn.fore = glm::length(lateral) > 0.2f ? glm::normalize(glm::cross(lateral, glm::vec3(0.0f, 1.0f, 0.0f))) : glm::vec3(0.0f);
            turn.swingAsked = ai < swings.size() ? swings[ai] : 0.0f;
            turns.push_back(turn);
        }
    }
}

void Armature::stopHangTurns(std::vector<HangTurn>& turns) {
    const IkRig&      rig = *m_ikRig;
    // THE STOP: the FK collision stop (as Armature::nudgeSelectedBone), at the body volumes and the
    // FLOOR: a turn in full, else bisected back to the largest fraction that goes no deeper than it
    // was. ORDER-INDEPENDENT: each arm's fraction is measured against a body that is otherwise as
    // the solve left it (the arm is put back before the next one is measured), then all are applied,
    // and if two arms that each cleared the other where it STOOD have turned INTO each other, they
    // give way together. Turned and stopped one after the other, the second arm was measured against
    // the first where the loop had just left it, and two arms that touch (an all-fours pose whose
    // hands meet under the chest) came down lopsided: one hand 4cm ahead of the other and planted
    // a tick sooner, on six of the eight reference rigs (found by the harness's symmetry measure).
    static const bool kHangInTurn = std::getenv("IK_ARM_HANG_IN_TURN") != nullptr; // A/B probe: the old, ordered stop
    static const bool kNoHangOut = std::getenv("IK_ARM_HANG_NO_OUT") != nullptr; // A/B probe
    for (HangTurn& t : turns) {
        t.depthBefore = hangTurnDepth(t);
        t.floorDepthBefore = hangTurnFloorDepth(t);
        hangStopFraction(t);
        // AN ARM AGAINST THE BODY HANGS WHERE THE BODY LETS IT. The settle allowance above is spent
        // in 20 degrees of swing down a chest that widens 2mm a degree, and the arm stopped there,
        // 25 degrees forward of plumb. Blocked by a volume it is inside, the arm asks for the same
        // hang turned OUTWARD — the least angle, up to kArmHangOutMost, whose turn goes (nearly)
        // through: it slides down the chest's surface. (Coarse steps of 3 degrees, taken at half
        // a turn, ratcheted the arm out to 35 degrees by the time it hung.)
        if (!kNoHangOut && t.fraction < kArmHangOutThrough && t.depthBefore > 0.5e-3f && glm::length(t.out) > 0.5f) {
            // (A BISECTION on the angle, not steps of it: mirror arms must get mirror answers, and in
            // steps of 0.6 degrees one arm took a step the other did not — an un-bow ended 12mm
            // lopsided at the hands on one rig.)
            const auto outwardTurn = [&](float angle, HangTurn& retry) {
                if (!hangTurnFor(*t.arm, glm::normalize(t.wanted + std::tan(angle) * t.out), retry)) {
                    return false;
                }
                retry.out = t.out;
                retry.unswung = t.unswung;
                retry.fore = t.fore;
                retry.swingAsked = t.swingAsked;
                retry.depthBefore = t.depthBefore;
                retry.floorDepthBefore = t.floorDepthBefore;
                hangStopFraction(retry);
                return true;
            };
            HangTurn best = t;
            HangTurn atMost;
            if (outwardTurn(kArmHangOutMost, atMost) && atMost.fraction >= kArmHangOutThrough) {
                float lo = 0.0f;
                float hi = kArmHangOutMost;
                best = atMost;
                for (int k = 0; k < kArmHangOutBisections; ++k) {
                    const float mid = 0.5f * (lo + hi);
                    HangTurn        retry;
                    if (outwardTurn(mid, retry) && retry.fraction >= kArmHangOutThrough) {
                        hi = mid;
                        best = retry;
                    } else {
                        lo = mid;
                    }
                }
            } else if (atMost.arm != nullptr && atMost.fraction > best.fraction) {
                best = atMost;
            }
            t = best;
        }
        if (kHangInTurn) {
            hangTurnTo(t, t.fraction);
        }
    }
    if (!kHangInTurn) {
        const auto together = [&](float share) {
            for (const HangTurn& t : turns) {
                hangTurnTo(t, t.fraction * share);
            }
            for (const HangTurn& t : turns) {
                if (hangTurnDepth(t) > hangTurnAllow(t) || hangTurnFloorDepth(t) > std::max(t.floorDepthBefore, 0.0f)) {
                    return false;
                }
            }
            return true;
        };
        if (!together(1.0f) && turns.size() > 1) {
            float lo = 0.0f;
            float hi = 1.0f;
            for (int k = 0; k < 6; ++k) {
                const float mid = 0.5f * (lo + hi);
                (together(mid) ? lo : hi) = mid;
            }
            together(lo);
            for (HangTurn& t : turns) {
                t.fraction *= lo;
            }
        }
    }
}

/// The turns applied as stopped; what each arm now carries of its stride swing is remembered.
bool Armature::applyHangTurns(const std::vector<HangTurn>& turns) {
    bool changed = false;
    static const bool kHangTrace = std::getenv("IK_ARM_HANG_TRACE") != nullptr;
    for (const HangTurn& t : turns) {
        const std::size_t s = t.s;
        changed = changed || glm::length(m_boneEuler[s] - t.before) > 1.0e-4f;
        if (s < m_jsPostStepEuler.size()) {
            m_jsPostStepEuler[s] = m_boneEuler[s];
            m_jsPostStepTurned[s] = 1;
        }
        if (glm::length(t.fore) > 0.5f && s < m_hangSwingDeg.size()) {
            // The swing the arm now CARRIES: its fore-aft angle against the un-swung target's — no
            // more than was ASKED, and never the other way: a shortfall of the hang's own (an arm
            // the body stops short of its plumb under a fold) is not a swing, and remembered as one
            // it was asked for again at the next press.
            const glm::vec3 nowDir = glm::normalize(glm::vec3(m_poseGlobal[static_cast<std::size_t>(t.arm->elbow)][3]) - glm::vec3(m_poseGlobal[s][3]));
            const float     carried = sagittalDeg(nowDir, t.fore) - sagittalDeg(t.unswung, t.fore);
            m_hangSwingDeg[s] = glm::clamp(carried, std::min(0.0f, t.swingAsked), std::max(0.0f, t.swingAsked));
        }
        if (kHangTrace) {
            std::fprintf(stderr, "[hang] %s: wanted step (%.2f %.2f %.2f) applied (%.2f %.2f %.2f) depth %.1f -> %.1f mm",
                         m_boneNames[s].c_str(), t.delta.x, t.delta.y, t.delta.z, m_boneEuler[s].x - t.before.x,
                         m_boneEuler[s].y - t.before.y, m_boneEuler[s].z - t.before.z, t.depthBefore * 1000.0f,
                         hangTurnDepth(t) * 1000.0f);
            const glm::vec3 nowDir = glm::normalize(glm::vec3(m_poseGlobal[static_cast<std::size_t>(t.arm->elbow)][3]) -
                                                    glm::vec3(m_poseGlobal[s][3]));
            std::fprintf(stderr, " | wanted dir (%.2f %.2f %.2f) now (%.2f %.2f %.2f) euler (%.1f %.1f %.1f)", t.wanted.x,
                         t.wanted.y, t.wanted.z, nowDir.x, nowDir.y, nowDir.z, m_boneEuler[s].x, m_boneEuler[s].y,
                         m_boneEuler[s].z);
            std::fputc(10, stderr);
        }
    }
    return changed;
}

bool Armature::hangIdleArms() {
    static const bool kNoArmHang = std::getenv("IK_NO_ARM_HANG") != nullptr; // A/B probe
    if (kNoArmHang || !m_ikRig || !m_ikRig->dragActive()) {
        return false;
    }
    const IkRig&      rig = *m_ikRig;
    const std::size_t n = m_bones.size();
    const int         root = rig.rootNode();
    if (m_jsStartPos.size() != n || m_jsStartRot.size() != n) {
        return false;
    }
    if (m_hangSwingDeg.size() != n) {
        m_hangSwingDeg.assign(n, 0.0f);
        m_hangBaseValid.assign(n, 0);
    }
    if (m_hangElbowValid.size() != n) {
        m_hangElbowDeg.assign(n, 0.0f);
        m_hangElbowValid.assign(n, 0);
    }
    findHangArms();
    m_jsHangHeld.resize(m_jsHangArms.size());
    for (std::size_t a = 0; a < m_jsHangArms.size(); ++a) {
        m_jsHangHeld[a] = hangArmHeld(m_jsHangArms[a]) ? 1 : 0;
    }
    m_jsHangHeldCached = true; // (the parts below read the cache; cleared before the return)
    bool changed = false;
    const std::vector<float> swings = hangArmSwings();
    if (softenHangElbows(swings)) {
        changed = true;
    }
    if (relaxLiftedWrists()) {
        changed = true;
    }
    if (relaxIdleFingers()) {
        changed = true;
    }
    // (Every arm's turn is FOUND first, on the pose as the solve left it, and only then are they
    // all stopped and applied: findHangTurns, stopHangTurns, applyHangTurns.)
    std::vector<HangTurn> turns;
    findHangTurns(swings, turns);
    stopHangTurns(turns);
    if (applyHangTurns(turns)) {
        changed = true;
    }
    m_jsHangHeldCached = false;
    return changed;
}

// --- THE HEAD STAYS UP ---------------------------------------------------------------------------
//
// The neck and the head ride the chest like any idle limb, so a figure leaned or hinged 20 degrees
// forward looked at the floor, one walked forward by her chest arrived staring at her feet, and a
// kneel or a sit bowed her head with her back — the second thing the in-app screenshots showed
// (tools/ikscripts), after the arms. People do not move like that: the head is RIGHTED — as the
// trunk tilts, the neck gives most of the tilt back, up to what a neck comfortably gives.
//
// A POST-STEP like the arms' hang, and balance is blind to it the same way (balancePositions).
// The chest's tilt from upright (every bind is translation-only, so a bone's world rotation IS its
// rotation away from the bind) is given back by righted(tilt) = C tanh(tilt / C), C = kHeadRightDeg:
// all but a degree or two of a 15 degree lean, 28 of a 60 degree bow (she looks at the floor ahead
// of her, as a bowing person does), fading out from 70 to 110 degrees (on all fours or lying down a
// neck is not held up against the trunk). The head's TARGET is the righting applied to the chest's
// rotation, times the head's rotation in the chest's righted frame AS THE DRAG FOUND IT — what the
// user posed the head to, over and above the righting: a function of the trunk's pose alone, so
// nothing pops at the press, a drag brought back brings the head back, and two drags compose (a
// bow let go of and taken back up in another drag ends with the neck where it began).
//
// The chain's free channels are found by a small regularized least-squares of their own — the
// head's rotation held at the target (1e4), each channel pulled to its drag-start value by
// 1/range^2 — solved in its 3x3 dual from the pose as it stands, so the split between the neck's
// bones is the ranges' and the answer does not depend on the path; limits as an active set; the
// move rate-limited (kHeadRightStepDeg a tick). Not when the solve has the neck (a head or neck
// drag, a pin or a floor contact in the head, a suspended body: a child of the chain is ACTIVE).
constexpr float kHeadRightDeg = 30.0f;          // C: the most tilt the neck gives back
constexpr float kHeadRightFadeFromDeg = 70.0f;  // past this tilt the righting fades ...
constexpr float kHeadRightFadeGoneDeg = 110.0f; // ... and is gone: all fours, lying down
constexpr float kHeadRightStepDeg = 3.0f;       // per tick, the largest channel
constexpr float kHeadRightRangeCap = 60.0f;     // a channel's range as its share's weight, at most
constexpr float kHeadRightHold = 1.0e4f;        // the rotation's weight over the channels' 1/range^2

namespace {

/// The rotation that gives a tilted base most of its tilt back (see THE HEAD STAYS UP).
/// @p fadeOff (0-1) withholds the deep-tilt fade by that share: a STANDING figure reaching down
/// keeps looking at the floor ahead of her however far she folds (REACHING DOWN) — the fade is
/// for a body on all fours or lying down, whose neck is not held up against the trunk.
glm::mat3 rightingOf(const glm::mat3& base, float giveDeg, float fadeOff = 0.0f) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 axisNow = glm::normalize(base * up);
    const float     tilt = std::acos(glm::clamp(axisNow.y, -1.0f, 1.0f));
    glm::vec3       about = glm::cross(up, axisNow); // up -> axisNow turns about this, by +tilt
    const float     length = glm::length(about);
    if (length < 1.0e-5f || tilt < 1.0e-5f || giveDeg <= 0.0f) {
        return glm::mat3(1.0f);
    }
    about /= length;
    const float give = glm::radians(giveDeg);
    static const bool kNoFade = std::getenv("IK_HEAD_RIGHT_NO_FADE") != nullptr; // A/B probe
    const float fade = 1.0f - glm::smoothstep(glm::radians(kHeadRightFadeFromDeg), glm::radians(kHeadRightFadeGoneDeg), tilt);
    const float back = give * std::tanh(tilt / give) * (kNoFade ? 1.0f : glm::mix(fade, 1.0f, glm::clamp(fadeOff, 0.0f, 1.0f)));
    return glm::mat3(glm::rotate(glm::mat4(1.0f), -back, about));
}

} // namespace

bool Armature::rightIdleHead() {
    static const bool  kNoRight = std::getenv("IK_NO_HEAD_RIGHT") != nullptr; // A/B probe
    static const bool  kTrace = std::getenv("IK_HEAD_RIGHT_TRACE") != nullptr;
    static const float kGiveDeg = envOr<float>("IK_HEAD_RIGHT_DEG", kHeadRightDeg);
    if (kNoRight || !m_ikRig || !m_ikRig->dragActive()) {
        return false;
    }
    const IkRig&      rig = *m_ikRig;
    const std::size_t n = m_bones.size();
    if (m_jsStartRot.size() != n || m_ikStartEuler.size() != n) {
        return false;
    }
    if (!m_jsHeadFound) {
        m_jsHeadFound = true;
        m_jsHeadChain.clear();
        m_jsHeadBase = -1;
        const int head = rig.headNode();
        const int neck = rig.neckBase();
        std::vector<int> chain;
        for (int cur = head; cur >= 0 && neck >= 0; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            chain.insert(chain.begin(), cur);
            if (cur == neck) {
                break;
            }
        }
        if (!chain.empty() && chain.front() == neck && m_bones[static_cast<std::size_t>(neck)].parent >= 0) {
            m_jsHeadChain = chain;
            m_jsHeadBase = m_bones[static_cast<std::size_t>(neck)].parent;
            const glm::mat3& base = m_jsStartRot[static_cast<std::size_t>(m_jsHeadBase)];
            // K = chest0^T . righting(chest0)^T . chest0: the drag-start righting UNDONE, in the chest's frame.
            m_jsHeadIntrinsic = glm::transpose(base) * glm::transpose(rightingOf(base, kGiveDeg)) * base;
            // (... and how far the righting was FADED at the press — on all fours or lying down the
            // neck is not held up, and the head's hang is captured here as if the user had posed it.
            // That share of the intrinsic relaxes as the body RISES: stood up by the hips off all
            // fours she kept looking 57 degrees down. A head the user turned on an upright trunk
            // keeps its turn. 2026-10-28.)
            {
                const glm::vec3 axis0 = glm::normalize(base * glm::vec3(0.0f, 1.0f, 0.0f));
                const float     tilt0 = std::acos(glm::clamp(axis0.y, -1.0f, 1.0f));
                m_jsHeadIntrinsicHang = glm::smoothstep(glm::radians(kHeadRightFadeFromDeg), glm::radians(kHeadRightFadeGoneDeg), tilt0);
            }
        }
    }
    if (m_jsHeadChain.empty()) {
        return false;
    }
    // Which of the chain's bones has the SOLVE got? (A bone is one of its unknowns when a child of
    // it is active.) The righting works with the rest: under a HEAD drag that is the head bone
    // alone (its own channels do nothing for the grabbed joint's place, so the solve leaves them).
    // And what it gives back is the TRUNK's tilt, never the neck's own bend: drag a head sideways
    // and it tilts that way, pull it forward and it nods — that is what grabbing a head is FOR,
    // and what every posing tool's users expect of it (levelling a dragged head outright was
    // tried, 2026-09-21: a head slid sideways on a level gaze is a pose nobody asks for). But a
    // BOW made by the head tilts the chest 35 degrees under a neck flexed 30 more, and riding it
    // she stared back at her own feet: the head bone gives the chest's share back, as under any
    // other drag.
    static const bool kNoDragLevel = std::getenv("IK_NO_HEAD_DRAG_LEVEL") != nullptr; // A/B probe
    const std::vector<char>& active = rig.activeNodes();
    std::vector<char>        inChain(n, 0);
    std::vector<char>        owned(n, 0);
    for (const int bone : m_jsHeadChain) {
        inChain[static_cast<std::size_t>(bone)] = 1;
    }
    for (std::size_t i = 0; i < n && i < active.size(); ++i) {
        const int p = m_bones[i].parent;
        if (active[i] && p >= 0 && inChain[static_cast<std::size_t>(p)]) {
            owned[static_cast<std::size_t>(p)] = 1;
        }
    }
    // (... and the chain bones THE FACE LIFT has given the solve this tick — m_jsNeckLift, a pelvis drag
    // going down with the trunk tilted — are the solve's, as under a head drag.)
    for (const int bone : m_jsHeadChain) {
        if (bone >= 0 && static_cast<std::size_t>(bone) < m_jsNeckLift.size() && m_jsNeckLift[static_cast<std::size_t>(bone)]) {
            owned[static_cast<std::size_t>(bone)] = 1;
        }
    }
    m_jsHeadFree.clear();
    for (const IkEffector& pin : rig.pins()) {
        for (int cur = pin.node; cur >= 0; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            if (inChain[static_cast<std::size_t>(cur)]) {
                return false; // a pin or a floor contact in the head: held
            }
        }
    }
    bool anyOwned = false;
    for (const int bone : m_jsHeadChain) {
        anyOwned = anyOwned || owned[static_cast<std::size_t>(bone)];
    }
    if (anyOwned && kNoDragLevel) {
        return false;
    }
    for (const int bone : m_jsHeadChain) {
        if (!owned[static_cast<std::size_t>(bone)]) {
            m_jsHeadFree.push_back(bone);
        }
    }
    if (m_jsHeadFree.empty()) {
        return false;
    }
    const glm::mat3 baseNow(m_poseGlobal[static_cast<std::size_t>(m_jsHeadBase)]);
    // The target: the chest's rotation, RIGHTED — less the righting the drag found, which is part
    // of the pose it began with — and then the chain as it would RIDE: the solve's bones as the
    // solve has them, the righting's own at their drag-start values. (With the whole chain the
    // righting's this is the head's drag-start rotation in the chest's righted frame.)
    glm::mat3 rides(1.0f);
    for (const int b : m_jsHeadChain) {
        const Bone&     bone = m_bones[static_cast<std::size_t>(b)];
        const glm::vec3 e = owned[static_cast<std::size_t>(b)] ? m_boneEuler[static_cast<std::size_t>(b)]
                                                               : m_ikStartEuler[static_cast<std::size_t>(b)];
        rides = rides * glm::mat3(bone.orient) * glm::mat3(eulerMatrix(e, bone.rotationOrder)) * glm::mat3(bone.invOrient);
    }
    glm::mat3 intrinsic = m_jsHeadIntrinsic;
    {
        const float relax = kNoRiseTrunkHome ? 0.0f : glm::clamp(m_jsTrunkHomeShare * m_jsHeadIntrinsicHang, 0.0f, 1.0f);
        if (relax > 1.0e-4f) {
            const glm::quat k = glm::normalize(glm::quat_cast(m_jsHeadIntrinsic));
            intrinsic = glm::mat3_cast(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), k, 1.0f - relax));
        }
    }
    const glm::mat3 target = rightingOf(baseNow, kGiveDeg, m_jsReachHinge) * baseNow * intrinsic * rides;

    // The chain's channels, and the head's rotation as a function of them.
    struct Channel {
        int   bone;
        int   axis;
        float weight; // range^2 (degrees^2)
        float lo, hi;
        bool  limited;
    };
    std::vector<Channel> channels;
    for (const int b : m_jsHeadFree) {
        const Bone& bone = m_bones[static_cast<std::size_t>(b)];
        for (int a = 0; a < 3; ++a) {
            const float range = bone.rotMax[a] - bone.rotMin[a];
            if (bone.rotLimited[a] && range < kLockedRangeDeg) {
                continue;
            }
            const float share = bone.rotLimited[a] ? glm::clamp(range, 20.0f, kHeadRightRangeCap) : kHeadRightRangeCap;
            channels.push_back({b, a, share * share, bone.rotMin[a], bone.rotMax[a], bone.rotLimited[a]});
        }
    }
    if (channels.empty()) {
        return false;
    }
    std::vector<glm::vec3> euler(m_jsHeadChain.size());
    for (std::size_t k = 0; k < m_jsHeadChain.size(); ++k) {
        euler[k] = m_boneEuler[static_cast<std::size_t>(m_jsHeadChain[k])];
    }
    const auto slot = [&](int bone) {
        return static_cast<std::size_t>(std::find(m_jsHeadChain.begin(), m_jsHeadChain.end(), bone) - m_jsHeadChain.begin());
    };
    const auto headRotation = [&](const std::vector<glm::vec3>& eulers) {
        glm::mat3 rotation = baseNow;
        for (std::size_t k = 0; k < m_jsHeadChain.size(); ++k) {
            const Bone& bone = m_bones[static_cast<std::size_t>(m_jsHeadChain[k])];
            rotation = rotation * glm::mat3(bone.orient) * glm::mat3(eulerMatrix(eulers[k], bone.rotationOrder)) *
                       glm::mat3(bone.invOrient);
        }
        return rotation;
    };
    std::vector<char> fixed(channels.size(), 0);
    for (int pass = 0; pass < 4; ++pass) {
        const glm::mat3 now = headRotation(euler);
        const glm::vec3 miss = rotationVector(target * glm::transpose(now));
        // J (3 x channels, radians per degree) by differences; y = W J^T (J W J^T + 1/w)^-1 (miss + J d).
        std::vector<glm::vec3> column(channels.size(), glm::vec3(0.0f));
        std::vector<glm::vec3>& nudged = m_jsHeadNudgeBuf; // (the chain's Euler with ONE channel nudged: copied once a pass, the channel put back after its column)
        nudged = euler;
        glm::vec3              rhs = miss;
        glm::mat3              dual(0.0f);
        for (std::size_t c = 0; c < channels.size(); ++c) {
            if (fixed[c]) {
                continue;
            }
            const std::size_t k = slot(channels[c].bone);
            nudged[k][channels[c].axis] += 0.5f;
            column[c] = rotationVector(headRotation(nudged) * glm::transpose(now)) / 0.5f;
            nudged[k][channels[c].axis] = euler[k][channels[c].axis];
            const float away = euler[k][channels[c].axis] -
                               m_ikStartEuler[static_cast<std::size_t>(channels[c].bone)][channels[c].axis];
            rhs += column[c] * away;
            for (int r = 0; r < 3; ++r) {
                for (int q = 0; q < 3; ++q) {
                    dual[q][r] += channels[c].weight * column[c][r] * column[c][q];
                }
            }
        }
        for (int r = 0; r < 3; ++r) {
            dual[r][r] += 1.0f / kHeadRightHold;
        }
        const glm::vec3 lambda = glm::inverse(dual) * rhs;
        bool            clamped = false;
        for (std::size_t c = 0; c < channels.size(); ++c) {
            if (fixed[c]) {
                continue;
            }
            const std::size_t k = slot(channels[c].bone);
            float value = m_ikStartEuler[static_cast<std::size_t>(channels[c].bone)][channels[c].axis] +
                          channels[c].weight * glm::dot(column[c], lambda);
            if (channels[c].limited && (value < channels[c].lo || value > channels[c].hi)) {
                value = glm::clamp(value, channels[c].lo, channels[c].hi);
                fixed[c] = 1;
                clamped = true;
            }
            euler[k][channels[c].axis] = value;
        }
        if (!clamped && glm::length(miss) < 1.0e-4f) {
            break;
        }
    }
    // Toward it, the largest channel kHeadRightStepDeg a tick.
    float farthest = 0.0f;
    for (std::size_t k = 0; k < m_jsHeadChain.size(); ++k) {
        const glm::vec3 delta = euler[k] - m_boneEuler[static_cast<std::size_t>(m_jsHeadChain[k])];
        farthest = std::max({farthest, std::abs(delta.x), std::abs(delta.y), std::abs(delta.z)});
    }
    if (farthest < 0.005f) {
        return false;
    }
    const float share = std::min(1.0f, kHeadRightStepDeg / farthest);
    for (std::size_t k = 0; k < m_jsHeadChain.size(); ++k) {
        const std::size_t b = static_cast<std::size_t>(m_jsHeadChain[k]);
        m_boneEuler[b] += (euler[k] - m_boneEuler[b]) * share;
        clampBoneEuler(m_jsHeadChain[k]);
        recomposePoseLocal(b);
        if (b < m_jsPostStepEuler.size() && !owned[b]) {
            m_jsPostStepEuler[b] = m_boneEuler[b];
            m_jsPostStepTurned[b] = 1;
        }
    }
    computeSkinMatrices();
    if (kTrace) {
        const glm::vec3 up = glm::mat3(m_poseGlobal[static_cast<std::size_t>(m_jsHeadBase)]) * glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 look = glm::mat3(m_poseGlobal[static_cast<std::size_t>(m_jsHeadChain.back())]) * glm::vec3(0.0f, 0.0f, 1.0f);
        std::fprintf(stderr, "[head] chest tilt %.1f deg, gaze %.1f deg, step share %.2f, miss %.2f deg |",
                     glm::degrees(std::acos(glm::clamp(up.y, -1.0f, 1.0f))),
                     glm::degrees(std::asin(glm::clamp(look.y, -1.0f, 1.0f))), share,
                     glm::degrees(glm::length(rotationVector(target * glm::transpose(headRotation(euler))))));
        for (std::size_t k = 0; k < m_jsHeadChain.size(); ++k) {
            const glm::vec3& e = m_boneEuler[static_cast<std::size_t>(m_jsHeadChain[k])];
            std::fprintf(stderr, " %s(%.1f %.1f %.1f)", m_boneNames[static_cast<std::size_t>(m_jsHeadChain[k])].c_str(), e.x, e.y, e.z);
        }
        std::fputc(10, stderr);
    }
    return true;
}

} // namespace pose

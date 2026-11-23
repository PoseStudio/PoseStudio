// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file armatureiksolvecursor.cpp
 * @brief The DRAGGED JOINT's rows (see armatureiksolve.cpp for the map): the cursor's row with its
 *        bounded pull, the hips drawn under a rising trunk, the hip sway's level chest, a sliding
 *        foot's sole, a hand slid along the floor (ikHandSlideRows), a hand laid on the body with
 *        the akimbo's elbow (ikHandLayRows), THE GROUP's secondary cursors, a suspended body's hang.
 *
 * What the user's own gesture asks of the solve, as the pin rows (armatureiksolvepins.cpp) are what
 * the world asks. Every stage is a member function over the tick's IkSolveScratch
 * (armatureiksolvestate.h) and begins by naming the members it reads and writes. Qt-free (std + GLM).
 */

#include "armature.h"
#include "armatureiksolvegeometry.h"
#include "armatureiksolvestate.h"
#include "armatureiksolvetuning.h"

#include "ikmath.h"
#include "ikrig.h"
#include "jointsolver.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pose {

namespace {
// A/B levers read at more than one site of this TU.
const double kSlidePitchW = envOr("IK_JS_SLIDEPITCH", kSlideSolePitchWeight);
const double kSlideHeadingW = envOr("IK_JS_SLIDEHEADING", kSlideSoleHeadingWeight);
const double kSlideRollW = envOr("IK_JS_SLIDEROLL", kSlideSoleRollWeight);
const double kCursorPull = envOr("IK_JS_PULL", kEffectorPull);         // the cursor row's bounded pull ...
const double kCursorRootPull = envOr("IK_JS_ROOTPULL", kRootDragPullScale); // ... and a pelvis drag's multiple of it
} // namespace

/// ikCursorRows' part: A HAND SLID ALONG THE FLOOR — a dragged wrist whose target is at the hand's
/// floor height lies on its palm, headed along the drag (see the block's own comment).
void Armature::ikHandSlideRows(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    const glm::vec3* const dragTarget = s.dragTarget;
    const float& floorModel = s.floorModel;
    const float& legScale = s.legScale;
    JointSolver::Problem& problem = s.problem;

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
            glm::vec3       f(0.0f);
            glm::vec3       palm(0.0f);
            const bool      palmFound = handBindFrame(fingers, f, palm);
            const glm::vec3 down(0.0f, -1.0f, 0.0f);
            // (The heading: the drag's own way along the floor once it has gone a few
            // centimetres, the figure's forward until then.)
            // (Not the scratch's fore axis: this one is normalized, and differs from it in the last bit.)
            const glm::vec3 forward = glm::normalize(glm::vec3(std::sin(m_jsRootHeading), 0.0f, std::cos(m_jsRootHeading)));
            const float     went = glm::length(travel);
            const glm::vec3 h = went > 0.03f * legScale ? glm::normalize(travel) : forward;
            if (palmFound) {
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
                JointOrientationTask flat;
                flat.bone = effector;
                flat.target = glm::dmat3(lieRot);
                flat.perAxis = true;
                flat.frame = glm::dmat3(glm::cross(up, fore), up, fore);
                flat.axisWeights = glm::dvec3(kSlidePitchW, kSlideHeadingW, kSlideRollW) * static_cast<double>(handSlide);
                problem.orientations.push_back(flat);
            }
        }
    }
}

/// ikCursorRows' part: A HAND LAID ON THE BODY — a dragged wrist whose target comes to rest against
/// a body volume lies on its palm, or the back of its hand, by the forearm's twist room (see the
/// block's own comment).
void Armature::ikHandLayRows(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    const glm::vec3* const dragTarget = s.dragTarget;
    const float& floorModel = s.floorModel;
    const float& legScale = s.legScale;
    JointSolver::Problem& problem = s.problem;

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
    // (The akimbo's share is THIS tick's lay's: set only inside the lay block, it stayed at its last
    // value once the lay dropped — the hand leaving the hip or moving on — and the shoulder's reference
    // followed the hand through the rest of the drag: the all-fours route's hands pass the hips.)
    m_jsAkimboShare = 0.0f;
    const float akimboEaseBefore = m_jsAkimboEase; // (the ease decays unless the lay re-engages it below)
    m_jsAkimboEase = std::max(0.0f, akimboEaseBefore - kHandLayElbowEaseStep);
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
            const float     t = volumeAxisParam(ax, T);
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
                const float      tRaw = volumeAxisParam(ax, R);
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
            glm::vec3 f(0.0f);
            glm::vec3 palm(0.0f);
            if (handBindFrame(fingers, f, palm)) {
                const glm::mat3 now(m_poseGlobal[hnode]);
                const glm::vec3 nOut = bestNormal;
                // AKIMBO — A HAND LAID ON THE HIP TAKES ITS ELBOW OUT (2026-11-02; kHandLayElbow*): the lay
                // turns the palm to the hip, and the arm's redundancy — the elbow — stayed where the
                // posture terms left it, hanging straight beside the body: a hand pressed to the hip's
                // side on a straight arm, which nobody does. Where the laid surface's normal is LATERAL
                // (the hip's side, not the belly: the belly's normal is forward) and the hand lies at the
                // hips' height, the SHOULDER'S ABDUCTION channel — the socket's Side-Side dial — gets a
                // REFERENCE that stands the upper arm kHandLayElbowOutDeg from straight down on the side
                // that takes the elbow OUT (the dial's sweep says which), at kHandLayElbowStiffness x the
                // lay (ikPostureDofs, the next tick): a reference moves its own channel and nothing else.
                // (A soft POSITION row on the elbow was the first cut: the trunk answered it as readily
                // as the shoulder — at 300/m2 the head moved 90-185mm on four rigs as the body leaned
                // toward the elbow's target; at 100 the arm came out only 23-32 degrees.) IK_JS_NO_AKIMBO.
                static const bool kNoAkimbo = std::getenv("IK_JS_NO_AKIMBO") != nullptr; // A/B probe
                {
                    const glm::vec3 rootAt(m_poseGlobal[static_cast<std::size_t>(root)][3]);
                    const float     lateralShare = std::abs(glm::dot(nOut, s.lateral));
                    // The surface must be the BODY's — the pelvis, the belly's side, a thigh — not the
                    // other ARM's: a hand laid on the other forearm hanging beside the hip reads a lateral
                    // normal at the hips' height too, and took its elbow out.
                    bool armSurface = false;
                    for (std::size_t w = 0; w < n && !armSurface; ++w) {
                        if (m_jsWristOf[w] != static_cast<int>(w)) {
                            continue;
                        }
                        const int sock = armSocketOf(static_cast<int>(w));
                        armSurface = sock >= 0 && (bestA == sock || boneUnder(bestA, sock));
                    }
                    const float     hipBand = 1.0f - glm::smoothstep(0.5f * kHandLayHipBand * scale, kHandLayHipBand * scale, std::abs(T.y - rootAt.y));
                    const int       socket = armSocketOf(effector);
                    const JointDial dial = socket >= 0 ? jointDial(socket, JointDialKind::BendSideways) : JointDial{};
                    JointDialSweep  sweep;
                    if (!kNoAkimbo && !armSurface && lateralShare > 0.7f && hipBand > 1.0e-3f && dial.valid() && dial.bone == socket &&
                        jointDialSweep(dial, sweep) && std::abs(dial.fullDeg) > 1.0e-3f) {
                        const glm::vec3 socketAt(m_poseGlobal[static_cast<std::size_t>(socket)][3]);
                        const float     side = glm::dot(socketAt - rootAt, s.lateral) >= 0.0f ? 1.0f : -1.0f; // the arm's own side
                        const glm::vec3 outDir = s.lateral * side;
                        // The upper arm's angle from straight down now, and how a degree of the dial's
                        // positive side moves its far end: + takes it out, - in.
                        const glm::vec3 upper = sweep.tip - socketAt;
                        const float     upperLen = glm::length(upper);
                        const float     outNowDeg = upperLen > 1.0e-4f ? glm::degrees(std::acos(glm::clamp(glm::dot(upper / upperLen, glm::vec3(0.0f, -1.0f, 0.0f)), -1.0f, 1.0f))) : 0.0f;
                        const float     outPerDialDeg = glm::dot(sweep.tipPerDegree, outDir); // metres per degree of the channel, outward
                        // WHAT THE ARM CAN REACH (kHandLayElbowReachMarginDeg): with the hand held on the hip, the
                        // upper arm may stand out only as far as the FOREARM still reaches the hand's target -
                        // the elbow on the upper arm's sphere about the socket, within the forearm's length of
                        // the target: the law of cosines gives the half-angle of that cap about the socket->
                        // target direction, and the outboard edge of the cap, in the lateral-down plane the
                        // dial swings in, is the most abduction there is. A fixed 45 asked 3cm more than the
                        // forearm had on a broad-shouldered, narrow-hipped body (the hand 2cm INBOARD of the
                        // socket), and the solve paid it with the trunk: the body leaned and sank toward the
                        // hand (head 71mm, hips 18mm down, knees 25 degrees).
                        int elbow = -1;
                        for (int cur = m_bones[hnode].parent; cur >= 0 && cur != socket && elbow < 0; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                            if (foldAxisOf(cur) >= 0) {
                                elbow = cur;
                            }
                        }
                        float reachOutDeg = kHandLayElbowOutDeg;
                        bool  reachable = true;
                        if (elbow >= 0) {
                            const glm::vec3 elbowAt(m_poseGlobal[static_cast<std::size_t>(elbow)][3]);
                            const glm::vec3 handAt(m_poseGlobal[hnode][3]);
                            const float     U = glm::length(elbowAt - socketAt); // the upper arm, socket to elbow
                            const float     F = glm::length(handAt - elbowAt);   // the forearm, elbow to wrist
                            const glm::vec3 d = T - socketAt;
                            const float     R = glm::length(d);
                            if (U > 1.0e-3f && F > 1.0e-3f && R > 1.0e-3f) {
                                const float cosAlpha = glm::clamp((U * U + R * R - F * F) / (2.0f * U * R), -1.0f, 1.0f);
                                // u(theta) = out sin(theta) - up cos(theta); cos(beta) = u . d/R = a sin + b cos
                                const float a = glm::dot(d, outDir) / R;
                                const float b = -d.y / R;
                                const float hyp = std::sqrt(a * a + b * b);
                                if (hyp > 1.0e-4f && cosAlpha <= hyp) {
                                    const float psi = std::atan2(b, a);
                                    const float thetaMax = glm::pi<float>() - std::asin(glm::clamp(cosAlpha / hyp, -1.0f, 1.0f)) - psi;
                                    reachOutDeg = std::min(kHandLayElbowOutDeg, glm::degrees(thetaMax) - kHandLayElbowReachMarginDeg);
                                } else {
                                    reachable = false; // (no abduction in that plane reaches the target: leave the arm alone)
                                }
                            }
                        }
                        if (reachable && reachOutDeg > 0.0f && std::abs(outPerDialDeg) > 1.0e-5f && upperLen > 1.0e-4f) {
                            // (The dial's positive side is the channel moving by sign(fullDeg): the channel's
                            // direction that takes the elbow OUT is that sign times the sweep's; a degree of
                            // channel is a degree of arm, as near as the swing is a rotation about the socket.)
                            const float chanSign = (outPerDialDeg > 0.0f ? 1.0f : -1.0f) * (dial.fullDeg < 0.0f ? -1.0f : 1.0f);
                            const float needDeg = reachOutDeg - outNowDeg; // more out (+) or less (-)
                            m_jsAkimboBone = socket;
                            m_jsAkimboAxis = dial.axis;
                            m_jsAkimboRefDeg = m_boneEuler[static_cast<std::size_t>(socket)][dial.axis] + chanSign * needDeg;
                            m_jsAkimboEase = std::min(1.0f, akimboEaseBefore + kHandLayElbowEaseStep);
                            m_jsAkimboShare = lay * lateralShare * hipBand * m_jsAkimboEase;
                            if (kHandLayTrace) {
                                std::fprintf(stderr, "[js-lay] akimbo: %s's %s axis %d at %.1f (upper arm %.1f deg out, reach %.1f, %+.4f m/deg out) -> ref %.1f, share %.2f (ease %.2f)",
                                             m_boneNames[hnode].c_str(), m_boneNames[static_cast<std::size_t>(socket)].c_str(), dial.axis,
                                             m_boneEuler[static_cast<std::size_t>(socket)][dial.axis], outNowDeg, reachOutDeg, outPerDialDeg, m_jsAkimboRefDeg, m_jsAkimboShare, m_jsAkimboEase);
                                std::fputc(10, stderr);
                            }
                        }
                    }
                }
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
                            if (foldAxisOf(cur) >= 0) {
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
                                    const glm::mat3 prefix = channelPrefix(cb, a);
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
                    const glm::vec3& forward = s.fore;
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
                    static const float kLayStep = envOr("IK_JS_HANDLAYSTEP", kHandLayTurnStepDeg);
                    const float        step = kLayStep * lay; // (the ease advances WITH the lay)
                    const LeadingEase  ends = advanceLeadingEase(m_jsHandLayFrom, lieRot, now, step, m_jsHandLayEase);
                    lieRot = glm::mat3_cast(glm::slerp(ends.from, ends.to, m_jsHandLayEase));
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
}

/// The CURSOR's row and its company: the hips drawn under a rising trunk, the sway's level chest, a slid
/// foot's sole, a hand slid along the floor (ikHandSlideRows), a hand laid on the body (ikHandLayRows),
/// a suspended body's hang.
void Armature::ikCursorRows(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    const glm::vec3* const dragTarget = s.dragTarget;
    const glm::vec3* const holdTarget = s.holdTarget;
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
        // (A trunk joint's drag keeps the hand's pull. The pelvis's fivefold was tried for it: a
        // crouching figure's chest reached no further once its real blockers were gone — the
        // pelvis's prices under a hinge, the spine's S-curve — a slouched head raised 15cm went
        // 6cm up onto her toes, and with a foot pinned a far chest drag turned the planted one
        // 10-12 degrees.)
        task.huber = kCursorPull * (effector == root ? kCursorRootPull : 1.0) / (2.0 * kEffectorWeight);
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
        JointPositionTask task;
        task.bone = sec;
        task.target = glm::dvec3(m_jsGroupTargets[i]);
        task.weight = kEffectorWeight;
        task.huber = kCursorPull * (sec == root ? kCursorRootPull : 1.0) / (2.0 * kEffectorWeight);
        problem.positions.push_back(task);
    }

    // The hip sway's CHEST: level (its roll about the fore axis), as the drag found it. (Its HEADING
    // under THE STRIDE TURNS THE HIPS is held by the spine's twist REFERENCES, ikPostureDofs — not by
    // a row here: a 1000/rad^2 heading row tipped a standing knee off its saddle, a 67mm pop.)
    if (swayChest >= 0 && static_cast<std::size_t>(swayChest) < m_jsStartRot.size()) {
        static const double kChestLevel = envOr("IK_JS_SWAYCHEST", kSwayChestLevelWeight);
        const glm::dvec3 lateral(s.lateral);
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
            glm::quat level;
            if (slideAtStart < 1.0f && flatKeepingHeading(target, lateralLocal, level)) {
                target = glm::mat3_cast(glm::slerp(level, glm::quat_cast(target), slideAtStart));
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
                    const LeadingEase ends = advanceLeadingEase(m_jsSoleEaseFrom, target, glm::mat3(m_poseGlobal[e]), kSoleTurnStepDeg,
                                                                m_jsSoleEase);
                    if (m_jsSoleEase < 1.0f) {
                        target = glm::mat3_cast(glm::slerp(ends.from, ends.to, m_jsSoleEase));
                        m_jsPinsEasing = true; // (the settle waits for the sole to arrive, as for a pin)
                    }
                }
            }
            const glm::dvec3 lateral = glm::normalize(glm::dvec3(target * lateralLocal));
            JointOrientationTask sole;
            sole.bone = effector;
            sole.target = glm::dmat3(target);
            sole.perAxis = true;
            sole.frame = frameSquareTo(lateral);
            sole.axisWeights = glm::dvec3(kSlidePitchW, kSlideHeadingW, kSlideRollW) * static_cast<double>(slide);
            problem.orientations.push_back(sole);
        }
    }

    ikHandSlideRows(s); // A HAND SLID ALONG THE FLOOR
    ikHandLayRows(s);   // A HAND LAID ON THE BODY

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

} // namespace pose

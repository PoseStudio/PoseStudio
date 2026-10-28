/**
 * @file armatureiksolveposture.cpp
 * @brief The solve's POSTURE MODEL (see armatureiksolve.cpp for the map): every unknown's
 *        stiffness and reference — the trunk drag's, the seat roll's, the sagittal lock's, the
 *        kneel tilt's, the rise home's, the root's home — then the spine's coupling rows and the
 *        posture easing. What the solver minimizes besides its rows.
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

/// The POSTURE MODEL: every unknown's stiffness and reference (the trunk drag, the seat roll, the sagittal lock, the kneel tilt, the rise home, the root's home).
void Armature::ikPostureModel(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    const std::vector<IkEffector>& pins = s.pins;
    std::vector<char>& dofBone = s.dofBone;
    const float& rising = s.rising;
    const float& risen = s.risen;
    const bool& rootTurns = s.rootTurns;
    bool& swayValid = s.swayValid;
    glm::vec3& swayEuler = s.swayEuler;
    const float& contrapposto = s.contrapposto;
    std::vector<char>& cls = s.cls;
    std::vector<char>& twistPriced = s.twistPriced;
    std::vector<char>& landedArm = s.landedArm;
    std::vector<char>& pinJunction = s.pinJunction;
    std::vector<char>& pinNode = s.pinNode;
    std::vector<char>& homeLimb = s.homeLimb;
    std::vector<char>& girdle = s.girdle;
    const std::vector<int>& handCollars = s.handCollars; // (each dragged arm's collar and its ELEVATION channel: capped at kCollarElevationMaxDeg)
    const std::vector<int>& handCollarAxes = s.handCollarAxes;
    // The thighs whose foot the rig has PLANTED (a standing foot model: its ball found), and each
    // one's ABDUCTION channel: of its unlocked channels, neither the twist nor the flexion, the
    // one whose axis lies most along the figure's fore axis at bind (every bind is
    // translation-only, so a channel's orientation axis is a world axis at rest). See the
    // abduction box below (kThighAbductBoxDeg).
    std::vector<char> plantedThigh(n, 0);
    std::vector<int>  abductAxis(n, -1);
    std::vector<int>  flexAxis(n, -1); // the thigh's third channel: its flexion, which tightens the box
    for (std::size_t p = 0; p < s.pins.size(); ++p) {
        const int node = s.pins[p].node;
        if (node < 0 || static_cast<std::size_t>(node) >= n || static_cast<std::size_t>(node) >= m_jsBall.size() ||
            m_jsBall[static_cast<std::size_t>(node)] < 0) {
            continue;
        }
        // The THIGH: the foot's ancestor that hangs from the solve root or the pelvis bone — the
        // bend bone that flexes and abducts. (Not IkRig::legOf: the thigh SOCKET is a girdle joint
        // to the rig, and that walk returns the bone below it — the twist bone on the generations
        // that cut the thigh's twist out, the SHIN on the one whose thighs hang straight off the
        // hips with their twist helpers beside the shin.)
        int thigh = node;
        while (thigh >= 0 && static_cast<std::size_t>(thigh) < n) {
            const int par = m_bones[static_cast<std::size_t>(thigh)].parent;
            if (par < 0 || par == root || rig.isPelvisBone(par)) {
                break;
            }
            thigh = par;
        }
        if (thigh < 0 || thigh == node || static_cast<std::size_t>(thigh) >= n) {
            continue;
        }
        const Bone& tb = m_bones[static_cast<std::size_t>(thigh)];
        int   best = -1;
        float bestFore = 0.0f;
        int   flex = -1;
        float flexLateral = 0.0f;
        for (int a = 0; a < 3; ++a) {
            if (m_jsTwistAxis[static_cast<std::size_t>(thigh)] == a) {
                continue;
            }
            const float range = tb.rotLimited[a] ? tb.rotMax[a] - tb.rotMin[a] : 360.0f;
            if (range < 2.0f) {
                continue; // locked
            }
            const float fore = std::abs(glm::vec3(tb.orient[a]).z);
            if (best < 0 || fore > bestFore) {
                best = a;
                bestFore = fore;
            }
            const float lateral = std::abs(glm::vec3(tb.orient[a]).x);
            if (flex < 0 || lateral > flexLateral) {
                flex = a;
                flexLateral = lateral;
            }
        }
        if (best >= 0 && flex >= 0 && flex != best) {
            plantedThigh[static_cast<std::size_t>(thigh)] = 1;
            abductAxis[static_cast<std::size_t>(thigh)] = best;
            flexAxis[static_cast<std::size_t>(thigh)] = flex;
        }
        static const bool kAbductTrace = std::getenv("IK_JS_ABDUCT_TRACE") != nullptr; // the planted thighs and their boxed channel
        if (kAbductTrace) {
            std::fprintf(stderr, "[js-abduct] pin %s -> thigh %s: abduction axis %d (fore %.2f), twist axis %d, euler (%.1f %.1f %.1f)\n",
                         m_boneNames[static_cast<std::size_t>(node)].c_str(), m_boneNames[static_cast<std::size_t>(thigh)].c_str(), best, bestFore,
                         m_jsTwistAxis[static_cast<std::size_t>(thigh)], m_boneEuler[static_cast<std::size_t>(thigh)].x,
                         m_boneEuler[static_cast<std::size_t>(thigh)].y, m_boneEuler[static_cast<std::size_t>(thigh)].z);
        }
    }
    const float& legScale = s.legScale;
    bool& legDrag = s.legDrag;
    const glm::vec3*& goalPoint = s.goalPoint;
    const float& slide = s.slide;
    bool& kneeDrag = s.kneeDrag;
    std::vector<char>& kneeLegBone = s.kneeLegBone;
    float& plant = s.plant;
    bool& elbowDrag = s.elbowDrag;

    static const double kTrunk = envOr("IK_JS_TRUNK", kTrunkStiffness);
    static const double kPinLimb = envOr("IK_JS_PINLIMB", kPinLimbStiffness);
    static const double kRootS = envOr("IK_JS_ROOT", kRootStiffness);
    static const double kTwistS = envOr("IK_JS_PINTWIST", kPinLimbTwistScale);

    JointSolver::Problem& problem = s.problem;
    // A TRUNK drag: a trunk-class joint that is not the root's, a knee's, an elbow's or a foot's.
    s.trunkDrag = effector != root && rig.effectorIsTrunk() && !kneeDrag && !elbowDrag && !legDrag;
    const bool& trunkDrag = s.trunkDrag;
    // The SPINE chain (searched once per skeleton): from the root up through the child with the
    // biggest foot-free subtree to the first bone three big branches leave.
    {
        if (m_jsSpineChain.empty()) {
            std::vector<int> size(n, 1);
            std::vector<char> hasFoot(n, 0);
            for (std::size_t i = n; i-- > 0;) { // children follow parents
                hasFoot[i] = hasFoot[i] || m_ikBindPos[i].y < 0.20f * rig.sizeScale();
                const int par = m_bones[i].parent;
                if (par >= 0) {
                    size[static_cast<std::size_t>(par)] += size[i];
                    hasFoot[static_cast<std::size_t>(par)] = hasFoot[static_cast<std::size_t>(par)] || hasFoot[i];
                }
            }
            m_jsSpineChain.push_back(-1); // (searched: an empty chain stays "searched")
            for (int cur = root, depth = 0; depth < 8; ++depth) {
                int next = -1;
                for (const int c : m_children[static_cast<std::size_t>(cur)]) {
                    if (!hasFoot[static_cast<std::size_t>(c)] &&
                        (next < 0 || size[static_cast<std::size_t>(c)] > size[static_cast<std::size_t>(next)])) {
                        next = c;
                    }
                }
                if (next < 0) {
                    break;
                }
                m_jsSpineChain.push_back(next);
                int branches = 0;
                for (const int c : m_children[static_cast<std::size_t>(next)]) {
                    branches += size[static_cast<std::size_t>(c)] >= 4 ? 1 : 0;
                }
                if (branches >= 3) {
                    break;
                }
                cur = next;
            }
        }
    }
    // THE WEIGHT LIFT this tick (a foot drag's 1 - slide, a knee drag's 1 - plant), measured from the
    // lift the drag BEGAN with: THE WEIGHT SHIFT (the root's home, below) moves the hips by it, and
    // CONTRAPPOSTO here rolls the pelvis by it.
    {
        s.weightLift = 0.0f;
        if (legDrag && goalPoint != nullptr) {
            s.weightLift = 1.0f - slide;
        } else if (kneeDrag && s.kneeFootPin >= 0) {
            s.weightLift = 1.0f - plant;
        }
        if (m_jsWeightLiftStart < 0.0f) {
            m_jsWeightLiftStart = s.weightLift;
        }
        const float room = 1.0f - m_jsWeightLiftStart;
        m_jsWeightLift = room > 1.0e-3f ? glm::clamp((s.weightLift - m_jsWeightLiftStart) / room, 0.0f, 1.0f) : 0.0f;
    }
    // CONTRAPPOSTO (2026-09-25; IK_JS_NO_CONTRAPPOSTO, IK_JS_CONTRAPPOSTO): standing on one leg — a
    // foot lifted off a two-footed stance, the weight shifted over the other — the pelvis ROLLS the
    // free side down by kContrappostoDeg at the full lift, the standing hip hiking, and the CHEST is
    // kept level (the hip sway's row: kSwayChestLevelWeight), so the spine curves back between them
    // — the S of a relaxed one-leg stance. Since the weight shift a knee raise stood balanced over
    // its foot with a dead-level pelvis and a trunk stiff as a post. The roll is a REFERENCE on the
    // root's rotation (the sway's fields: swayEuler, swayChest; the pelvis drag's price over the
    // share), built like the sway's on top of the rotation the drag began with, so a foot brought
    // back down levels her again; and the spine chain stays among the unknowns for the rest of the
    // drag once the tilt has engaged, to bring the chest home (the sway's lesson: taken out the
    // moment nothing pulled on it, its joints froze a few tenths of a degree short).
    // (IN THE SOLVE FROM THE DRAG'S FIRST TICK — the roll's reference simply 0 until the foot lifts,
    // the pelvis frozen at its price floor: nothing appears or disappears. Entering with the lift,
    // the spine chain met a balance need the shrinking support had already built up and leaned the
    // trunk through it in one tick — 62mm at the brow as the knee left the floor, 131 as it came
    // back down and the chain left again.)
    static const bool kNoContrapposto = std::getenv("IK_JS_NO_CONTRAPPOSTO") != nullptr; // A/B probe
    static const double kTiltDeg = envOr("IK_JS_CONTRAPPOSTO", kContrappostoDeg);
    s.contrapposto = 0.0f;
    if (!kNoContrapposto && !rootTurns && glm::dot(m_jsWeightShift, m_jsWeightShift) > 0.0f &&
        !rig.suspended() && static_cast<std::size_t>(root) < m_jsStartRot.size() && kTiltDeg > 0.0) {
        const glm::vec3 lateral(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
        const glm::vec3 fore(std::sin(m_jsRootHeading), 0.0f, std::cos(m_jsRootHeading));
        // (The standing foot's side along the figure's lateral axis: the shift points at it. A
        // positive turn about the fore axis raises the +lateral side, so the standing side comes
        // UP and the free side goes down.)
        const float side = glm::dot(glm::vec2(lateral.x, lateral.z), m_jsWeightShift) >= 0.0f ? 1.0f : -1.0f;
        const float theta = glm::radians(static_cast<float>(kTiltDeg)) * m_jsWeightLift * side;
        const Bone& rb = m_bones[static_cast<std::size_t>(root)];
        const glm::mat3 roll(glm::mat4_cast(glm::angleAxis(theta, fore)));
        const glm::mat3 parentRot = rb.parent >= 0 ? glm::mat3(m_poseGlobal[static_cast<std::size_t>(rb.parent)]) : glm::mat3(1.0f);
        const glm::mat3 local = glm::mat3(rb.invOrient) * glm::transpose(parentRot) * roll * m_jsStartRot[static_cast<std::size_t>(root)] * glm::mat3(rb.orient);
        swayEuler = eulerFromMatrix(local, rb.rotationOrder);
        for (int a = 0; a < 3; ++a) {
            while (swayEuler[a] - m_ikStartEuler[static_cast<std::size_t>(root)][a] > 180.0f) {
                swayEuler[a] -= 360.0f;
            }
            while (swayEuler[a] - m_ikStartEuler[static_cast<std::size_t>(root)][a] < -180.0f) {
                swayEuler[a] += 360.0f;
            }
        }
        swayValid = true;
        s.contrapposto = std::max(m_jsWeightLift, kContrappostoFloor); // (the price's share: frozen at the floor until the lift)
        dofBone[static_cast<std::size_t>(root)] = 1;
        s.swayChest = m_jsSpineChain.size() > 1 ? m_jsSpineChain.back() : -1;
        for (std::size_t k = 1; k < m_jsSpineChain.size(); ++k) {
            dofBone[static_cast<std::size_t>(m_jsSpineChain[k])] = 1;
        }
    }
    // A drag IN the figure's sagittal plane keeps the spine in it (kSagittalLock): its twist and
    // side-bend channels cost that many times as much while the trunk joint's travel has no
    // sideways share to speak of. A chest dragged straight forward out of a crouch, past what the
    // spine's flexion could give, broke the symmetry instead — a helix, every twist and side-bend
    // channel wrung to its limit for 2cm more reach, entered in ONE tick (a 22-29cm jump at
    // the fingertips) — and a folded trunk kept its top up the same way rather than pay for the
    // hips to rise.
    s.inSpine.assign(n, 0);
    std::vector<char>& inSpine = s.inSpine;
    const std::vector<char>& active = s.active;
    for (std::size_t k = 1; k < m_jsSpineChain.size(); ++k) {
        inSpine[static_cast<std::size_t>(m_jsSpineChain[k])] = 1;
    }
    // THE SEAT ROLLS (a figure sitting ON THE FLOOR, her chest or head dragged): the pelvis's pitch
    // REFERENCE turns with the cursor about the seat — by the angle the line from the hips to the
    // target has swept, in the figure's sagittal plane, since the drag began. A price can only
    // prefer less travel: at the bow's price the pelvis gave a recline a third of it and the spine
    // — a tenth the price, joint for joint — the rest, to its limits: leaned back by the chest she
    // arched over a pelvis still 40 degrees from the floor, "her hip pointed up", and could not be
    // laid down. A body let down onto its back ROLLS over its seat, the trunk all of a piece; so
    // the reference goes with the target, and the spine (its own references where they were, its
    // coupling to the pelvis measured from the ROLLED pitch) keeps the shape it sat in. A function
    // of the target alone: brought back up, she sits as she sat.
    s.seatRollDeg = 0.0;
    double& seatRollDeg = s.seatRollDeg;
    float  seatRollLift = 0.0f; // ... and how much higher (lower) the hip joint RESTS at that roll
    m_jsSeatRolls = false;
    static const bool kSeatTrace = std::getenv("IK_JS_SEAT_TRACE") != nullptr;
    if (kSeatTrace) {
        std::fprintf(stderr, "[seat] floorSeatStart %d hinge %.2f trunkDrag %d goal %d effector %d root %d rising %.2f",
                     m_jsFloorSeatStart ? 1 : 0, m_jsTrunkHinge, trunkDrag ? 1 : 0, goalPoint != nullptr ? 1 : 0, effector, root,
                     rising);
        std::fputc(10, stderr);
    }
    {
        static const bool kNoSeatRoll = std::getenv("IK_JS_NO_SEAT_ROLL") != nullptr; // A/B probe
        if (!kNoSeatRoll && m_jsFloorSeatStart && m_jsTrunkHinge > 1.0e-3f && trunkDrag && goalPoint != nullptr &&
            effector != root && root >= 0) {
            const glm::vec3 lateral(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
            const glm::vec3 seat = m_jsStartPos[static_cast<std::size_t>(root)];
            glm::vec3 from = m_jsStartTarget - seat;
            glm::vec3 to = *goalPoint - seat;
            from -= lateral * glm::dot(from, lateral);
            to -= lateral * glm::dot(to, lateral);
            const float lever = glm::length(from);
            m_jsSeatRolls = true;
            if (lever > 0.10f * rig.sizeScale() && glm::length(to) > 1.0e-4f) {
                float swept = std::atan2(glm::dot(glm::cross(from, to), lateral), glm::dot(from, to));
                // (The hip joint does not stay where it is as the pelvis rolls: lying on her back
                // it rests 14cm over the floor, sitting 24 — the flesh under it, IkRig::
                // floorClearanceAlong. The angle is swept about the seat AS ROLLED: a fixed point,
                // three rounds of it. About the joint as found, a figure sat back up from lying
                // was asked 10 degrees short, and stopped with her chest 11cm behind its cursor.)
                // (The flesh is the SEAT NODE's — the pelvis bone's, which the root only borrows —
                // taken to ride the roll rigidly.)
                const int seatNode = m_jsSeatNode >= 0 ? m_jsSeatNode : root;
                if (seatNode < static_cast<int>(m_jsStartRot.size())) {
                    const float floorAt = -m_transform[3][1];
                    const glm::vec3 down(0.0f, -1.0f, 0.0f);
                    const glm::mat3& seatFound = m_jsStartRot[static_cast<std::size_t>(seatNode)];
                    const float restsFound = rig.floorClearanceAlong(seatNode, glm::transpose(seatFound) * down);
                    for (int round = 0; round < 3; ++round) {
                        const glm::mat3 rolled = glm::mat3(glm::rotate(glm::mat4(1.0f), swept, lateral)) * seatFound;
                        const float rests = seat.y + rig.floorClearanceAlong(seatNode, glm::transpose(rolled) * down) - restsFound;
                        glm::vec3 rolledTo = *goalPoint - glm::vec3(seat.x, std::max(rests, floorAt), seat.z);
                        rolledTo -= lateral * glm::dot(rolledTo, lateral);
                        if (glm::length(rolledTo) < 1.0e-4f) {
                            break;
                        }
                        to = rolledTo;
                        swept = std::atan2(glm::dot(glm::cross(from, to), lateral), glm::dot(from, to));
                    }
                }
                // (A target close over the seat says nothing of a direction: none of it within a
                // third of the lever, all of it from two thirds. And not while she is getting UP.)
                const float told = glm::smoothstep(0.35f, 0.65f, glm::length(to) / lever);
                static const double kShare = envOr("IK_JS_SEATROLL", kSeatRollShare);
                // (... nor for a trunk that sits UPRIGHT and is taken UP: that is getting up, the
                // rise's own business from its first centimetre — a roll that came with the first
                // of the travel and went again as the rise took over stalled one generation's
                // getting-up for three ticks and let go of it in one, 14cm. A trunk that lies back
                // and is taken up is a SIT-UP, and the roll's.)
                const float startTilt = std::acos(glm::clamp(from.y / lever, -1.0f, 1.0f));
                const float upright = 1.0f - glm::smoothstep(glm::radians(25.0f), glm::radians(45.0f), startTilt);
                const float takenUp = glm::smoothstep(0.0f, 0.05f * rig.sizeScale(), goalPoint->y - m_jsStartTarget.y);
                seatRollDeg = static_cast<double>(glm::degrees(swept) * told * (1.0f - rising) * (1.0f - upright * takenUp)) * kShare;
                seatRollDeg = glm::clamp(seatRollDeg, -kSeatRollMaxDeg, kSeatRollMaxDeg);
                // The root's HEIGHT goes with the roll too: where the hip joint rests over the
                // flesh under it — 14cm lying on her back, 24 sitting. Its reference left at the
                // height the drag found, the pelvis's vertical price (1000/m^2, for a body that
                // stands) held a figure sitting up from her back 7cm down against the floor's own
                // rows: she stopped with her pelvis 19 degrees short and her chest 12cm from the
                // cursor.
                if (seatNode < static_cast<int>(m_jsStartRot.size())) {
                    const glm::mat3& found = m_jsStartRot[static_cast<std::size_t>(seatNode)];
                    const glm::mat3  rolled =
                        glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(static_cast<float>(seatRollDeg)), lateral)) * found;
                    const glm::vec3 down(0.0f, -1.0f, 0.0f);
                    seatRollLift = rig.floorClearanceAlong(seatNode, glm::transpose(rolled) * down) -
                                   rig.floorClearanceAlong(seatNode, glm::transpose(found) * down);
                }
            }
        }
    }
    double sagittalLock = 1.0;
    glm::vec3 lateralAxis(1.0f, 0.0f, 0.0f);
    if (trunkDrag && goalPoint != nullptr) {
        lateralAxis = glm::vec3(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
        const glm::vec3 travel = *goalPoint - m_jsStartTarget;
        const float sideways = std::abs(glm::dot(travel, lateralAxis)) / std::max(glm::length(travel), 0.03f * rig.sizeScale());
        static const double kLock = envOr("IK_JS_SAGITTALLOCK", kSagittalLock);
        sagittalLock = glm::mix(kLock, 1.0, static_cast<double>(glm::smoothstep(0.10f, 0.35f, sideways)));
    }
    double kneelTilt = 1.0; // (see ON HER KNEES THE PELVIS'S TILT IS DEAR, below)
    {
        static const double kTiltScale = envOr("IK_JS_KNEELTILT", kKneelTiltScale);
        float landedMost = 0.0f;
        for (std::size_t p = 0; p < rig.pins().size() && effector == root; ++p) {
            const IkEffector& pin = rig.pins()[p];
            if (rig.pinIsLive(p) && pin.hard && !rig.pinUnderEffector(p) && pin.node >= 0) {
                const float above = m_poseGlobal[static_cast<std::size_t>(pin.node)][3].y - pin.target.y;
                landedMost = std::max(landedMost, 1.0f - glm::smoothstep(0.003f * rig.sizeScale(), 0.04f * rig.sizeScale(), above));
            }
        }
        // (EASED over the ticks, a seventh more or less a tick: the tilt a descent has gathered by
        // the time its knees land is given back over a fifth of a second, not in the landing's
        // two ticks — 9cm at the brow in one of them, on the generation that gathers most.)
        // LYING FORWARD OFF HER KNEES (kProneFootFreeFrom .. Full): a pelvis drag that takes the
        // hips AHEAD of a planted knee is a body lowered onto its belly — the pelvis must pitch
        // face-down there (the thighs point back to the knees, and a hip extends 35 degrees at
        // most), and dear on its knees it could not: the thigh met its extension limit with the
        // pelvis 22 degrees from upright, and the hips stopped 30cm over the floor, 30cm from
        // their cursor, on every rig. The tilt's price fades with the hips' travel ahead of the
        // knee, and so do the rows of the foot behind it (see THE FEET COME UP, the pins).
        m_jsProneShare = 0.0f;
        static const bool kNoProne = std::getenv("IK_JS_NO_PRONE") != nullptr; // A/B probe
        for (std::size_t p = 0; p < rig.pins().size() && effector == root && !kNoProne; ++p) {
            const IkEffector& pin = rig.pins()[p];
            if (!rig.pinIsLive(p) || pin.node < 0 || rig.isGirdleJoint(pin.node)) {
                continue;
            }
            if (!rig.isLegJointAboveFoot(pin.node)) { // (a knee: a foot-class joint below it)
                continue;
            }
            const glm::vec3 fore(std::sin(m_jsRootHeading), 0.0f, std::cos(m_jsRootHeading));
            const glm::vec3 knee(m_poseGlobal[static_cast<std::size_t>(pin.node)][3]);
            const glm::vec3 hips(m_poseGlobal[static_cast<std::size_t>(root)][3]);
            m_jsProneShare = std::max(m_jsProneShare, glm::smoothstep(kProneFootFreeFrom * rig.sizeScale(),
                                                                    kProneFootFreeFull * rig.sizeScale(), glm::dot(hips - knee, fore)));
        }
        const double wanted = glm::mix(1.0, kTiltScale, static_cast<double>(landedMost) * (1.0 - static_cast<double>(m_jsProneShare)));
        static const double kTiltEase = envOr("IK_JS_KNEELTILTEASE", 1.15);
        m_jsKneelTilt = glm::clamp(wanted, m_jsKneelTilt / kTiltEase, m_jsKneelTilt * kTiltEase);
        kneelTilt = m_jsKneelTilt;
    }
    for (std::size_t b = 0; b < n; ++b) {
        if (!dofBone[b]) {
            continue;
        }
        const Bone& bone = m_bones[b];
        for (int a = 0; a < 3; ++a) {
            const float range = bone.rotMax[a] - bone.rotMin[a];
            if (bone.rotLimited[a] && range < kLockedRangeDeg) {
                continue;
            }
            // (CONTRAPPOSTO's spine: the SIDE-BEND channels alone — the ones the S-curve is made of,
            // about the fore axis. The chain is among the unknowns only for the pelvis's roll to be
            // taken up; with its flexion and twist in too, the balance row had the whole trunk as a
            // lever under a knee drag, and as the foot came back down and the support regrew it
            // pitched the trunk 9 degrees in one tick — 131mm at the brow, 91 at a shoulder.)
            if (contrapposto > 1.0e-3f && inSpine[b] && !active[b] &&
                std::abs(glm::dot(glm::vec3(bone.orient[a]), glm::vec3(0.0f, 0.0f, 1.0f))) < 0.7f) {
                continue;
            }
            const double rangeDeg =
                bone.rotLimited[a] ? std::max(static_cast<double>(range), kMinRangeDeg) : kFreeRangeDeg;
            const double rangeRad = rangeDeg * 3.14159265358979323846 / 180.0;
            double s = 1.0 / (rangeRad * rangeRad);
            s *= cls[b] == 1 ? kLimbStiffness : cls[b] == 2 ? kPinLimb : kTrunk;
            // (The PELVIS BONE — the girdle joint between the root and the thigh sockets, no spine
            // bone — is locked with the spine: it is the trunk's other way out of the plane. A sit-up
            // dragged past what a seated trunk can reach, both thighs at their flexion limit, bought
            // its last centimetres with 10 degrees of pelvis twist and 10 of side bend, the legs
            // folded over to one side under planted feet: invisible from the side, and found only
            // once the active set's hair rule let that solve move at all.)
            static const bool kNoPelvisSagittal = std::getenv("IK_JS_NO_PELVIS_SAGITTAL") != nullptr; // A/B probe
            const bool pelvisBone = !kNoPelvisSagittal && rig.isPelvisBone(static_cast<int>(b));
            if ((inSpine[b] || pelvisBone) && sagittalLock > 1.0 &&
                std::abs(glm::dot(glm::vec3(bone.orient[a]), glm::vec3(1.0f, 0.0f, 0.0f))) < 0.7f) {
                s *= sagittalLock; // (a channel that is not the trunk's flexion: see kSagittalLock)
            }
            if (girdle[b]) {
                static const double kGirdle = envOr("IK_JS_ELBOWGIRDLE", kElbowGirdleStiffness);
                s *= kGirdle;
            }
            // ON HER KNEES THE PELVIS'S TILT IS DEAR (kKneelTiltScale, over the root's pitch and the
            // PELVIS BONE's — the bone between the root and the thigh sockets, a joint of the girdle
            // that is no spine bone: the two are one anatomical thing). Hips pushed lower than rigid
            // thighs allow over knees that are ON THE FLOOR cannot go there — a kneeling body gets
            // lower by sitting BACK — and against a pull five times a limb's the 7cm were found by
            // tipping the pelvis instead: 27 degrees at the root and 25 more, its limit, at the
            // pelvis bone, under an arched back. With the knees planted the tilt costs five times as
            // much, coming in with the landing: she stops where her thighs stop her. (A global price
            // was tried first: at 240 every generation's SIT, whose pelvis must roll back, stopped
            // 20cm short. Found by LOOKING at the in-app kneel; no gate measured it.)
            static const bool kNoPelvisBonePrice = std::getenv("IK_JS_NO_PELVIS_BONE_PRICE") != nullptr; // A/B probe
            if (!kNoPelvisBonePrice && effector == root && rig.isPelvisBone(static_cast<int>(b)) && kneelTilt > 1.0) {
                s = std::max(s, kRootRot * kneelTilt);
            }
            if (static_cast<int>(b) == root && !rootTurns && contrapposto > 1.0e-3f &&
                std::abs(glm::vec3(bone.orient[a]).z) >= 0.9f) {
                // CONTRAPPOSTO: the pelvis's ROLL alone — the channel about the fore axis — at the
                // pelvis drag's squared price over the tilt's share, following its reference
                // (swayEuler). Nothing else of the root's rotation: let through at the sway's
                // prices (pitch cheap, as under a pelvis drag) the balance row pitched the pelvis
                // 12 degrees back under a knee raise and every lifted-foot phase popped 100-190mm.
                static const double kSquare = envOr("IK_JS_ROOTSQUARE", kRootSquareness);
                s = kRootRot * kSquare / static_cast<double>(contrapposto);
            } else if (static_cast<int>(b) == root && !rootTurns) {
                // (The bow's hinge alone: the pelvis's PITCH, and nothing else of its rotation — and
                // only while a hinge is ON: the root is marked an unknown by CONTRAPPOSTO too, whose
                // roll the branch above prices, and its pitch must then stay out — priced here at
                // kHinge over a hinge share of ZERO it went infinitely stiff, and every lifted-foot
                // drag stood still.)
                if (std::abs(glm::vec3(bone.orient[a]).x) < 0.9f || m_jsTrunkHinge <= 1.0e-3f) {
                    continue;
                }
                static const double kStanding = envOr("IK_JS_TRUNKHINGE", kTrunkHingeStiffness);
                static const double kKneeling = envOr("IK_JS_KNEELFOLD", kKneelFoldStiffness);
                // (A hand's REACH DOWN holds its pitch to a folding REFERENCE at kReachHingeStiffness —
                // firm enough that the reference decides the fold, soft enough that the cursor can
                // bend it. Priced at the bow's 20 with the spine cheapened to follow, the price alone
                // decided: a hand to the knee arched her BACKWARD 9-23 degrees, and a hand to the
                // floor folded the trunk 130, the head hanging at the knees.)
                static const double kReach = envOr("IK_JS_REACHHINGE", kReachHingeStiffness);
                const double        kHinge = m_jsKneelStart ? kKneeling : (m_jsReachHinge > 1.0e-3f && !trunkDrag ? kReach : kStanding);
                s = kHinge / static_cast<double>(m_jsTrunkHinge);
            } else if (static_cast<int>(b) == root) {
                s = effector == root ? kRootRot * kneelTilt : kRootRot / static_cast<double>(rising);
                if (m_jsTrunkHinge > 1.0e-3f && std::abs(glm::vec3(bone.orient[a]).x) >= 0.9f) {
                    static const double kStanding = envOr("IK_JS_TRUNKHINGE", kTrunkHingeStiffness);
                    static const double kKneeling = envOr("IK_JS_KNEELFOLD", kKneelFoldStiffness);
                    const double        kHinge = m_jsKneelStart ? kKneeling : kStanding;
                    s = std::min(s, kHinge / static_cast<double>(m_jsTrunkHinge));
                }
                // (A pelvis PITCHES readily — a sit rolls it back, a kneel and a deep fold tip it
                // forward — but it stays SQUARE: its heading and its level are the stance's, and
                // cost kRootSquareness times its pitch. At one price, a lunge pressed down onto
                // its back knee answered the last centimetres with 11 degrees of pelvis roll.)
                static const double kSquare = envOr("IK_JS_ROOTSQUARE", kRootSquareness);
                const float lateral = std::abs(glm::vec3(bone.orient[a]).x);
                s *= glm::mix(kSquare, 1.0, static_cast<double>(glm::smoothstep(0.5f, 0.9f, lateral)));
            }
            if ((cls[b] == 2 || cls[b] == 1) && twistPriced[b] && m_jsTwistAxis[b] == a) {
                s *= kTwistS;
            }
            static const bool kLiftedTwistFree = std::getenv("IK_JS_KNEE_LIFT_TWIST_FREE") != nullptr; // A/B probe
            if (!kLiftedTwistFree && kneeLegBone[b] && plant < 1.0f) {
                // (The price is the MOTION's, not the channel's name — 2026-09-23, IK_JS_TWIST_BY_NAME:
                // a thigh's abduction channel lies about the fore axis INSIDE its flexion, and with
                // the thigh flexed to horizontal that axis runs along the thigh: the channel IS a
                // twist there, at a swing channel's price. Rolling the thigh 22 degrees in one tick
                // — the shin's side channel crossing its whole range to keep the foot — was the
                // cheapest way to a knee goal 3cm inboard at the top of the male rig's draw, found
                // after eight ticks of a knee held short: a toe 88mm in a tick. A channel whose
                // axis, as it stands, lies within kTwistAlongCos of the bone's own direction carries
                // the lifted leg's twist price, geometrically with how far along it lies.)
                static const bool kTwistByName = std::getenv("IK_JS_TWIST_BY_NAME") != nullptr; // A/B probe
                double share = m_jsTwistAxis[b] == a ? 1.0 : 0.0;
                if (!kTwistByName && share < 1.0) {
                    float longest = 0.02f;
                    glm::vec3 segment(0.0f);
                    for (const int ch : m_children[b]) {
                        const glm::vec3 offset(m_bones[static_cast<std::size_t>(ch)].localBind[3]);
                        if (glm::length(offset) > longest) {
                            longest = glm::length(offset);
                            segment = offset / longest;
                        }
                    }
                    if (glm::dot(segment, segment) > 0.5f) {
                        // the channel's axis after the rotations composed before it, and the
                        // segment as the bone turns it — both in the parent's frame
                        glm::mat3 prefix(1.0f);
                        for (const char ch : bone.rotationOrder) {
                            const int axis = ch == 'X' ? 0 : ch == 'Y' ? 1 : ch == 'Z' ? 2 : -1;
                            if (axis < 0 || axis == a) {
                                break;
                            }
                            glm::vec3 e(0.0f);
                            e[axis] = 1.0f;
                            prefix = prefix * glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(m_boneEuler[b][axis]), e));
                        }
                        glm::vec3 e(0.0f);
                        e[a] = 1.0f;
                        const glm::vec3 axisNow = glm::mat3(bone.orient) * prefix * e;
                        const glm::vec3 segNow = glm::mat3(bone.orient) * glm::mat3(eulerMatrix(m_boneEuler[b], bone.rotationOrder)) * glm::mat3(bone.invOrient) * segment;
                        const float along = std::abs(glm::dot(glm::normalize(axisNow), glm::normalize(segNow)));
                        share = static_cast<double>(glm::smoothstep(kTwistAlongFrom, kTwistAlongFull, along));
                    }
                }
                s *= std::pow(kTwistS, static_cast<double>(1.0f - plant) * share); // (the lifted leg's twist price, geometrically with the release)
            }
            JointSolverDof dof;
            dof.bone = static_cast<int>(b);
            dof.axis = a;
            dof.stiffness = s;
            dof.reference = m_ikStartEuler[b][a];
            // A bone a POST-STEP has turned (a hanging arm, the righted neck) that is now the
            // SOLVE's — a hanging hand has come down on the floor and is a contact; the body has
            // been lifted — is referred to where the post-step LEFT it. Referred to its drag-start
            // value, the arm of a figure reclined from a sit (the hang had turned its shoulder 45
            // degrees by then) was snapped back the tick its hand's contact let go: 70cm at the
            // fingertips in ONE tick, and 30 and 15 in the two after (found by the in-app floor
            // gallery, 2026-09-21; no harness phase lands a hanging hand).
            if (b < m_jsPostStepTurned.size() && m_jsPostStepTurned[b]) {
                dof.reference = m_jsPostStepEuler[b][a];
            }
            if (static_cast<int>(b) == root && rising > 0.0f) {
                dof.reference = glm::mix(m_ikStartEuler[b][a], m_jsRootUprightEuler[a], risen);
            }
            if (static_cast<int>(b) == root && swayValid) {
                dof.reference = swayEuler[a]; // the hip sway's roll (above)
            }
            if (static_cast<int>(b) == root && seatRollDeg != 0.0 && std::abs(glm::vec3(bone.orient[a]).x) >= 0.9f) {
                // (THE SEAT ROLLS, above: the pitch channel's reference goes with the target.)
                dof.reference += static_cast<float>(seatRollDeg) * (glm::vec3(bone.orient[a]).x < 0.0f ? -1.0f : 1.0f);
            }
            if (static_cast<int>(b) == root && m_jsReachHinge > 1.0e-3f && !trunkDrag &&
                std::abs(glm::vec3(bone.orient[a]).x) >= 0.9f) {
                // REACHING DOWN (ikTrunkPolicy): the pelvis's pitch REFERENCE folds forward with the
                // hand's downward travel — kReachFoldPerMetre degrees a metre, kReachFoldMaxDeg at
                // most, by the reach's share. A price can only prefer less travel; what a person does
                // reaching down is given here, and the squat, the knees and the balance find the rest.
                static const double kPerMetre = envOr("IK_JS_REACHFOLD", kReachFoldPerMetre);
                static const double kMaxDeg = envOr("IK_JS_REACHFOLDMAX", kReachFoldMaxDeg);
                const float fold = static_cast<float>(std::min(kMaxDeg, kPerMetre * static_cast<double>(m_jsReachTravel))) * m_jsReachHinge;
                dof.reference += fold * (glm::vec3(bone.orient[a]).x < 0.0f ? -1.0f : 1.0f);
            }
            // RISING, the planted legs and the bone they hang from go HOME too: their posture
            // reference turns from the pose the drag began in to the rest stance, and the legs
            // (all but free otherwise) get a little stiffness to make it count. What a standing
            // leg does is dictated by its pins and the hip; the reference only picks among the
            // poses that serve them — pelvis bone tilted with the thighs flexed against it, or
            // upright; knees bent on lifted heels, or straight on flat feet. A kneel leaves the
            // pelvis bone 21 degrees forward (a stiff trunk joint: it stayed there), and she stood
            // up with her knees bent 15 degrees and her heels 2cm up. Not the limb's TWIST
            // channels: a turned-out stance is the user's, and the feet's heading is held hard.
            // (The PINNED joint's own priced channel does go home: the foot's heading is the sole
            // row's to hold, and on four figure generations that channel is one the foot needs
            // to come down flat — left at the kneel's value, their heels stayed 4-13cm up.)
            const bool limbTwist = twistPriced[b] && m_jsTwistAxis[b] == a && !pinNode[b];
            // (A standing foot's limb only: a hand planted on the floor serves a pin too, and
            // sent "home" its arm swung toward the bind pose as she got up off all fours.)
            if (rising > 0.0f && static_cast<int>(b) != root && (homeLimb[b] || pinJunction[b]) && !limbTwist) {
                dof.reference = glm::mix(m_ikStartEuler[b][a], 0.0f, risen);
                if (homeLimb[b]) {
                    static const double kRiseHome = envOr("IK_JS_RISEHOME", kRiseHomeStiffness);
                    dof.stiffness += kRiseHome * static_cast<double>(risen);
                }
            }
            // A KNEELING FOOT LIES FLAT (the pin rows set m_jsFootFlat / m_jsFootFlatRef, one tick
            // before): the ankle's pitch channel is REFERRED to full plantar flexion over the
            // foot's share, at kKneelFlatStiffness — the top of the foot comes down onto the floor,
            // toes back, and the floor's rows stop it there. (A price can only prefer less travel;
            // what a kneeling foot does is given.)
            if (b < m_jsFootFlat.size() && m_jsFootFlat[b] > 0.0f && b < m_jsFootFlatAxis.size() && m_jsFootFlatAxis[b] >= 0 &&
                b < m_jsFootFlatRefValid.size() && m_jsFootFlatRefValid[b]) {
                static const double kFlatStiffness = envOr("IK_JS_KNEELFLAT", kKneelFlatStiffness);
                const float share = m_jsFootFlat[b];
                dof.reference = dof.reference + (m_jsFootFlatRef[b][a] - dof.reference) * share;
                dof.stiffness += kFlatStiffness * static_cast<double>(share);
            }
            if (b < m_jsToeFlatShare.size() && m_jsToeFlatShare[b] > 0.0f && b < m_jsToeFlatValid.size() && m_jsToeFlatValid[b]) {
                static const double kFlatStiffness = envOr("IK_JS_KNEELFLAT", kKneelFlatStiffness);
                const float share = m_jsToeFlatShare[b];
                dof.reference = dof.reference + (m_jsToeFlatRef[b][a] - dof.reference) * share; // (the toes and mid-foot lie straight)
                dof.stiffness += kFlatStiffness * static_cast<double>(share);
            }
            // A FOLD channel (a knee, an elbow: a wide range to one side of straight, a few
            // degrees of hyperextension to the other): IK never takes it further past straight
            // than the pose already is (JointSolverDof::foldSign has the why). A fold joint is a
            // HINGE: its other channel, the twist excepted, is narrow (kFoldHingeOtherDeg) — a
            // knee's side channel spans 10 degrees, an elbow's is locked. The range test alone
            // took the two oldest generations' THIGH for one: their abduction channel spans
            // -85..15, and 15 is within a fifth of 85, so a standing thigh could never adduct
            // past straight under IK — asked to put the pelvis over its foot it TWISTED 12
            // degrees instead, and the foot pivoted a centimetre on its ball (found by the
            // weight shift, 2026-09-25; the six other generations author 20-26 degrees there).
            static const bool kFoldByRangeAlone = std::getenv("IK_JS_FOLD_RANGE_ONLY") != nullptr; // A/B probe
            bool hinge = true;
            for (int o = 0; o < 3 && hinge && !kFoldByRangeAlone; ++o) {
                if (o == a || m_jsTwistAxis[b] == o) {
                    continue;
                }
                const float otherRange = bone.rotLimited[o] ? bone.rotMax[o] - bone.rotMin[o] : 360.0f;
                hinge = otherRange <= kFoldHingeOtherDeg;
            }
            if (hinge && bone.rotLimited[a] && range >= kFoldRangeDeg && bone.rotMin[a] < 0.0f &&
                bone.rotMax[a] > 0.0f) {
                const float narrow = std::min(-bone.rotMin[a], bone.rotMax[a]);
                const float wide = std::max(-bone.rotMin[a], bone.rotMax[a]);
                if (narrow <= kFoldNarrowFraction * wide) {
                    dof.foldSign = bone.rotMax[a] > -bone.rotMin[a] ? 1 : -1;
                    dof.foldRetry = !landedArm[b];
                    if (dof.foldSign > 0) {
                        dof.minDeg = std::min(m_boneEuler[b][a], 0.0f);
                    } else {
                        dof.maxDeg = std::max(m_boneEuler[b][a], 0.0f);
                    }
                }
            }
            // THE COLLAR ELEVATES kCollarElevationMaxDeg AT MOST under a hand drag (2026-09-25): its
            // elevation channel's box is narrowed to +-that about the bind — never tighter than where
            // the collar already stands, the fold bound's idiom. The rigs author 55 degrees, a
            // clavicle rises about 30; past it the shrug was a socket hauled onto the neck. (Its
            // twist and swing stay at the plain price: they are what carry an arm smoothly from one
            // basin to the next — priced dear, a hand raised beside the head flipped 136mm.)
            // (A standing body's: hanging by the hand, the shoulder girdle is stretched to whatever the
            // rig allows — with the box the two oldest generations' rigid hang rested 81-87mm short of
            // the cursor, 52-66 before.)
            int handCollarAxis = -1; // (this bone's, if it is a dragged arm's collar)
            for (std::size_t k = 0; k < handCollars.size(); ++k) {
                if (handCollars[k] == static_cast<int>(b)) {
                    handCollarAxis = handCollarAxes[k];
                }
            }
            if (handCollarAxis >= 0 && !rig.suspended()) {
                static const double kCollarMax = envOr("IK_JS_COLLARMAX", kCollarElevationMaxDeg);
                static const double kCollarOther = envOr("IK_JS_COLLAROTHERMAX", kCollarOtherMaxDeg);
                const double cap = a == handCollarAxis ? kCollarMax : kCollarOther; // (its swing and twist: what a clavicle does)
                if (cap > 0.0) {
                    dof.minDeg = std::max(dof.minDeg, std::min(static_cast<double>(m_boneEuler[b][a]), -cap));
                    dof.maxDeg = std::min(dof.maxDeg, std::max(static_cast<double>(m_boneEuler[b][a]), cap));
                }
            }
            // A PLANTED LEG'S THIGH ABDUCTS kThighAbductBoxDeg AT MOST (2026-10-27): the thigh of a
            // foot the rig has planted (a standing foot model: its ball found) has its ABDUCTION
            // channel — the free channel about the fore axis, neither the twist nor the flexion —
            // boxed to +-that about the bind, never tighter than where it stands. (A lifted leg's
            // thigh, a dragged foot's: the rig's own limit — a side kick, a wide step.)
            if (plantedThigh[b] && a == abductAxis[b] && !rig.suspended()) {
                static const double kAbductFlexed = envOr("IK_JS_THIGHABDUCT", kThighAbductBoxDeg);
                static const double kAbductStraight = envOr("IK_JS_THIGHABDUCT_STRAIGHT", kThighAbductBoxStraightDeg);
                // (By the thigh's flexion AND by the pelvis's descent: a kneel's abduction was spent
                // in the descent's first centimetres, the thigh still near straight and the box at
                // its straight width, and a box never tighter than where the thigh stands could not
                // take it back once the thigh was flexed — the knees still landed 15cm outside the
                // feet. m_jsTargetDown is the target's downward travel since the press.)
                const float  flexion = std::abs(m_boneEuler[b][static_cast<std::size_t>(flexAxis[b])]);
                const float  tight = std::max(glm::smoothstep(kThighAbductFlexFromDeg, kThighAbductFlexFullDeg, flexion), m_jsTargetDown);
                const double kAbductBox = glm::mix(kAbductStraight, kAbductFlexed, static_cast<double>(tight));
                if (kAbductBox > 0.0) {
                    dof.minDeg = std::max(dof.minDeg, std::min(static_cast<double>(m_boneEuler[b][a]), -kAbductBox));
                    dof.maxDeg = std::min(dof.maxDeg, std::max(static_cast<double>(m_boneEuler[b][a]), kAbductBox));
                    static const bool kBoxTrace = std::getenv("IK_JS_ABDUCT_TRACE") != nullptr;
                    if (kBoxTrace) {
                        std::fprintf(stderr, "[js-abduct] %s axis %d boxed [%.1f, %.1f] at %.1f\n", m_boneNames[b].c_str(), a, dof.minDeg, dof.maxDeg,
                                     m_boneEuler[b][a]);
                    }
                }
            }
            problem.dofs.push_back(dof);
        }
    }
    // Under a SLIDE the root's reference goes with the STANCE: its centre (a foot moved d along
    // the floor moves the middle of the supports d / their number) and its HEIGHT (as tall as
    // the legs' length allows over the feet where they now are, less what the pose the drag
    // began in was short of that). The price alone cannot say either — it only ever prefers
    // LESS travel: at any price the hips stayed by the standing foot and the reach was paid in
    // its knee (a 40cm side step was a side lunge: the standing knee bent 53 degrees, the head
    // 11cm down), and a wide stance brought back in left both knees bent 30 degrees under hips
    // that stayed 3cm low (a new drag's home is the pose it begins in). A height simply made
    // FREE was tried: a foot lifted off the floor then took the hips up with it for the first
    // three centimetres, onto the standing foot's toes, before its knee gave.
    glm::vec3 slideShift(0.0f);
    static const bool kNoCentre = std::getenv("IK_JS_NO_SLIDE_CENTRE") != nullptr; // A/B probe
    if (slide > 0.0f && !rig.suspended() && !kNoCentre) {
        std::size_t supports = 1;
        for (std::size_t p = 0; p < pins.size(); ++p) {
            supports += pins[p].node >= 0 ? 1 : 0;
        }
        const glm::vec3 travel = *goalPoint - m_jsStartTarget;
        slideShift.x = slide * travel.x / static_cast<float>(supports);
        slideShift.z = slide * travel.z / static_cast<float>(supports);
        // The root height at which a leg is straight: over its ankle at `ankleAt`, the root at
        // `rootAt` (the thigh joint rides the root as the pose has it).
        const glm::vec3 rootNow(m_poseGlobal[static_cast<std::size_t>(root)][3]);
        const auto straightHeight = [&](int ankle, const glm::vec3& rootAt, const glm::vec3& ankleAt) {
            const int junction = rig.limbJunction(ankle);
            int top = ankle;
            float length = 0.0f;
            for (;;) {
                const int par = m_bones[static_cast<std::size_t>(top)].parent;
                if (par < 0 || par == junction || par == root) {
                    break;
                }
                length += glm::length(glm::vec3(m_bones[static_cast<std::size_t>(top)].localBind[3]));
                top = par;
            }
            const glm::vec3 socket = glm::vec3(m_poseGlobal[static_cast<std::size_t>(top)][3]) - rootNow;
            const glm::vec2 across(rootAt.x + socket.x - ankleAt.x, rootAt.z + socket.z - ankleAt.z);
            return ankleAt.y + std::sqrt(std::max(0.0f, length * length - glm::dot(across, across))) - socket.y;
        };
        const glm::vec3 rootStart = m_jsEffectorStartPos - m_jsStartGrabFromRoot;
        glm::vec3 rootGoes = rootStart + glm::vec3(rig.stanceShift()) ;
        rootGoes.x += slideShift.x;
        rootGoes.z += slideShift.z;
        float tallStart = straightHeight(effector, rootStart, m_jsEffectorStartPos);
        float tallNow = straightHeight(effector, rootGoes, *goalPoint);
        for (std::size_t p = 0; p < pins.size(); ++p) {
            const int node = pins[p].node;
            if (node < 0 || rig.pinIsLive(p) || rig.pinIsUser(p) ||
                m_ikBindPos[static_cast<std::size_t>(node)].y >= 0.20f * legScale) {
                continue; // a standing FOOT's leg only
            }
            tallStart = std::min(tallStart, straightHeight(node, rootStart, pins[p].target));
            tallNow = std::min(tallNow, straightHeight(node, rootGoes, pins[p].target));
        }
        slideShift.y = slide * glm::clamp(tallNow - tallStart, -0.35f * legScale, 0.35f * legScale);
    }
    // The root's home: over the stance as the rig has walked it, and on from there by the follow.
    glm::vec3 rootHome(rig.stanceShift());
    rootHome.x += m_jsTrunkFollow.x;
    rootHome.z += m_jsTrunkFollow.y;
    rootHome.x += m_jsUnfoldShift.x; // (THE UNFOLD BRINGS THE HIPS UNDER: the follow)
    rootHome.z += m_jsUnfoldShift.y;
    rootHome.y += seatRollLift; // (THE SEAT ROLLS: the hip joint's resting height at the rolled pitch)
    rootHome += m_jsKneelSeatShift; // (THE HIPS COME UP TO THE SEAT, over the knees, under a kneel-up: dragIkTick)
    rootHome.y += m_jsTiptoe;       // (THE TIPTOE: a hand pulled up beyond its reach lifts the heels — dragIkTick; each heel's target rises with it in ikPinRows)
    // THE WEIGHT SHIFT: a foot lifted off a two-footed stance takes the hips over the STANDING
    // foot — by the LIFT (a foot drag's 1 - slide, a knee drag's 1 - plant: functions of the
    // target), measured from the lift the drag BEGAN with (a foot already in the air shifts
    // nothing: the drag-start pose is the contract), toward where the centre of mass as the drag
    // found it would stand over the standing foot's footprint (m_jsWeightShift, a constant of the
    // drag: beginIkDrag). A price can only prefer less travel, and balance never asks back the
    // imbalance a drag begins with — measured with the lifted foot OUT of the support, a knee
    // raise began 10cm "out" and stayed there: she stood on one leg with her weight beside the
    // standing foot, the leg hanging plumb from its socket and the pelvis a hand's width to the
    // lifted side, a stance that would topple (every knee raise, kick, step-up and foot lifted
    // behind in the galleries). A person shifts her weight onto the standing foot BEFORE the
    // other leaves the floor; here the two go together, and a raise brought back down brings the
    // hips back with it.
    static const bool kNoWeightShift = std::getenv("IK_JS_NO_WEIGHT_SHIFT") != nullptr; // A/B probe
    static const double kShiftShare = envOr("IK_JS_WEIGHTSHIFT", kWeightShiftShare);
    {
        const float lift = s.weightLift; // (m_jsWeightLift with it: computed at the top of this stage, where CONTRAPPOSTO reads it too)
        // (Approached at the PINS' pace, kPinEaseStep: the foot lets go over 7-9cm of the target's
        // lift, and the home went 24cm with it on the base rig — three and a half times the
        // cursor's speed, a body sliding sideways in a dozen ticks under a quick lift. The pose it
        // ends in is the same; the settle waits for the home to arrive, as it does for a pin.)
        const glm::vec2 want = (!kNoWeightShift && !rig.suspended())
                                   ? static_cast<float>(kShiftShare) * m_jsWeightLift * m_jsWeightShift
                                   : glm::vec2(0.0f);
        const glm::vec2 gap = want - m_jsWeightEased;
        const float gapLen = glm::length(gap);
        const float step = kPinEaseStep * legScale;
        m_jsWeightEasing = gapLen > step;
        m_jsWeightEased = m_jsWeightEasing ? m_jsWeightEased + gap * (step / gapLen) : want;
        rootHome.x += m_jsWeightEased.x;
        rootHome.z += m_jsWeightEased.y;
        static const bool kWeightTrace = std::getenv("IK_JS_WEIGHT_TRACE") != nullptr;
        if (kWeightTrace && (m_jsWeightLift > 0.0f || m_jsWeightLiftStart > 0.0f || gapLen > 0.0f)) {
            std::fprintf(stderr, "[weight] shift(%.4f %.4f) lift %.3f start %.3f applied %.3f eased(%.4f %.4f) slide %.3f plant %.3f\n",
                         m_jsWeightShift.x, m_jsWeightShift.y, lift, m_jsWeightLiftStart, m_jsWeightLift,
                         m_jsWeightEased.x, m_jsWeightEased.y, slide, plant);
        }
    }
    // (A SCOPED drag of anything but the pelvis has no root translation among its unknowns: the
    // chain alone moves — IkScope::Chain, ikChooseUnknowns.)
    const bool rootFrozen = m_ikScope == IkScope::Chain && effector != root;
    for (int a = 0; a < 3 && !rootFrozen; ++a) {
        JointSolverDof dof;
        dof.bone = root;
        dof.axis = 3 + a;
        // A pelvis drag DRIVES the root: its posture price must not fight the cursor there.
        static const double kRootY = envOr("IK_JS_ROOTY", kRootStiffnessVertical);
        // (A SUSPENDED body goes where the hand takes it: its root is as free as a dragged one.)
        static const double kRootYield = envOr("IK_JS_ROOTYIELD", kRootYieldStiffness);
        // (The counter-yield below is the vertical price's too: a hinge from a crouch RAISES the
        // hips as the trunk folds forward at the chest's height — 11-15cm for a 15cm drag — and
        // at 1000/m^2 that was dear enough that the spine wrung itself to its twist limits
        // instead, a helix being another way to keep a folded trunk's top up.)
        static const double kCounterY = envOr("IK_JS_TRUNKCOUNTER", kTrunkCounterStiffness);
        // (... and ON HER KNEES the horizontal price yields with the vertical one: a kneeling body
        // takes no step and keeps no balance, and its hips go where the trunk takes them — pushed
        // DOWN from all fours by the chest, the hips must come forward over the planted knees to
        // lie prone, and at 4500/m^2 they came 2cm: the chest stopped 39cm short.)
        static const bool kNoKneelYield = std::getenv("IK_JS_NO_KNEEL_HORIZONTAL_YIELD") != nullptr; // A/B probe
        const double kneelYield = (!kNoKneelYield && m_jsKneelStart && trunkDrag) ? static_cast<double>(m_jsKneelYield) : 0.0;
        // (A trunk taken UP off all fours: the hips' HOME goes under the target (m_jsKneelSeatShift,
        // dragIkTick), the prices stay. Yielding the horizontal price to the target's uprightness
        // instead was tried and put back behind IK_JS_KNEEL_UP_YIELD: the hips went where the
        // pull took them, back and down on the oldest generation, 78mm short.)
        static const bool kKneelUpYields = std::getenv("IK_JS_KNEEL_UP_YIELD") != nullptr; // A/B probe: the rejected yield
        const double kneelUpYield = (kKneelUpYields && m_jsKneelStart && trunkDrag) ? static_cast<double>(m_jsKneelUpright) : 0.0;
        // (Under a HAND pushed down the vertical yield is kReachSquatShare of the push-down's: the
        // knee is the body's cheapest joint, and at a chest push's yield a hand brought to the knee
        // was a squat with nothing folding — the hinge (REACHING DOWN, ikTrunkPolicy) must be able
        // to compete. The rise's yield stays whole: a hand raised out of a crouch stands her up.)
        static const double kReachSquat = envOr("IK_JS_REACHSQUAT", kReachSquatShare);
        const double rootYieldUsed = m_jsReachHinge > 1.0e-3f
                                         ? std::max(static_cast<double>(m_jsRootRise), static_cast<double>(m_jsReachDown) * kReachSquat)
                                         : static_cast<double>(m_jsRootYield);
        const double vertical = kRootY * std::pow(kRootYield / kRootY, std::max(rootYieldUsed, kneelYield)) *
                                std::pow(std::min(1.0, kCounterY / kRootY), static_cast<double>(m_jsTrunkCounter));
        static const double kCounter = envOr("IK_JS_TRUNKCOUNTER", kTrunkCounterStiffness);
        const double horizontal = kRootS * std::pow(kRootYield / kRootS, std::max({static_cast<double>(m_jsRootRise), kneelYield, kneelUpYield})) *
                                  std::pow(std::min(1.0, kCounter / kRootS), static_cast<double>(m_jsTrunkCounter)) *
                                  // (... and under a hand's REACH DOWN the hips go BACK as the trunk folds forward:
                                  // the counter's price, by the reach's share — its horizontal half alone.)
                                  std::pow(std::min(1.0, kCounter / kRootS), static_cast<double>(m_jsReachHinge));
        dof.stiffness = (effector == root || rig.suspended()) ? 1.0 : (a == 1 ? vertical : horizontal);
        if (slide > 0.0f && !rig.suspended()) {
            static const double kLegRoot = envOr("IK_JS_LEGROOT", kLegDragRootStiffness);
            dof.stiffness *= std::pow(std::min(1.0, kLegRoot / dof.stiffness), static_cast<double>(slide));
        }
        // The reference WALKS with the stance (IkRig::stanceShift; model space — the root's
        // parent chain carries no rotation worth the name on a figure node).
        dof.reference = m_jsStartTranslation[static_cast<std::size_t>(root)][a] + rootHome[a];
        dof.reference += static_cast<double>(slideShift[a]); // (the stance's centre and height)
        problem.dofs.push_back(dof);
    }
}

/// The spine's COUPLING rows (the neck as its top, the pelvis's hinge as its base) and the posture EASING.
void Armature::ikSpineCoupling(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    JointSolver::Problem& problem = s.problem;
    double& seatRollDeg = s.seatRollDeg;

    // THE SPINE BENDS AS ONE (kSpineCoupling): each pair of neighbours in the spine chain — from
    // the root up through the child with the biggest foot-free subtree to the first bone three
    // big branches leave — is held, softly and channel by channel, to the same SHARE of its
    // range away from where the drag found it. Four independent hinges made an S-curve as
    // cheap as any other way to put the chest where it was asked: a crouching figure's chest
    // dragged forward at its own height flexed the lower spine 27 degrees and EXTENDED the chest
    // bone to its limit against it (to keep the height), stopped 4cm short, and then corkscrewed
    // — 47 degrees of spine twist for 7mm of reach; a chest pulled back curled the upper spine
    // forward as a counterweight.
    static const double kCoupling = envOr("IK_JS_SPINECOUPLING", kSpineCoupling);
    if (kCoupling > 0.0) {
        std::vector<int> dofAt(n * 3, -1);
        for (std::size_t j = 0; j < problem.dofs.size(); ++j) {
            const JointSolverDof& dof = problem.dofs[j];
            if (dof.axis < 3) {
                dofAt[static_cast<std::size_t>(dof.bone) * 3 + static_cast<std::size_t>(dof.axis)] = static_cast<int>(j);
            }
        }
        // ... and THE NECK IS THE SPINE'S TOP: its bones (the rig's neck base up to the head's
        // parent) are links of the same chain whenever they are unknowns — a head or a neck drag.
        // Priced as the dragged "limb" and uncoupled, the neck took a head drag ALONE, to its
        // limits: a head pulled 15cm back threw one generation's head back to the sky (both neck
        // bones at -25 and -27 degrees, the gaze 55 up) while another's flexed 20 degrees the
        // other way, and 15cm sideways tilted every head 50 degrees over a trunk leaned 3.
        static const bool kNoNeck = std::getenv("IK_JS_NO_NECK_COUPLING") != nullptr; // A/B probe
        static const double kNeckWeight = envOr("IK_JS_NECKCOUPLING", kNeckCoupling);
        std::vector<int> coupled(m_jsSpineChain.begin(), m_jsSpineChain.end());
        const std::size_t spineLinks = coupled.size(); // (links from here up are the neck's)
        if (!kNoNeck && coupled.size() > 1 && rig.neckBase() >= 0 && rig.headNode() >= 0) {
            std::vector<int> neck;
            for (int cur = m_bones[static_cast<std::size_t>(rig.headNode())].parent; cur >= 0;
                 cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                neck.insert(neck.begin(), cur);
                if (cur == rig.neckBase()) {
                    break;
                }
            }
            if (!neck.empty() && neck.front() == rig.neckBase() &&
                m_bones[static_cast<std::size_t>(rig.neckBase())].parent == coupled.back()) {
                coupled.insert(coupled.end(), neck.begin(), neck.end());
            }
        }
        for (std::size_t k = 1; k + 1 < coupled.size(); ++k) {
            const std::size_t lower = static_cast<std::size_t>(coupled[k]);
            const std::size_t upper = static_cast<std::size_t>(coupled[k + 1]);
            for (std::size_t a = 0; a < 3; ++a) {
                const int dl = dofAt[lower * 3 + a];
                const int du = dofAt[upper * 3 + a];
                if (dl < 0 || du < 0) {
                    continue;
                }
                const auto range = [&](std::size_t b) {
                    const Bone& bone = m_bones[b];
                    const double deg = bone.rotLimited[a]
                                           ? std::max(static_cast<double>(bone.rotMax[a] - bone.rotMin[a]), kMinRangeDeg)
                                           : kFreeRangeDeg;
                    return deg * 3.14159265358979323846 / 180.0;
                };
                JointCouplingTask task;
                task.dofA = dl;
                task.dofB = du;
                task.scaleA = 1.0 / range(lower);
                task.scaleB = 1.0 / range(upper);
                task.refA = static_cast<double>(m_ikStartEuler[lower][a]) * 3.14159265358979323846 / 180.0;
                task.refB = static_cast<double>(m_ikStartEuler[upper][a]) * 3.14159265358979323846 / 180.0;
                task.weight = k + 1 < spineLinks ? kCoupling : kNeckWeight;
                problem.couplings.push_back(task);
            }
        }
        // ... and under THE BOW the pelvis's pitch is one more link of that chain, below the first
        // spine bone: the trunk folds from the HIPS as it curls. Priced alone the split was the
        // lever's — a chest dragged down and forward curled the spine 66 degrees over a pelvis
        // pitched 8 (a hunch), a head taken the same way pitched the pelvis 26 under a spine bent
        // 1 degree a joint (a plank).
        static const double kStandingRange = envOr("IK_JS_TRUNKHINGERANGE", kTrunkHingeRangeDeg);
        static const double kKneelingRange = envOr("IK_JS_KNEELFOLDRANGE", kKneelFoldRangeDeg);
        // (... and a hand's REACH DOWN folds from the HIPS first: its coupling range is
        // kReachHingeRangeDeg — the spine curls half as much per degree of pelvis pitch as under a
        // bow, whose split was set for a chest taken down by hand. At the bow's 90 a hand taken to
        // the floor folded the trunk 130 degrees, the head hanging at the knees.)
        static const double kReachRange = envOr("IK_JS_REACHHINGERANGE", kReachHingeRangeDeg);
        const double        kHingeRange = m_jsKneelStart ? kKneelingRange
                                          : (m_jsReachHinge > 1.0e-3f && !s.trunkDrag ? kReachRange : kStandingRange);
        if (m_jsTrunkHinge > 1.0e-3f && effector != root && kHingeRange > 0.0 && m_jsSpineChain.size() > 1) {
            const std::size_t first = static_cast<std::size_t>(m_jsSpineChain[1]);
            const std::size_t rootAt = static_cast<std::size_t>(root);
            for (std::size_t ar = 0; ar < 3; ++ar) {
                const glm::vec3 rootAxis(m_bones[rootAt].orient[static_cast<int>(ar)]);
                const int dr = dofAt[rootAt * 3 + ar];
                if (dr < 0 || std::abs(rootAxis.x) < 0.9f) {
                    continue;
                }
                for (std::size_t as = 0; as < 3; ++as) {
                    const glm::vec3 spineAxis(m_bones[first].orient[static_cast<int>(as)]);
                    const int ds = dofAt[first * 3 + as];
                    if (ds < 0 || std::abs(spineAxis.x) < 0.7f) {
                        continue;
                    }
                    const Bone& bone = m_bones[first];
                    const double deg = bone.rotLimited[as]
                                           ? std::max(static_cast<double>(bone.rotMax[as] - bone.rotMin[as]), kMinRangeDeg)
                                           : kFreeRangeDeg;
                    JointCouplingTask task;
                    task.dofA = dr;
                    task.dofB = ds;
                    task.scaleA = (rootAxis.x * spineAxis.x < 0.0f ? -1.0 : 1.0) * 180.0 / (kHingeRange * 3.14159265358979323846);
                    task.scaleB = 180.0 / (deg * 3.14159265358979323846);
                    // (A seat that ROLLS shares only what it pitches over and above the roll.)
                    task.refA = (static_cast<double>(m_ikStartEuler[rootAt][ar]) + seatRollDeg * (rootAxis.x < 0.0f ? -1.0 : 1.0)) *
                                3.14159265358979323846 / 180.0;
                    task.refB = static_cast<double>(m_ikStartEuler[first][as]) * 3.14159265358979323846 / 180.0;
                    task.weight = kCoupling * static_cast<double>(m_jsTrunkHinge);
                    problem.couplings.push_back(task);
                }
            }
        }
        // THE KNEES BEND ALIKE under a hand's REACH DOWN (kReachKneeCoupling): the two standing
        // legs' fold channels are held, softly, to the same share of their range from where the
        // press found them. A deep one-handed reach left one knee straight and the other bent
        // 20-30 degrees on most rigs — a straight knee is a bound the fold retry hops about, and
        // nothing said the legs should agree — a staggered stance for a reach made square on.
        static const double kKneeCoupling = envOr("IK_JS_REACHKNEECOUPLING", kReachKneeCoupling);
        if (m_jsReachHinge > 1.0e-3f && kKneeCoupling > 0.0) {
            int    kneeBone[2] = {-1, -1};
            int    kneeAxis[2] = {0, 0};
            double kneeRange[2] = {0.0, 0.0};
            int    found = 0;
            for (std::size_t p = 0; p < rig.pins().size(); ++p) {
                const int node = rig.pins()[p].node;
                if (node < 0 || rig.pinIsLive(p) || m_ikBindPos[static_cast<std::size_t>(node)].y >= 0.20f * rig.sizeScale()) {
                    continue; // a standing FOOT's leg only
                }
                bool got = false;
                for (int cur = node; cur >= 0 && cur != root && !got; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                    const Bone& kb = m_bones[static_cast<std::size_t>(cur)];
                    for (int a = 0; a < 3 && !got; ++a) {
                        const float range = kb.rotMax[a] - kb.rotMin[a];
                        if (kb.rotLimited[a] && range >= kFoldRangeDeg && kb.rotMin[a] < 0.0f && kb.rotMax[a] > 0.0f &&
                            std::min(-kb.rotMin[a], kb.rotMax[a]) <= kFoldNarrowFraction * std::max(-kb.rotMin[a], kb.rotMax[a])) {
                            if (found < 2) {
                                kneeBone[found] = cur;
                                kneeAxis[found] = a;
                                kneeRange[found] = static_cast<double>(range);
                            }
                            ++found;
                            got = true;
                        }
                    }
                }
            }
            if (found == 2 && kneeBone[0] != kneeBone[1]) {
                const int dA = dofAt[static_cast<std::size_t>(kneeBone[0]) * 3 + static_cast<std::size_t>(kneeAxis[0])];
                const int dB = dofAt[static_cast<std::size_t>(kneeBone[1]) * 3 + static_cast<std::size_t>(kneeAxis[1])];
                if (dA >= 0 && dB >= 0) {
                    JointCouplingTask task;
                    task.dofA = dA;
                    task.dofB = dB;
                    task.scaleA = 180.0 / (kneeRange[0] * 3.14159265358979323846);
                    task.scaleB = 180.0 / (kneeRange[1] * 3.14159265358979323846);
                    task.refA = static_cast<double>(m_ikStartEuler[static_cast<std::size_t>(kneeBone[0])][kneeAxis[0]]) * 3.14159265358979323846 / 180.0;
                    task.refB = static_cast<double>(m_ikStartEuler[static_cast<std::size_t>(kneeBone[1])][kneeAxis[1]]) * 3.14159265358979323846 / 180.0;
                    task.weight = kKneeCoupling * static_cast<double>(m_jsReachHinge);
                    problem.couplings.push_back(task);
                }
            }
        }
    }

    // POSTURE EASING (see Armature::ikDamping): s (theta - ref)^2 + kappa s (theta -
    // previous)^2 is one quadratic, s (1 + kappa) (theta - ref')^2 with ref' the weighted mean
    // of the two — so the easing is a change of reference and stiffness, in the posture term's
    // own metric (every channel eases at the same rate), and the solver knows nothing of it.
    const double kappa = kDampingLevels[ikDamping()].kappa;
    if (kappa > 0.0) {
        for (JointSolverDof& dof : problem.dofs) {
            const std::size_t b = static_cast<std::size_t>(dof.bone);
            const double previous = dof.axis < 3 ? static_cast<double>(m_boneEuler[b][dof.axis])
                                                 : static_cast<double>(m_boneTranslation[b][dof.axis - 3]);
            dof.reference = (dof.reference + kappa * previous) / (1.0 + kappa);
            dof.stiffness *= 1.0 + kappa;
        }
    }
}

} // namespace pose

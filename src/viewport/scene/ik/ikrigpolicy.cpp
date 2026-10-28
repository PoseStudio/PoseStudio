/**
 * @file ikrigpolicy.cpp
 * @brief The IkRig's per-tick POLICY around the Armature's solve: the drag's INTENT (a pelvis
 *        drag below its start height or a taut limb pushed well below itself is DOWN — a planted
 *        knee then holds in full; a limb pulled above itself is UP — no new steps), the LIFT-OFF
 *        test (a sustained, mostly vertical pull on a target geometrically beyond the leash-bound
 *        body's reach releases the contact pins: SUSPENSION), the step policy's entry (the
 *        balance effort, a pelvis drag's or an out-of-reach trunk drag's stance anchor), and the
 *        landing of a step still in flight when the button comes up.
 *
 * See ikrig.cpp for the TU layout; the constants come from ikrig_constants.h. Qt-free (std + GLM).
 */

#include "ikrig.h"

#include "balancecontroller.h"
#include "ikrig_constants.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace pose {

float IkRig::balanceMargin() const {
    return kBalanceMargin * m_sizeScale;
}

bool IkRig::limbDownIntent(const glm::vec3& target, const std::vector<glm::vec3>& positions) const {
    const glm::vec3& effPos = positions[static_cast<std::size_t>(m_effector)];
    bool limbTaut = effectorIsTrunk();
    if (!limbTaut) {
        const int junction = limbJunction(m_effector);
        glm::vec3 span(0.0f);
        for (int cur = m_effector; cur >= 0 && cur != junction;
             cur = m_parents[static_cast<std::size_t>(cur)]) {
            span += m_edgeRestDir[static_cast<std::size_t>(cur)] *
                    m_edgeRestLen[static_cast<std::size_t>(cur)];
        }
        const float reach =
            junction >= 0 ? glm::length(effPos - positions[static_cast<std::size_t>(junction)])
                          : 0.0f;
        limbTaut = reach >= kDownIntentTaut * glm::length(span);
    }
    return target.y < effPos.y - kLimbDownIntentDepth * m_sizeScale && limbTaut;
}

void IkRig::updateIntent(const glm::vec3& target, const std::vector<glm::vec3>& positions) {
    if (!dragActive() || positions.size() != m_parents.size()) {
        return;
    }
    if (m_scoped) {
        // A SCOPED drag (setScoped): no intent — nothing yields, nothing rises — and no
        // suspension; the chain alone answers the cursor. The live pins keep their bounds.
        m_lastDownIntent = false;
        m_lastUpIntent = false;
        boundLivePins(positions);
        return;
    }
    if (m_effector == m_graph.root()) {
        // DOWN: the hips below where the drag found them — and NOT ON THEIR WAY BACK UP. "Below
        // the start" alone is down for as long as a drag that went down lasts, and a knee that
        // came down over its planted foot is a HARD pin while the drag goes down (boundLivePins:
        // held in full, its height too): a figure taken down onto hands and knees and back up IN
        // ONE DRAG was held to the floor by her own knees, the hips 32-36cm under the cursor. (The
        // reference rigs' all-fours never lands its knees, so no gate saw it; three custom
        // characters' did — a petite one, a dwarf, a toon.) Once the target has come kRiseAgain
        // back up from the lowest it has been, the drag is no longer going down.
        static const bool kDownForGood = std::getenv("IK_DOWN_INTENT_STICKS") != nullptr; // A/B probe
        m_lowestPelvisTarget = std::min(m_lowestPelvisTarget, target.y);
        m_lastDownIntent = target.y < m_startPose[static_cast<std::size_t>(m_pelvis)].y - kPelvisDownIntentDepth * m_sizeScale &&
                           (kDownForGood || target.y < m_lowestPelvisTarget + kRiseAgain * m_sizeScale);
        m_lastUpIntent = false;
    } else {
        m_lastDownIntent = limbDownIntent(target, positions);
        m_lastUpIntent =
            target.y > positions[static_cast<std::size_t>(m_effector)].y + kLimbUpIntentHeight * m_sizeScale && !effectorIsTrunk();
    }
    boundLivePins(positions);
    if (m_effector != m_graph.root()) {
        updateSuspension(target, positions); // a sustained beyond-reach lift releases the pins
    }
}

void IkRig::updateSuspension(const glm::vec3& target, const std::vector<glm::vec3>& positions) {
    if (m_suspended || m_pins.empty()) {
        return;
    }
    const glm::vec3& effPos = positions[static_cast<std::size_t>(m_effector)];
    // Chain length from the grab point to the root (cached at beginDrag): the body's maximum
    // reach from the (leash-bound) pelvis, and later the hang distance the root dangles at.
    const float chainLen = m_effectorChainLen;
    const glm::vec3 strain = target - effPos;
    const float strainLen = glm::length(strain);
    bool leashBound = false;
    const glm::vec3& rootPos = positions[static_cast<std::size_t>(m_pelvis)];
    // (The test measures the ROOT against the pin target although the leash itself is centered
    // at (target - leashOffset) on the socket — see IkEffector::leashOffset. The suspension and
    // arm-raise gates are calibrated on it as written: correcting it moves the lift-off
    // boundary, so it wants those two phases re-gated, not a silent fix.)
    for (const IkEffector& pin : m_pins) {
        if (pin.leashRadius > 0.0f &&
            glm::length(rootPos - pin.target) > 0.96f * pin.leashRadius) {
            leashBound = true;
            break;
        }
    }
    // The lift gate is GEOMETRIC unreachability — the target farther from the leash-bound
    // root than the fully-extended chain — NOT the drag error: a reachable up-pull must simply
    // RAISE THE ARM overhead, however briskly it is made.
    const bool beyondReach =
        glm::length(target - rootPos) > chainLen + kSuspendStrain * m_sizeScale;
    static const bool kSuspendTrace = std::getenv("IK_SUSPEND_TRACE") != nullptr;
    if (kSuspendTrace) {
        std::fprintf(stderr,
                     "[suspend] leashBound=%d beyondReach=%d (|t-root|=%.3f chain=%.3f) "
                     "strain=%.3f up=%.2f ticks=%d\n",
                     leashBound ? 1 : 0, beyondReach ? 1 : 0, glm::length(target - rootPos),
                     chainLen, strainLen, strainLen > 1e-6f ? strain.y / strainLen : 0.0f,
                     m_suspendTicks);
    }
    if (leashBound && beyondReach && strainLen > kSuspendStrain * m_sizeScale &&
        strain.y > kSuspendUpFraction * strainLen) {
        if (++m_suspendTicks >= kSuspendConfirmTicks) {
            m_suspended = true;
            m_airborne = false; // the feet are still on the floor; see the landing rule
            // Ground contacts release; USER pins stay (they are explicit intent — a body
            // hanging from a pinned hand is exactly what a lift against one produces).
            {
                std::vector<IkEffector> kept;
                std::vector<char> keptUser;
                int seatPin = -1; // (the seat is a user pin: it is kept, at a new index)
                for (std::size_t p = 0; p < m_pins.size(); ++p) {
                    if (p < m_pinUser.size() && m_pinUser[p]) {
                        if (static_cast<int>(p) == m_seatPin) {
                            seatPin = static_cast<int>(kept.size());
                        }
                        kept.push_back(m_pins[p]);
                        keptUser.push_back(1);
                    }
                }
                m_pins = std::move(kept);
                m_pinUser = std::move(keptUser);
                m_seatPin = seatPin;
                m_pinFootprint.assign(m_pins.size(), {});
                m_pinSteppable.assign(m_pins.size(), 0);
                m_pinStanceOffset.assign(m_pins.size(), glm::vec2(0.0f));
                m_pinLive.assign(m_pins.size(), 0); // live contacts release with the rest
                m_liveErrTicks.assign(m_pins.size(), 0);
                m_liveSpan.assign(m_pins.size(), 0.0f);
            }
            m_supportHull.clear();
            m_stepPin = -1; // an in-flight step's foot is released with the rest
            m_suspendHang = chainLen;
            // Only real body-mass segments dangle (trunk, limbs — the balance mass model
            // already classifies them); token-mass bones (face, fingers, helpers) keep
            // their local pose and ride. Without this the JAW — a hinged joint — was
            // gravity-pulled OPEN: the figure hung with its mouth agape. The effector's own
            // chain is always active regardless of mass.
            m_active.assign(m_bodyNode.size(), 0);
            for (std::size_t i = 0; i < m_bodyNode.size(); ++i) {
                if (m_bodyNode[i] && m_masses[i] > kRealMassThreshold) {
                    m_active[i] = 1;
                }
            }
            for (int cur = m_effector; cur >= 0;
                 cur = m_parents[static_cast<std::size_t>(cur)]) {
                if (!m_bodyNode[static_cast<std::size_t>(cur)]) {
                    break;
                }
                m_active[static_cast<std::size_t>(cur)] = 1;
                if (cur == m_pelvis) {
                    break;
                }
            }
        }
    } else {
        m_suspendTicks = 0;
    }
}

void IkRig::updateStepPolicy(const glm::vec3& target, const std::vector<glm::vec3>& positions,
                             const glm::vec2* bodyAnchor) {
    if (!dragActive() || m_suspended || m_pins.empty() || positions.size() != m_parents.size()) {
        return;
    }
    if (m_seatPin >= 0) {
        return; // (a SEATED body — its pelvis pinned — goes nowhere: no step answers anything)
    }
    if (m_scoped) {
        return; // (a SCOPED drag: the chain alone moves, and takes no step)
    }
    // HEALING ends when it is done: the flag (plantContacts: a hovering figure's pins snapped
    // down to the floor) was set once and blocked every step of the drag. It stands only while a
    // planted foot's joint is still ABOVE its target — a hover being pulled down; a foot lying on
    // its top behind a planted knee sits BELOW its standing target and is no hover (2026-09-26: a
    // chest pulled up out of a flat kneel could never step under its leaning body).
    // (... and a hover is a foot OFF THE FLOOR, 2026-09-28 — its lowest part, not its ankle:
    // on a heavy character the lying ankle rests 13mm OVER its standing height (a thicker foot
    // under it), the flag stood from the press, the foot then came up onto its toes and stayed
    // there — the ankle 37mm up, the toes on the floor — and no step ever came under a body
    // pulled up out of a kneel: she stood on tiptoe, her chest 13mm short, where her base
    // figure takes two steps and arrives. IK_HEALING_BY_ANKLE is the A/B.)
    static const bool kHealingSticks = std::getenv("IK_HEALING_STICKS") != nullptr; // A/B probe: the flag for the whole drag, as before
    static const bool kHealingByAnkle = std::getenv("IK_HEALING_BY_ANKLE") != nullptr; // A/B probe: the ankle's height alone
    if (m_healing && !kHealingSticks) {
        bool still = false;
        for (std::size_t p = 0; p < m_pins.size() && !still; ++p) {
            const int node = m_pins[p].node;
            if (node < 0 || (p < m_pinLive.size() && m_pinLive[p]) || (p < m_pinUser.size() && m_pinUser[p])) {
                continue;
            }
            still = positions[static_cast<std::size_t>(node)].y - m_pins[p].target.y > 0.01f * m_sizeScale &&
                    (kHealingByAnkle || contactHeight(node, positions) > 0.01f * m_sizeScale);
        }
        m_healing = still;
    }
    // A LOW body takes no new step (see kStepLowBody): a kneel, a crouch. (A taut-leg exception — a
    // pelvis low because a planted leg is straight and LEANING may step — was tried for a day,
    // 2026-09-26, for a chest pulled up out of a flat kneel that stood on straight legs on tiptoe
    // 27cm ahead of her feet: it made two generations step three and four times through a sit-up
    // and the newest through a kneel-up off all fours, and once the kneel's feet rolled back onto
    // their toes under a knee that lifts with the roll, every rig's kneel getting-up stepped
    // without it. The shadow's expiry — kShadowMaxRise — is what lets that step come.)
    const float standingY = m_bindPos[static_cast<std::size_t>(m_pelvis)].y + m_groundOffsetY;
    const float lowY = standingY - kStepLowBody * m_sizeScale;
    const bool lowBody = positions[static_cast<std::size_t>(m_pelvis)].y < lowY;
    if (m_effector == m_graph.root()) {
        // A pelvis drag walks the figure; a downward one is a crouch and keeps its feet.
        const bool crouchIntent =
            target.y < m_startPose[static_cast<std::size_t>(m_pelvis)].y - kCrouchIntentDepth * m_sizeScale;
        const glm::vec2 anchor(target.x, target.z);
        // (... and a LOW body with nothing but its FEET on the floor may step after a target above
        // the gate, 2026-10-28: the low-body gate is a kneel's — a figure dragged up out of one
        // stepped a foot forward under her own knees while still kneeling — and stood up by the
        // hips off a sphinx-like all fours, the hips a thigh's length ahead of the knees, the knees
        // lift off, the legs reach full extension with the feet 63cm behind the hips, the pelvis
        // rests 13cm under standing, and no step could come that would let it rise: a deadlock the
        // gate itself made. No live contact left — no knee, hand or seat — is a standing body with
        // its feet misplaced. IK_STEP_LOW_BODY_ALWAYS is the A/B.)
        static const bool kLowBodyAlways = std::getenv("IK_STEP_LOW_BODY_ALWAYS") != nullptr; // A/B probe
        bool liveContact = false;
        for (std::size_t p = 0; p < m_pins.size() && !liveContact; ++p) {
            liveContact = p < m_pinLive.size() && m_pinLive[p] != 0 && !pinIsShadow(p);
        }
        const bool lowKneeling = lowBody && (kLowBodyAlways || liveContact);
        updateStepping(positions, !crouchIntent && !lowKneeling && target.y >= lowY, &anchor);
        return;
    }
    // The balance EFFORT: how far the solved centre of mass sits outside its support, low-passed.
    glm::vec2 need(0.0f);
    if (!m_supportHull.empty()) {
        BalanceController::balanceCorrection(positions, m_parents, m_masses, m_supportHull,
                                             kBalanceMargin * m_sizeScale, need);
    }
    {
        const float needLen = glm::length(need);
        need *= needLen > 1.0e-6f ? std::max(0.0f, 1.0f - m_startImbalance / needLen) : 0.0f; // (see m_startImbalance)
    }
    m_balanceCorrection = glm::mix(m_balanceCorrection, need, kBalanceSmoothing);
    const glm::vec3& effPos = positions[static_cast<std::size_t>(m_effector)];
    const glm::vec2 residXZ(target.x - effPos.x, target.z - effPos.z);
    glm::vec2        anchor(0.0f);
    const glm::vec2* anchorPtr = nullptr;
    if (bodyAnchor != nullptr) {
        // A trunk drag whose solve takes the hips along: the stance goes under where the hips
        // are going, as it does under a pelvis drag's target.
        anchor = *bodyAnchor;
        anchorPtr = &anchor;
    } else if (effectorIsTrunk() && !m_lastDownIntent &&
        glm::length(residXZ) > kStepStanceThreshold * m_sizeScale) {
        const glm::vec3& rootPos = positions[static_cast<std::size_t>(m_pelvis)];
        anchor = glm::vec2(rootPos.x, rootPos.z) + residXZ;
        anchorPtr = &anchor;
    }
    static const bool kStepGateTrace = std::getenv("IK_STEP_TRACE") != nullptr;
    if (kStepGateTrace) {
        std::fprintf(stderr, "[step-gate] up=%d healing=%d low=%d pelvisY=%.3f lowY=%.3f", m_lastUpIntent ? 1 : 0, m_healing ? 1 : 0, lowBody ? 1 : 0,
                     positions[static_cast<std::size_t>(m_pelvis)].y, lowY);
        std::fputc(10, stderr);
    }
    updateStepping(positions, !m_lastUpIntent && !m_healing && !lowBody, anchorPtr);
}

void IkRig::landPendingStep(const std::vector<glm::vec3>& positions) {
    if (dragActive() && m_stepPin >= 0 && positions.size() == m_parents.size()) {
        landStep(positions);
    }
}

} // namespace pose

/**
 * @file ikrigstepping.cpp
 * @brief Balance-driven STEPPING: when a sustained drag holds the body beyond what leaning can
 *        absorb, the IkRig re-plants a standing foot at its balanced position with an animated
 *        swing (updateStepping), lands it (landStep), and rebuilds the support polygon from the
 *        pins' footprints (rebuildSupportHull).
 *
 * The trigger is sustained and two-signaled — the smoothed balance EFFORT, or a foot far from
 * its anchor-centred stance spot (what makes a hip walk, and a trunk drag that takes the hips
 * along, step proactively and take the final gathering step). There is no pin-strain trigger:
 * the solve never drags a foot off its plant. Landings are reach-clamped against the leg's path from where
 * the root is GOING and clearance-clamped on the near side of other pins so legs never scissor;
 * a step in flight always advances whatever the drag does meanwhile. See ikrig.cpp for the TU
 * layout; the constants come from ikrig_constants.h. Qt-free (std + GLM).
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

void IkRig::rebuildSupportHull(int excludePin) {
    std::vector<glm::vec2> pts;
    for (std::size_t p = 0; p < m_pins.size(); ++p) {
        if (static_cast<int>(p) == excludePin) {
            continue; // the foot in flight supports nothing
        }
        const glm::vec2 t(m_pins[p].target.x, m_pins[p].target.z);
        for (const glm::vec2& off : m_pinFootprint[p]) {
            pts.push_back(t + off);
        }
    }
    m_supportHull = BalanceController::supportPolygon(std::move(pts));
}

void IkRig::landStep(const std::vector<glm::vec3>& positions) {
    IkEffector& pin = m_pins[static_cast<std::size_t>(m_stepPin)];
    pin.target = m_stepTo;
    pin.leashRadius = socketLeash(pin.node, m_stepTo, positions, pin.leashOffset);
    rebuildSupportHull(-1);
    // The body's pose prior WALKS with the stance: the drag-start positions every non-pin
    // node eases back toward shift horizontally by the stance center's move (this step's
    // displacement shared over the standing feet), so the pelvis — the stiffest prior of all
    // — comes over the new stance instead of staying anchored where the drag began, with the
    // feet planted a stride ahead of it. (A pelvis drag pins the root and never reads this.)
    int stanceCount = 0;
    for (std::size_t p = 0; p < m_pins.size(); ++p) {
        if (p < m_pinSteppable.size() && m_pinSteppable[p]) {
            ++stanceCount;
        }
    }
    if (stanceCount > 0) {
        const glm::vec3 shift = (m_stepTo - m_stepFrom) / static_cast<float>(stanceCount);
        for (glm::vec3& p : m_startPose) {
            p.x += shift.x;
            p.z += shift.z;
        }
        m_stanceShift += glm::vec3(shift.x, 0.0f, shift.z);
    }
    m_stepPin = -1;
    m_imbalanceTicks = 0;
    m_startImbalance = 0.0f; // (the stance is the rig's own now)
    ++m_stepsTaken;
}

void IkRig::updateStepping(const std::vector<glm::vec3>& positions, bool allowTrigger,
                           const glm::vec2* anchorXZ) {
    if (m_stepPin >= 0) {
        // A step in flight ALWAYS advances (drag intent may change mid-swing; a foot must never
        // hang mid-air waiting for it to change back): ease the pin target along the glide with
        // a sine lift arc, then land — re-pin at the balanced spot, restore a recomputed leash,
        // and rebuild the full support polygon.
        ++m_stepTick;
        const float t =
            glm::clamp(static_cast<float>(m_stepTick) / static_cast<float>(m_stepTotal), 0.0f,
                       1.0f);
        const float ease = t * t * (3.0f - 2.0f * t);
        IkEffector& pin = m_pins[static_cast<std::size_t>(m_stepPin)];
        pin.target = glm::mix(m_stepFrom, m_stepTo, ease);
        pin.target.y += m_stepHeight * std::sin(3.14159265f * t);
        if (m_stepTick >= m_stepTotal) {
            landStep(positions);
        }
        return;
    }
    if (!allowTrigger) {
        m_imbalanceTicks = 0;
        return;
    }
    // The stance ANCHOR the balanced-stance estimate centers on: a pelvis drag's TARGET when
    // explicit (where the user is TAKING the body — steps land where it is going, and the end
    // stance is the drag-start stance shape centered on where the hip stops), else the CoM
    // (upper-body drags: the balance point is the intent signal). Anchoring steps on the
    // current CoM during a pelvis drag placed every landing behind the moving hip and the walk
    // ended in a collapsed, machinery-looking stance.
    // The explicit anchor is offset by the stance's drag-start relationship to the hip (see
    // m_stanceRootOffset); the CoM anchor is the balance point itself.
    glm::vec2 anchor(0.0f);
    if (anchorXZ != nullptr) {
        anchor = *anchorXZ + m_stanceRootOffset;
    } else {
        const glm::vec3 com = BalanceController::centerOfMass(positions, m_parents, m_masses);
        anchor = glm::vec2(com.x, com.z);
    }
    // Worst steppable stance error vs the anchor — both the explicit-anchor trigger and the
    // step-choice ranking read it.
    const float scale = m_sizeScale;
    int best = -1;
    float bestErr = kStepMinDistance * scale;
    for (std::size_t p = 0; p < m_pins.size(); ++p) {
        if (!m_pinSteppable[p]) {
            continue;
        }
        const glm::vec2 desired = anchor + m_pinStanceOffset[p];
        const float e =
            glm::length(glm::vec2(m_pins[p].target.x, m_pins[p].target.z) - desired);
        if (e > bestErr) {
            bestErr = e;
            best = static_cast<int>(p);
        }
    }
    // Trigger, sustained for kStepConfirmTicks (a transient lean recovers by itself): the
    // smoothed balance EFFORT stays high — the drag keeps imposing a lean the pelvis correction
    // must continuously fight (the post-correction residual is useless here: the engagement
    // blend succeeds each tick, so the residual reads balanced right up until the figure is
    // grotesquely stretched) — or, with an EXPLICIT anchor only, a foot is simply far from its
    // target-centered stance spot: this is what makes a pelvis-walk step PROACTIVELY (before
    // the foot gets visibly dragged) and take its final GATHERING step once the hip arrives
    // (the trailing foot's pin holds fine there, so the effort signal would never fire). There
    // is no pin-strain trigger: the solve never drags a foot off its plant.
    const bool misplaced =
        anchorXZ != nullptr && best >= 0 && bestErr > kStepStanceThreshold * scale;
    static const bool kStepTrace = std::getenv("IK_STEP_TRACE") != nullptr;
    if (kStepTrace) {
        std::fprintf(stderr, "[step] corr=%.4f stanceErr=%.4f m=%d ticks=%d\n",
                     glm::length(m_balanceCorrection), best >= 0 ? bestErr : 0.0f,
                     misplaced ? 1 : 0, m_imbalanceTicks);
    }
    if (glm::length(m_balanceCorrection) <= kStepNeedThreshold * scale && !misplaced) {
        m_imbalanceTicks = std::max(0, m_imbalanceTicks - 1);
        return;
    }
    // (The confirmation is for the balance EFFORT, which blips; a foot a stride from its anchored
    // spot is no blip, and confirmed for the same 12 ticks a fast chest drag's first step began
    // as the cursor STOPPED — 20 ticks of drag, then 1.4 seconds of walking after it, each
    // step's 12 ticks of confirmation waited out after the last one landed. The MISPLACED
    // signal's confirmation shrinks with the distance: none at all from kStepStanceThreshold
    // times kStepPromptFactor. IK_STEP_SLOW_CONFIRM restores the flat 12.)
    static const bool kSlowConfirm = std::getenv("IK_STEP_SLOW_CONFIRM") != nullptr; // A/B probe
    const float promptness = (!kSlowConfirm && misplaced)
                                 ? glm::smoothstep(kStepStanceThreshold * scale, kStepPromptFactor * kStepStanceThreshold * scale, bestErr)
                                 : 0.0f;
    const int confirmTicks = static_cast<int>(std::lround(static_cast<float>(kStepConfirmTicks) * (1.0f - promptness)));
    if (++m_imbalanceTicks < confirmTicks) {
        return;
    }
    if (best < 0) {
        m_imbalanceTicks = 0;
        return;
    }
    IkEffector& pin = m_pins[static_cast<std::size_t>(best)];
    const float bindY = m_bindPos[static_cast<std::size_t>(pin.node)].y + m_groundOffsetY;
    glm::vec2 landXZ = anchor + m_pinStanceOffset[static_cast<std::size_t>(best)];
    // Reachability: the landing must stay within the leg's reach of where the ROOT is going —
    // the anchor (measuring from the CURRENT root systematically pulled landings in under the
    // pelvis: by landing time the root has moved on, and the tight ring NARROWED every stance).
    {
        const float pathLen = pathLenToRoot(pin.node);
        const float dy = positions[static_cast<std::size_t>(m_pelvis)].y - bindY;
        // 0.98 of the leg path: a STANDING leg already uses ~0.95 of its root-to-foot path
        // (the pelvis-to-socket offset is lateral, not collinear), so a conservative factor
        // here read every standing stance as "unreachable" and no step could ever initiate.
        const float maxHoriz2 = 0.9604f * pathLen * pathLen - dy * dy;
        if (maxHoriz2 <= 1e-6f) {
            m_imbalanceTicks = 0;
            return; // the root sits too high/low for this leg to plant anywhere useful
        }
        const glm::vec2 toLand = landXZ - anchor;
        const float horiz = glm::length(toLand);
        const float maxHoriz = std::sqrt(maxHoriz2);
        if (horiz > maxHoriz) {
            landXZ = anchor + toLand * (maxHoriz / horiz);
        }
    }
    // Clearance, SIDE-PRESERVING: never land on another pin, and never CROSS it — a landing on
    // the far side of the standing foot (a long lateral move whose trailing foot's desired spot
    // lies past the leading foot) is clamped to the NEAR side at clearance distance instead.
    // The legs never scissor, and on a long move the feet alternate naturally: the clamped foot
    // stops at the standing one, which then carries the larger stance error and leapfrogs.
    for (std::size_t p = 0; p < m_pins.size(); ++p) {
        if (static_cast<int>(p) == best) {
            continue;
        }
        const glm::vec2 other(m_pins[p].target.x, m_pins[p].target.z);
        const glm::vec2 curSide =
            glm::vec2(pin.target.x, pin.target.z) - other; // which side this foot is on now
        const glm::vec2 away = landXZ - other;
        const float sideLen = glm::length(curSide);
        if (sideLen > 1e-5f && glm::dot(away, curSide) < 0.0f) {
            landXZ = other + curSide * (kStepClearance * scale / sideLen); // would cross
            continue;
        }
        const float d = glm::length(away);
        if (d < kStepClearance * scale) {
            if (d < 1e-5f) {
                m_imbalanceTicks = 0;
                return;
            }
            landXZ = other + away * (kStepClearance * scale / d);
        }
    }
    m_stepFrom = pin.target;
    m_stepTo = glm::vec3(landXZ.x, bindY, landXZ.y);
    const float dist = glm::length(m_stepTo - m_stepFrom);
    if (dist < kStepMinDistance * scale) {
        m_imbalanceTicks = 0;
        return;
    }
    m_stepPin = best;
    m_stepTick = 0;
    m_stepTotal = glm::clamp(static_cast<int>(dist / (kStepSpeed * scale)), kStepMinTicks,
                             kStepMaxTicks);
    m_stepHeight = glm::clamp(kStepHeightFactor * dist, kStepHeightMin * scale,
                              kStepHeightMax * scale);
    // A moving pin's leash would yank the root after the swinging target; the standing limbs'
    // leashes keep binding the root, and the landing recomputes this one. The sentinel must be
    // effectively INFINITE, not merely unset: the solver treats a non-positive radius as
    // "compute the path-length fallback", which quietly re-leashed the root to the moving
    // swing target.
    pin.leashRadius = 1e9f;
    rebuildSupportHull(best);
    static const bool kRigStepTrace = std::getenv("POSESTUDIO_IK_TRACE") != nullptr;
    if (kRigStepTrace) {
        std::fprintf(stderr, "[ikrig] STEP pin=%d from(%.3f %.3f) to(%.3f %.3f) ticks=%d\n",
                     pin.node, m_stepFrom.x, m_stepFrom.z, m_stepTo.x, m_stepTo.z, m_stepTotal);
    }
}

} // namespace pose

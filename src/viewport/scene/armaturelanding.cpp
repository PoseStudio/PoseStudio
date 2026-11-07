/**
 * @file armaturelanding.cpp
 * @brief THE LANDING BOUNCE: a figure the Ground button dropped onto her feet absorbs the landing —
 *        the hips dip over the planted feet, the knees folding, and come back up — deeper the
 *        faster she came down. Purely aesthetic: the pose it ends in is the pose it began in.
 *
 * The motion is not keyframed: it is a PELVIS DRAG made by the solve's own machinery (the scoped
 * one, IkScope::Chain on the solve root — the pelvis and the legs alone over planted feet, no
 * balance, no step, no post-step), its target the hips' place at the landing lowered and brought
 * back along one smooth dip. So the knees fold the way this figure's knees fold, within her
 * limits, on any body; and since the solve's pose is a function of its target, the hips back at
 * their height are the pose she landed in. What little the solve may still leave different (a
 * foot the rig healed flat) is blended home over the bounce's tail, and the end restores the
 * landing pose exactly. The window drives it from the fall's timer (VulkanWindow::onFallTick).
 * Qt-free (std + GLM).
 */

#include "armature.h"
#include "softbodysettings.h"
#include "armatureiksolvetuning.h"

#include "ikrig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pose {

namespace {

constexpr float kLandingArmSwingDeg = 25.0f;  ///< THE ARMS' BOUNCE: how far a hanging arm swings forward at the deepest dip a drop makes (10 until 2026-11-05: the user, "the arms should react as well").
constexpr float kLandingArmHz = 1.2f;         ///< The arm swing is a damped oscillation kicked by the landing: forward to its peak in 0.18 s, back past hanging by a quarter, and out.
constexpr float kLandingArmDamping = 0.3f;    ///< Its damping ratio.
constexpr float kLandingArmPeak = 0.64f;      ///< The oscillation's first peak as a share of its drive (e^-zw t sin(wd t) at t_peak for the damping above): the amplitude is divided by it.
constexpr float kLandingArmElbowShare = 0.5f; ///< The elbow bends by this share of the arm's forward swing (the hands come up a little) and straightens as it swings back.
constexpr float kLandingArmRestDeg = 0.3f;    ///< The swing is over under this.
constexpr float kLandingTailSeconds = 1.5f;   ///< The bounce's TAIL after the dip: the arms' swing and the breasts' spring run on, for at most this.

/// The bounce's SHAPE, 0..1, at @p seconds of @p duration: down over the first kLandingDownShare
/// (sin^2), back up over the rest (cos^2) — see the tuning header.
float landingShape(float seconds, float duration) {
    if (seconds <= 0.0f || seconds >= duration) {
        return 0.0f;
    }
    const float down = kLandingDownShare * duration;
    const float phase = seconds < down ? seconds / down : 1.0f + (seconds - down) / (duration - down);
    const float wave = std::sin(1.57079633f * phase); // (0 at the landing, 1 at the deepest, 0 at the end)
    return wave * wave;
}

} // namespace

bool Armature::beginLandingBounce(float impactSpeed) {
    static const bool kNoBounce = std::getenv("POSESTUDIO_NO_LANDING_BOUNCE") != nullptr; // A/B probe
    if (kNoBounce || m_landing.active || m_digitDrag || m_bones.empty() || !ensureIkRig() || m_ikRig->dragActive()) {
        return false;
    }
    static const float kPerSpeed = envOr("POSESTUDIO_LANDING_DIP", kLandingDipPerSpeed); // metres per m/s
    kickSoftBody(impactSpeed); // (THE SOFT BREASTS keep falling as the chest stops, whatever the legs do)
    const float scale = m_ikRig->sizeScale();
    const float most = kLandingDipMax * scale;
    const float dip = most * std::tanh(kPerSpeed * std::max(0.0f, impactSpeed) / most);
    if (dip < kLandingDipMin * scale) {
        return false; // (a drop of a millimetre: nothing to absorb)
    }
    // (A pinned pelvis is a body something else holds up — a seat: nothing dips. The pin is looked
    // for here: the rig leaves a pin on the dragged joint itself out of the drag.)
    const int root = m_ikRig->rootNode();
    for (std::size_t b = 0; b < m_bonePinned.size() && b < m_bones.size(); ++b) {
        if (m_bonePinned[b] && (static_cast<int>(b) == root || m_ikRig->isGirdleJoint(static_cast<int>(b)))) {
            return false;
        }
    }
    // The bounce's drag is the pelvis's: it borrows the selection for as long as it lasts.
    const int   selected = m_selectedBone;
    const int   grabBone = m_ikGrabBone;
    const auto  giveBack = [&]() {
        m_selectedBone = selected;
        m_ikGrabBone = grabBone;
    };
    m_selectedBone = root;
    m_ikGrabBone = -1;
    if (root < 0 || !beginIkDrag(IkScope::Chain)) {
        giveBack();
        return false;
    }
    // ON HER FEET: what the rig planted is feet, and nothing else of her is on the floor — no
    // knee, hand, seat or head (the pose's own contacts, seeded live), no seat pin.
    int  feet = 0;
    bool other = m_ikRig->seatPin() >= 0 || m_ikRig->floorSeat() || m_ikRig->onKnees() || m_ikRig->suspended();
    const std::vector<IkEffector>& pins = m_ikRig->pins();
    for (std::size_t p = 0; p < pins.size() && !other; ++p) {
        const int node = pins[p].node;
        if (node < 0 || m_ikRig->pinIsUser(p)) {
            continue; // (a user pin holds what it holds: a hand on a rail)
        }
        const bool footClass = m_ikRig->isFootClass(node);
        if (m_ikRig->pinIsLive(p) || !footClass) {
            other = true;
        } else {
            ++feet;
        }
    }
    static const bool kTrace = std::getenv("POSESTUDIO_LANDING_TRACE") != nullptr;
    if (kTrace) {
        std::fprintf(stderr, "[landing] impact %.2f m/s -> dip %.1f mm; feet planted %d, other contacts %d: %s\n", impactSpeed,
                     dip * 1000.0f, feet, other ? 1 : 0, feet > 0 && !other ? "bounce" : "no bounce");
    }
    if (feet == 0 || other) {
        endIkDrag(); // (settles the soft body with the drag it closes, the kick above included ...)
        giveBack();
        // ... so the landing's kick is given again: not on her feet nothing dips, but the breasts
        // and the buttocks still bounce, and the fall's soft tail runs them to rest (left settled,
        // a kneeling or seated figure's breasts floated up through the fall and snapped to rest on
        // impact, 2026-11-07).
        kickSoftBody(impactSpeed);
        return false;
    }
    m_landing.active = true;
    m_landing.dip = dip;
    m_landing.seconds = kLandingSeconds * glm::mix(kLandingShortest, 1.0f, dip / most);
    m_landing.hipStart = m_boneWorldPos[static_cast<std::size_t>(root)];
    m_landing.euler = m_ikStartEuler; // (the drag's own snapshot of the pose: beginIkDrag)
    // THE ARMS' BOUNCE: every arm that hangs (the press found them: findHangArms) swings forward
    // over the dip and back, by kLandingArmSwingDeg at the deepest dip the drop makes.
    m_landing.arms.clear();
    m_landing.armSwingDeg = kLandingArmSwingDeg * (dip / most);
    m_landing.lastSeconds = 0.0f;
    m_landing.tail = false;
    for (const HangArm& arm : m_jsHangArms) {
        const JointDial dial = jointDial(arm.socket, JointDialKind::BendForward);
        if (!dial.valid() || dial.bone < 0 || static_cast<std::size_t>(dial.bone) >= m_bones.size() || dial.fullDeg == 0.0f) {
            continue;
        }
        LandingBounce::ArmSwing swing;
        swing.bone = dial.bone;
        swing.axis = dial.axis;
        swing.sign = dial.fullDeg > 0.0f ? 1.0f : -1.0f;
        swing.base = m_landing.euler[static_cast<std::size_t>(dial.bone)][dial.axis];
        const JointDial elbow = arm.elbow >= 0 ? jointDial(arm.elbow, JointDialKind::BendForward) : JointDial{};
        if (elbow.valid() && elbow.bone >= 0 && static_cast<std::size_t>(elbow.bone) < m_bones.size() && elbow.fullDeg != 0.0f) {
            swing.elbowBone = elbow.bone;
            swing.elbowAxis = elbow.axis;
            swing.elbowSign = elbow.fullDeg > 0.0f ? 1.0f : -1.0f;
            swing.elbowBase = m_landing.euler[static_cast<std::size_t>(elbow.bone)][elbow.axis];
        }
        m_landing.arms.push_back(swing);
    }
    m_landing.translation = m_jsStartTranslation;
    m_landing.selected = selected;
    m_landing.grabBone = grabBone;
    m_landing.grabLocal = m_ikGrabLocal;
    return true;
}

float Armature::landingArmAngle(float seconds) const {
    // e^(-zeta w t) sin(wd t), scaled so its first peak is the swing's amplitude.
    const float omega = 2.0f * 3.14159265f * kLandingArmHz;
    const float wd = omega * std::sqrt(1.0f - kLandingArmDamping * kLandingArmDamping);
    return m_landing.armSwingDeg / kLandingArmPeak * std::exp(-kLandingArmDamping * omega * seconds) * std::sin(wd * seconds);
}

bool Armature::landingBounceTick(float seconds) {
    if (!m_landing.active) {
        return false;
    }
    const float dt = seconds > m_landing.lastSeconds ? seconds - m_landing.lastSeconds : 1.0f / 60.0f;
    m_landing.lastSeconds = seconds;
    // THE ARMS' SWING: a damped oscillation kicked by the landing — forward to its peak in a fifth
    // of a second, back past hanging by a quarter of that, and out over a second; the elbow bends
    // with the forward swing. It runs through the dip and on through the tail (the pose home
    // under it), and endLandingBounce puts the landing pose back exactly.
    const float armAngle = landingArmAngle(seconds);
    const auto  applyArms = [&]() {
        if (m_landing.arms.empty()) {
            return false;
        }
        for (const LandingBounce::ArmSwing& arm : m_landing.arms) {
            m_boneEuler[static_cast<std::size_t>(arm.bone)][arm.axis] = arm.base + arm.sign * armAngle;
            clampBoneEuler(arm.bone);
            recomposePoseLocal(static_cast<std::size_t>(arm.bone));
            if (arm.elbowBone >= 0) {
                m_boneEuler[static_cast<std::size_t>(arm.elbowBone)][arm.elbowAxis] = arm.elbowBase + arm.elbowSign * kLandingArmElbowShare * std::max(0.0f, armAngle);
                clampBoneEuler(arm.elbowBone);
                recomposePoseLocal(static_cast<std::size_t>(arm.elbowBone));
            }
        }
        return true;
    };
    if (seconds >= m_landing.seconds) {
        // THE TAIL: the dip is over and the pose is home; the arms' swing and the breasts' spring
        // run on, for up to kLandingTailSeconds, then the bounce ends (endLandingBounce rests it
        // exactly).
        const bool armsSwinging = !m_landing.arms.empty() && std::abs(armAngle) > kLandingArmRestDeg;
        if (seconds >= m_landing.seconds + kLandingTailSeconds || (!armsSwinging && !softbody::breastMotion() && !softbody::buttockMotion())) {
            return false;
        }
        if (!m_landing.tail) {
            m_landing.tail = true;
            if (m_landing.euler.size() == m_bones.size() && m_landing.translation.size() == m_bones.size()) {
                m_boneEuler = m_landing.euler;
                m_boneTranslation = m_landing.translation;
                for (std::size_t b = 0; b < m_bones.size(); ++b) {
                    recomposePoseLocal(b);
                }
            }
        }
        if (applyArms()) {
            computeSkinMatrices();
        }
        const bool soft = tickSoftBody(dt); // (the breasts and the buttocks; at once when both are off)
        return armsSwinging || soft;
    }
    dragIkTo(m_landing.hipStart - glm::vec3(0.0f, m_landing.dip * landingShape(seconds, m_landing.seconds), 0.0f));
    if (applyArms()) {
        computeSkinMatrices();
    }
    tickSoftBody(dt); // (THE SOFT BODY feels the landing's deceleration and bounces; at once when off)
    // The tail: blended home to the pose she landed in.
    const float home = glm::smoothstep(kLandingBlendFrom * m_landing.seconds, m_landing.seconds, seconds);
    if (home > 0.0f && m_landing.euler.size() == m_bones.size() && m_landing.translation.size() == m_bones.size()) {
        bool moved = false;
        for (std::size_t b = 0; b < m_bones.size(); ++b) {
            if (m_boneEuler[b] == m_landing.euler[b] && m_boneTranslation[b] == m_landing.translation[b]) {
                continue;
            }
            m_boneEuler[b] = glm::mix(m_boneEuler[b], m_landing.euler[b], home);
            m_boneTranslation[b] = glm::mix(m_boneTranslation[b], m_landing.translation[b], home);
            recomposePoseLocal(b);
            moved = true;
        }
        if (moved) {
            computeSkinMatrices();
        }
    }
    return true;
}

void Armature::endLandingBounce() {
    if (!m_landing.active) {
        return;
    }
    settleSoftBody(); // (the breasts at rest in the landing pose)
    endIkDrag();
    if (m_landing.euler.size() == m_bones.size() && m_landing.translation.size() == m_bones.size()) {
        m_boneEuler = m_landing.euler;
        m_boneTranslation = m_landing.translation;
        reposeAll(); // the pose she landed in, exactly
    }
    m_selectedBone = m_landing.selected;
    m_ikGrabBone = m_landing.grabBone;
    m_ikGrabLocal = m_landing.grabLocal;
    m_landing = LandingBounce();
}

} // namespace pose

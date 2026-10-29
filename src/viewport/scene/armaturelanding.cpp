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
#include "armatureiksolvetuning.h"

#include "ikrig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pose {

namespace {

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
        endIkDrag();
        giveBack();
        return false;
    }
    m_landing.active = true;
    m_landing.dip = dip;
    m_landing.seconds = kLandingSeconds * glm::mix(kLandingShortest, 1.0f, dip / most);
    m_landing.hipStart = m_boneWorldPos[static_cast<std::size_t>(root)];
    m_landing.euler = m_ikStartEuler; // (the drag's own snapshot of the pose: beginIkDrag)
    m_landing.translation = m_jsStartTranslation;
    m_landing.selected = selected;
    m_landing.grabBone = grabBone;
    m_landing.grabLocal = m_ikGrabLocal;
    return true;
}

bool Armature::landingBounceTick(float seconds) {
    if (!m_landing.active || seconds >= m_landing.seconds) {
        return false;
    }
    dragIkTo(m_landing.hipStart - glm::vec3(0.0f, m_landing.dip * landingShape(seconds, m_landing.seconds), 0.0f));
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

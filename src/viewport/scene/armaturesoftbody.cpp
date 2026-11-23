// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file armaturesoftbody.cpp
 * @brief THE SOFT BODY's common entry points: what the drag tick, the settle, the fall, the landing
 *        and the Soft Body Physics tab call for the breasts and the buttocks TOGETHER — one tick, the
 *        landing's kick, the settle, "still moving", the settings applied — the set-up of the delta
 *        fields the Model bakes at import, the weights the GPU blends them by, and the acceleration
 *        reader both parts read their joint's jolt with.
 *
 * The parts themselves are armaturebreasts.cpp (the mass a breast is, on a pectoral bone),
 * armaturebreastfield.cpp (the same mass on a rig with no breast bone: a delta field) and
 * armaturebuttocks.cpp (the cheeks: a delta field). Each has its own model and tuning — the two
 * integrators are deliberately not one. OFF unless the Soft Body Physics tab turns a part on
 * (softbodysettings.h): the IK harness runs the engine as it was. Qt-free (std + GLM).
 */

#include "armature.h"

#include "ikmath.h" // normalizedFrame
#include "softbodysettings.h"

namespace pose {

glm::vec3 Armature::worldInBoneFrame(int bone, const glm::vec3& world) const {
    // The model transform taken off first; the bone's rotation is its world rotation away from the
    // bind (binds are translation-only).
    const glm::mat3 toModel = glm::inverse(glm::mat3(m_transform));
    const glm::mat3 frame = normalizedFrame(m_poseGlobal[static_cast<std::size_t>(bone)]);
    return glm::transpose(frame) * (toModel * world);
}

void Armature::setupSoftBodyFields() {
    if ((m_buttocksReady && m_breastFieldsReady) || m_bodyMesh.empty() || !ensureIkRig()) {
        return;
    }
    if (!m_buttocksReady) {
        setupButtocks();
    }
    if (!m_breastFieldsReady) {
        setupBreastFields(); // (armaturebreastfield.cpp: a breast no pectoral bone carries)
    }
}

void Armature::applySoftBodySettings() {
    if (softbody::breastMotion()) {
        if (!m_breastsReady && !m_bodyMesh.empty() && ensureIkRig()) {
            setupBreasts();
        }
    }
    settleBreasts(); // (the spring, its reader and the effective gravity at rest: a dial turned mid-bounce otherwise drew the rest pose with a stale jolt)
    settleButtocks(); // (the buttocks' field exists from the import; off, its weights read 0)
    computeSkinMatrices(); // (on: the resting swing; off: the bind's pectorals again)
}

bool Armature::tickSoftBody(float dt) {
    // ONE TICK OF THE SOFT BODY: the breasts (here) and the buttocks (armaturebuttocks.cpp), then
    // the skin matrices once — the breasts' skin transform, and the version bump the corrective
    // weights (the buttocks' field) are re-read on at the next recorded frame.
    dt = glm::clamp(dt, 1.0f / 240.0f, 1.0f / 20.0f);
    const bool breasts = softbody::breastMotion() && m_breastsReady && !m_breasts.empty();
    const bool buttocks = softbody::buttockMotion() && m_buttocksReady && !m_buttocks.empty();
    if (!breasts && !buttocks) {
        return false;
    }
    bool moving = false;
    if (breasts) {
        tickBreasts(dt, moving);
    }
    if (buttocks) {
        tickButtocks(dt, moving);
    }
    computeSkinMatrices();
    return moving;
}

glm::vec3 armaturestate::SoftBodyAccelReader::sample(const glm::vec3& p, float dt, float velSmooth, float accelSmooth, float deadG,
                                                float maxG, float& magOut) {
    glm::vec3 raw(0.0f);
    if (samples >= 1) {
        const glm::vec3 velRaw = (p - pos) / dt;
        const glm::vec3 v = samples >= 2 ? glm::mix(vel, velRaw, velSmooth) : velRaw;
        if (samples >= 2) {
            raw = (v - vel) / dt;
        }
        vel = v;
    }
    pos = p;
    ++samples;
    constexpr float g = 9.81f;
    accel = glm::mix(accel, raw / g, accelSmooth);
    glm::vec3 felt = accel;
    float     mag = glm::length(felt);
    if (mag < deadG) {
        felt = glm::vec3(0.0f);
        mag = 0.0f;
    } else if (mag > maxG) {
        felt *= maxG / mag;
        mag = maxG;
    }
    magOut = mag;
    return felt;
}

void Armature::kickSoftBody(float impactSpeed) {
    kickBreasts(impactSpeed);
    kickButtocks(impactSpeed);
}

void Armature::settleSoftBody() {
    const bool was = m_breastDynamic || m_buttockDynamic;
    settleBreasts();
    settleButtocks();
    if (was) {
        computeSkinMatrices(); // (the resting displacement; the buttocks' weights back to 0)
    }
}

bool Armature::softBodyMoving() const {
    return breastsMoving() || buttocksMoving();
}

float Armature::softBodyWeight(int index) const {
    if (index >= kSoftBodyBreastBase) {
        // A boneless breast's field (armaturebreastfield.cpp): side × 3 + the chest axis.
        const int k = index - kSoftBodyBreastBase;
        return k < kSoftBodyBreastFields ? breastFieldDisplacement(k / 3, k % 3) : 0.0f;
    }
    if (!softbody::buttockMotion() || !m_buttocksReady || !m_buttockDynamic || index < 0) {
        return 0.0f;
    }
    const int cheek = index / 2;
    const int axis = index % 2;
    if (cheek < 0 || static_cast<std::size_t>(cheek) >= m_buttocks.size()) {
        return 0.0f;
    }
    return m_buttocks[static_cast<std::size_t>(cheek)].disp[axis];
}

} // namespace pose

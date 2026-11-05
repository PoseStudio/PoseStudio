/**
 * @file armaturebreasts.cpp
 * @brief SOFT BREASTS (2026-11-04; the user: "natural breasts will hang and move based on the
 *        position of the torso and gravity, and depending on their size"): the pectoral bones —
 *        the rig standard's Pectoral role, which every native generation and the character-
 *        creation rigs carry with the breast's skin on them — are turned by what GRAVITY does to
 *        a breast that the sculpt has at rest STANDING, and sized by the breast the figure
 *        actually has.
 *
 * THE MODEL: a breast is a mass on a short tether out of the chest wall, forward along the chest
 * (r0 = +z in the chest's frame at bind, the figure facing +z). Standing, gravity pulls it down
 * the chest and the sculpt already shows that; what a pose changes is the gravity the breast
 * feels IN THE CHEST'S FRAME. Its deviation from the standing sculpt is the change of that
 * force perpendicular to the tether: delta = g_chest - (0,-1,0), perp = delta minus its part
 * along r0, and the bone turns about r0 × perp by kBreastSwingGainDeg × |perp| (capped). Bowed
 * forward, gravity runs along the tether and the sag toward the belly is relieved — the breast
 * lifts toward the head and hangs out; lying on the back it lifts toward the chin; on the side
 * it falls to the lower side; upside down it falls toward the head, twice as far. A rotation
 * about the pectoral joint swings and sags; it cannot flatten a volume (that is a morph's).
 *
 * THE SIZE: the body mesh sample (the skin the pectoral bone dominates) says how far the breast
 * stands out from its joint along the chest's forward; a flat chest (a male figure, a child)
 * reads under kBreastFlat and moves nothing, a full breast over kBreastFull moves the whole
 * amount, figure-scaled. One pectoral per side carries the breast (the one that stands out
 * furthest: a rib twist beside it reads a few centimetres); the rest ride as before.
 *
 * THE MOTION: while the engine ticks (a drag, its settle, the Ground button's fall and landing
 * bounce) the swing is a damped spring toward the resting swing (kBreastHz, kBreastDamping),
 * and the gravity it feels is EFFECTIVE gravity — g less the chest's own acceleration, read off
 * the chest joint's motion tick to tick — so a chest that stops short leaves the breasts to
 * catch up, a falling figure's breasts float up weightless, and the landing's deceleration
 * drops them and lets them bounce. Outside the ticks (a dial, a pose file, undo) the resting
 * swing applies at once. Nothing of it is in the pose: the channels, the pose files, the undo
 * stack, the harness's measures and the joints' positions (the swing is about the pectoral's
 * own joint) read as they did; balance, the IK and the volumes never see it. OFF unless the
 * Posing preference turns it on (softbody::breastMotion): the harness runs the engine as it was.
 * POSESTUDIO_BREAST_TRACE=1 prints each candidate's protrusion and the size it was given.
 */

#include "armature.h"

#include "ikrig.h"
#include "rigroles.h"
#include "softbodysettings.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace pose {

namespace {

constexpr float kBreastFlat = 0.002f;         ///< Overhang (m, figure-scaled) under which the chest is flat: no swing (the base males read 0).
constexpr float kBreastFull = 0.010f;         ///< ... over which the breast swings the whole amount (the base female reads 10 mm).
constexpr float kDomeFlat = 0.036f;           ///< A dome-alone patch's depth under which the chest is flat (a game rig's male reads 39).
constexpr float kDomeFull = 0.054f;           ///< ... over which it swings the whole amount (a game rig's woman reads 54).
constexpr float kFlushBand = 0.012f;          ///< A patch whose bottom row stands within this of the chest wall reaches the ribs (the native figures').
constexpr float kWallBand = 0.035f;           ///< The sternum: the chest wall's skin within this of the midline (m, figure-scaled).
constexpr float kWallHeight = 0.02f;          ///< ... and within this of the nipple's height.
constexpr float kFoldBand = 0.015f;           ///< The fold: the breast patch's skin within this of its lowest point (m, figure-scaled).
constexpr float kBreastSwingGainDeg = 16.0f;  ///< Degrees of swing per unit of gravity change perpendicular to the tether.
constexpr float kBreastSwingMaxDeg = 26.0f;   ///< The swing's cap (a figure upside down).
constexpr float kBreastHz = 2.4f;             ///< The bounce's frequency.
constexpr float kBreastDamping = 0.28f;       ///< Its damping ratio (under-damped: two or three visible swings).
constexpr float kBreastAccelMaxG = 2.0f;      ///< The chest's acceleration is read up to this, in g.
constexpr float kBreastGravitySmooth = 0.45f; ///< The effective gravity's low-pass per tick (a cursor's jitter is not a force).
constexpr float kTick = 1.0f / 60.0f;

} // namespace

void Armature::setupBreasts() {
    m_breasts.clear();
    m_breastsReady = false;
    if (!m_ikRig || !m_ikRig->roles().built() || m_bones.empty()) {
        return;
    }
    static const bool kTrace = std::getenv("POSESTUDIO_BREAST_TRACE") != nullptr;
    const RigRoles& roles = m_ikRig->roles();
    const float     scale = m_ikRig->sizeScale() > 0.0f ? m_ikRig->sizeScale() : 1.0f;
    // The bind joint positions (model space; binds are translation-only).
    std::vector<glm::vec3> bind(m_bones.size(), glm::vec3(0.0f));
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const Bone& b = m_bones[i];
        bind[i] = (b.parent >= 0 ? bind[static_cast<std::size_t>(b.parent)] : glm::vec3(0.0f)) + glm::vec3(b.localBind[3]);
    }
    for (const RigSide side : {RigSide::Left, RigSide::Right}) {
        Breast best;
        float  bestOut = -1.0f;
        for (const int bone : roles.all(RigRole::Pectoral, side)) {
            if (bone < 0 || static_cast<std::size_t>(bone) >= m_bones.size()) {
                continue;
            }
            // THE BREAST'S PROTRUSION BEYOND THE CHEST WALL: its skin's furthest point forward,
            // less the furthest point of the chest wall beside it — the sternum's skin, which the
            // spine link the pectoral hangs from owns, within kWallBand of the midline and within
            // the breast's own height. The joint says nothing (behind the breast on the native
            // figures, at its tip on a character-creation rig), and the bone's own skin DEPTH
            // read a flat native male chest at 70 mm (its pectoral owns a broad patch of chest
            // wall) against a full character-creation breast's 54.
            float zMin = std::numeric_limits<float>::max(), zMax = -std::numeric_limits<float>::max();
            float yMin = std::numeric_limits<float>::max();
            float yTip = 0.0f; // (the height of the breast's furthest point forward: the nipple's)
            int   points = 0;
            for (const BodyMeshPoint& p : m_bodyMesh) {
                if (p.bone != bone) {
                    continue;
                }
                zMin = std::min(zMin, p.pos.z);
                yMin = std::min(yMin, p.pos.y);
                if (p.pos.z > zMax) {
                    zMax = p.pos.z;
                    yTip = p.pos.y;
                }
                ++points;
            }
            // THE OVERHANG: the breast's tip against the skin at the BOTTOM of its own patch — the
            // fold, where the breast meets the ribs. A full breast hangs over its fold by a few
            // centimetres; a man's pectoral is flush with the ribs below it.
            float zUnder = -std::numeric_limits<float>::max();
            int   underPoints = 0;
            for (const BodyMeshPoint& p : m_bodyMesh) {
                if (p.bone != bone || p.pos.y > yMin + kFoldBand * scale) {
                    continue;
                }
                zUnder = std::max(zUnder, p.pos.z);
                ++underPoints;
            }
            const float overhang = underPoints > 0 ? std::max(0.0f, zMax - zUnder) : 0.0f;
            int wall = m_bones[static_cast<std::size_t>(bone)].parent;
            while (wall >= 0 && roles.role(wall) != RigRole::Spine && m_bones[static_cast<std::size_t>(wall)].parent >= 0) {
                wall = m_bones[static_cast<std::size_t>(wall)].parent;
            }
            float wallZ = -std::numeric_limits<float>::max();
            int   wallPoints = 0;
            if (wall >= 0 && points > 0) {
                // The sternum AT THE NIPPLE'S HEIGHT (a band of kWallHeight about it; twice that if
                // the skin is sparse there): over the breast's whole height the upper chest's bulge
                // stood in for the wall, and every figure read 1-18 mm.
                for (const float band : {kWallHeight * scale, 2.0f * kWallHeight * scale}) {
                    for (const BodyMeshPoint& p : m_bodyMesh) {
                        if (p.bone != wall || std::abs(p.pos.x) > kWallBand * scale || std::abs(p.pos.y - yTip) > band) {
                            continue;
                        }
                        wallZ = std::max(wallZ, p.pos.z);
                        ++wallPoints;
                    }
                    if (wallPoints > 0) {
                        break;
                    }
                }
            }
            const float depth = points > 0 ? zMax - zMin : 0.0f;
            const float beyondWall = wallPoints > 0 ? std::max(0.0f, zMax - wallZ) : 0.0f;
            // TWO KINDS OF PATCH. The native figures' pectoral owns the whole breast down to the
            // ribs (its bottom row sits FLUSH with the chest wall, within kFlushBand): the OVERHANG
            // says the size — 10 mm on the base female, 5 on the androgynous base, 0 on the males.
            // A game rig's breast bone owns the dome alone (its bottom row stands a centimetre and
            // more forward of the wall: the fold is the spine's skin): there the patch's DEPTH says
            // it — a dome's skin wraps 5 cm back on a full breast, 4 on a flat chest.
            const bool  holdsFold = points > 0 && (wallPoints == 0 || underPoints == 0 || zUnder - wallZ < kFlushBand * scale);
            const float out = holdsFold ? overhang : depth;
            const float flat = holdsFold ? kBreastFlat : kDomeFlat;
            const float full = holdsFold ? kBreastFull : kDomeFull;
            const float size = glm::clamp((out / scale - flat) / (full - flat), 0.0f, 1.0f);
            if (kTrace) {
                std::fprintf(stderr, "[breast] %s %-24s %s: overhang %.1f mm (bottom row %.1f forward of the wall), depth %.1f, beyond the wall %.1f (%s), %d points, scale %.2f -> size %.2f\n",
                             side == RigSide::Left ? "L" : "R", m_boneNames[static_cast<std::size_t>(bone)].c_str(), holdsFold ? "reaches the ribs" : "the dome alone",
                             overhang * 1000.0f, (zUnder - wallZ) * 1000.0f, depth * 1000.0f, beyondWall * 1000.0f,
                             wall >= 0 ? m_boneNames[static_cast<std::size_t>(wall)].c_str() : "-", points, scale, size);
            }
            if (points > 0 && out > bestOut) {
                bestOut = out;
                best.bone = bone;
                best.parent = m_bones[static_cast<std::size_t>(bone)].parent;
                best.size = size;
            }
        }
        if (best.bone >= 0 && best.parent >= 0 && best.size > 0.0f) {
            m_breasts.push_back(best);
        }
    }
    m_breastsReady = true;
    m_breastDynamic = false;
    m_breastChestSamples = 0;
    m_breastGravity = glm::vec3(0.0f, -1.0f, 0.0f);
}

void Armature::applySoftBodySettings() {
    if (softbody::breastMotion()) {
        if (!m_breastsReady && !m_bodyMesh.empty() && ensureIkRig()) {
            setupBreasts();
        }
    }
    m_breastDynamic = false;
    m_breastChestSamples = 0;
    computeSkinMatrices(); // (on: the resting swing; off: the bind's pectorals again)
}

glm::vec3 Armature::breastRestingSwing(const Breast& b) const {
    if (b.parent < 0 || static_cast<std::size_t>(b.parent) >= m_poseGlobal.size()) {
        return glm::vec3(0.0f);
    }
    // Gravity (effective, in g) in the chest's frame; the chest's rotation is its world rotation
    // away from the bind (binds are translation-only), the model transform taken off first.
    const glm::mat3 toModel = glm::inverse(glm::mat3(m_transform));
    const glm::vec3 gModel = toModel * m_breastGravity;
    glm::mat3       chest(m_poseGlobal[static_cast<std::size_t>(b.parent)]);
    for (int c = 0; c < 3; ++c) {
        const float len = glm::length(chest[c]);
        if (len > 1e-8f) {
            chest[c] /= len;
        }
    }
    const glm::vec3 gChest = glm::transpose(chest) * gModel;
    const glm::vec3 d0(0.0f, -1.0f, 0.0f); // (standing: what the sculpt already shows)
    const glm::vec3 r0(0.0f, 0.0f, 1.0f);  // (the tether: forward out of the chest)
    const glm::vec3 delta = gChest - d0;
    const glm::vec3 perp = delta - glm::dot(delta, r0) * r0;
    const float     amount = glm::length(perp);
    if (amount < 1e-4f) {
        return glm::vec3(0.0f);
    }
    const float angle = std::min(kBreastSwingMaxDeg, kBreastSwingGainDeg * amount) * b.size * softbody::breastStrength();
    return glm::normalize(glm::cross(r0, perp / amount)) * glm::radians(angle);
}

glm::mat4 Armature::breastSwingMatrix(const Breast& b, const glm::mat4& boneGlobal) const {
    const glm::vec3 rv = m_breastDynamic ? b.swing : breastRestingSwing(b);
    const float     angle = glm::length(rv);
    if (angle < 1e-5f || b.parent < 0) {
        return glm::mat4(1.0f);
    }
    glm::mat3 chest(m_poseGlobal[static_cast<std::size_t>(b.parent)]);
    for (int c = 0; c < 3; ++c) {
        const float len = glm::length(chest[c]);
        if (len > 1e-8f) {
            chest[c] /= len;
        }
    }
    const glm::vec3 axis = chest * (rv / angle); // (the swing is in the chest's frame: a turned chest carries it)
    const glm::vec3 joint(boneGlobal[3]);
    return glm::translate(glm::mat4(1.0f), joint) * glm::rotate(glm::mat4(1.0f), angle, axis) * glm::translate(glm::mat4(1.0f), -joint);
}

bool Armature::tickSoftBody(float dt) {
    if (!softbody::breastMotion() || !m_breastsReady || m_breasts.empty()) {
        return false;
    }
    dt = glm::clamp(dt, 1.0f / 240.0f, 1.0f / 20.0f);
    if (!m_breastDynamic) {
        m_breastChestSamples = 0; // (the first tick of a sequence: no velocity to read yet)
        for (Breast& b : m_breasts) {
            b.swing = breastRestingSwing(b);
            b.velocity = glm::vec3(0.0f);
        }
    }
    // EFFECTIVE GRAVITY: g less the chest's acceleration, off the chest joint's motion.
    const std::size_t chestBone = static_cast<std::size_t>(m_breasts.front().parent);
    const glm::vec3   chest = glm::vec3(m_transform * m_poseGlobal[chestBone][3]);
    glm::vec3         accel(0.0f);
    if (m_breastChestSamples >= 1) {
        const glm::vec3 vel = (chest - m_breastChestPos) / dt;
        if (m_breastChestSamples >= 2) {
            accel = (vel - m_breastChestVel) / dt;
        }
        m_breastChestVel = vel;
    }
    m_breastChestPos = chest;
    ++m_breastChestSamples;
    constexpr float g = 9.81f;
    const float     mag = glm::length(accel) / g;
    if (mag > kBreastAccelMaxG) {
        accel *= kBreastAccelMaxG / mag;
    }
    const glm::vec3 effective = glm::vec3(0.0f, -1.0f, 0.0f) - accel / g;
    m_breastGravity = glm::mix(m_breastGravity, effective, kBreastGravitySmooth);
    // THE SPRING toward the resting swing under that gravity.
    const float omega = 2.0f * 3.14159265f * kBreastHz;
    bool        moving = false;
    for (Breast& b : m_breasts) {
        const glm::vec3 target = breastRestingSwing(b);
        b.velocity += (omega * omega * (target - b.swing) - 2.0f * kBreastDamping * omega * b.velocity) * dt;
        b.swing += b.velocity * dt;
        const float most = glm::radians(kBreastSwingMaxDeg * 1.5f);
        const float len = glm::length(b.swing);
        if (len > most) {
            b.swing *= most / len;
        }
        if (glm::length(target - b.swing) > glm::radians(0.1f) || glm::length(b.velocity) > glm::radians(1.0f)) {
            moving = true;
        }
    }
    m_breastDynamic = true;
    computeSkinMatrices();
    return moving;
}

void Armature::settleSoftBody() {
    if (!m_breastDynamic) {
        return;
    }
    m_breastDynamic = false;
    m_breastChestSamples = 0;
    m_breastGravity = glm::vec3(0.0f, -1.0f, 0.0f);
    for (Breast& b : m_breasts) {
        b.velocity = glm::vec3(0.0f);
    }
    computeSkinMatrices(); // (the resting swing)
}

bool Armature::softBodyMoving() const {
    if (!m_breastDynamic || !softbody::breastMotion()) {
        return false;
    }
    for (const Breast& b : m_breasts) {
        if (glm::length(breastRestingSwing(b) - b.swing) > glm::radians(0.1f) || glm::length(b.velocity) > glm::radians(1.0f)) {
            return true;
        }
    }
    return false;
}

float Armature::breastSwingDeg(int side) const {
    if (!softbody::breastMotion() || !m_breastsReady) {
        return 0.0f;
    }
    for (const Breast& b : m_breasts) {
        const bool left = m_ikRig && m_ikRig->roles().side(b.bone) == RigSide::Left;
        if ((side == 0) == left) {
            return glm::degrees(glm::length(m_breastDynamic ? b.swing : breastRestingSwing(b)));
        }
    }
    return 0.0f;
}

} // namespace pose

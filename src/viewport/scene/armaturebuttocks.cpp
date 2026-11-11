/**
 * @file armaturebuttocks.cpp
 * @brief SOFT BUTTOCKS (2026-11-05; the user: "can we do something similar for the buttocks? It
 *        would have to be way less pronounced, and no hang at all in different poses, just some
 *        jiggle on movement, probably about 50 % of what we have on the breasts"): the glutes'
 *        skin jiggles with the hips' movement.
 *
 * NO BONE CARRIES THE BUTTOCKS — the glutes' skin is the pelvis bone's and the upper thighs' on
 * every rig seen — so the breasts' rigid skin transform is no use here. The buttocks are a
 * SOFT-BODY DELTA FIELD blended by the GPU exactly as the pose correctives are (correctiveset.h):
 * at import every render vertex near a cheek's fullest point gets a bind-space delta of the
 * field's weight along the figure's lateral axis and another along her trunk (softBodyFieldWeight:
 * a quartic bump of kButtockRadius about the APEX — the rearmost skin the pelvis bone or the hips
 * dominate on that side, behind the hip joint — on the skin the hips, the pelvis bone or that
 * side's thigh dominates, never in front of the hip joint), and the two CORRECTIVE WEIGHTS per
 * cheek are the mass's displacement in metres (softBodyWeight). The skinning then carries the
 * delta with the pelvis as it carries the skin, so a seated cheek jiggles in the seated pelvis's
 * frame; the field exists on every figure from the import (the vertex ranges ride in the vertex
 * data) and its weights read 0 while the preference is off.
 *
 * THE MASS: each cheek is a point mass on springs in the hips' frame — sideways and along the
 * trunk, nothing in and out — with NO RESTING DISPLACEMENT: a pose changes nothing (no hang; the
 * breasts' pose term is left out), only the hips' ACCELERATION moves it, read as the breasts'
 * chest is (the hip joint's velocity smoothed, differenced, smoothed again, a dead band, a cap,
 * kButtockJoltShare of it felt), with the breasts' figure of eight (a drop spreads the cheeks a
 * little) and the landing's KICK. Compliance half the breasts' sideways and the breasts' own
 * along the trunk (kButtockSide, kButtockDown: the first look read the up-and-down too subtle
 * at half), its own frequency and damping (kButtockHz, kButtockDamping), substepped, the
 * stiffening capped. Nothing of it is in the pose. POSESTUDIO_BREAST_TRACE=2 prints the left
 * cheek per tick beside the breast; POSESTUDIO_BUTTOCK_DOWN / _SIDE (mm per g) and _IMPACT are
 * the levers.
 */

#include "armature.h"

#include "ikmath.h" // normalizedFrame
#include "ikrig.h"
#include "rigroles.h"
#include "softbodysettings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace pose {

namespace {

constexpr float kButtockRadius = 0.13f;        ///< The field's reach about a cheek's apex (m, figure-scaled): the glute and the hip's side, fading to nothing at the edge.
constexpr float kButtockBehind = 0.02f;        ///< The field reaches at most this far in front of the hip joint's depth (m, figure-scaled): the belly is not a buttock.
constexpr float kButtockApexBand = 0.25f;      ///< The apex is looked for from this far below the hip joint ... (m, figure-scaled)
constexpr float kButtockApexAbove = 0.10f;     ///< ... to this far above it.
constexpr float kButtockDownDefault = 0.018f;  ///< Compliance along the trunk (m per g): the breasts' 18 at fullness 1 (9, half, at the first look: "side by side looks good, up and down is too subtle").
constexpr float kButtockSideDefault = 0.015f;  ///< Compliance sideways (m per g): half the breasts' 30.
constexpr float kButtockHz = 2.4f;             ///< The bounce's frequency (the breasts' at fullness 1).
constexpr float kButtockDamping = 0.6f;        ///< Its damping ratio: one swing past the rest and a settle.
constexpr float kButtockCapDefault = 0.020f;   ///< The stiffening spring's cap (m): the excursion past which the tissue runs out.
constexpr float kButtockStiffenMax = 16.0f;    ///< The stiffening factor is capped here (stability with the substeps).
constexpr int   kButtockSubsteps = 4;          ///< The spring's substeps per tick (semi-implicit Euler: omega × step well under 2).
constexpr float kButtockSplay = 0.3f;          ///< THE FIGURE OF EIGHT: the sideways rest moves outward by this share of the drop.
constexpr float kButtockDetune = 0.985f;       ///< The right cheek's frequency against the left's.
constexpr float kButtockImpactShareDefault = 0.4f; ///< THE LANDING'S KICK: the share of the impact speed the cheeks keep (0.25, half the breasts' 0.5, at the first look: the drop read too subtle).
constexpr float kButtockJoltShare = 0.7f;      ///< The share of the hips' acceleration the mass feels (the breasts' rule).
constexpr float kButtockVelSmooth = 0.4f;      ///< The hip joint's velocity low-passed by this per tick before it is differenced ...
constexpr float kButtockAccelSmooth = 0.3f;    ///< ... and the acceleration by this after (a trembling hand is a g and a half raw).
constexpr float kButtockAccelDeadG = 0.15f;    ///< Accelerations under this (g) are read as none.
constexpr float kButtockAccelMaxG = 1.5f;      ///< ... and read up to this.

float envOrF(const char* name, float fallback) {
    const char* v = std::getenv(name);
    return v && *v ? static_cast<float>(std::atof(v)) : fallback;
}
const float kButtockDown = envOrF("POSESTUDIO_BUTTOCK_DOWN", kButtockDownDefault * 1000.0f) / 1000.0f;
const float kButtockSide = envOrF("POSESTUDIO_BUTTOCK_SIDE", kButtockSideDefault * 1000.0f) / 1000.0f;
const float kButtockCap = envOrF("POSESTUDIO_BUTTOCK_CAP", kButtockCapDefault * 1000.0f) / 1000.0f;
const float kButtockImpactShare = envOrF("POSESTUDIO_BUTTOCK_IMPACT", kButtockImpactShareDefault);

} // namespace

void Armature::setupButtocks() {
    m_buttocks.clear();
    m_buttocksReady = false;
    m_buttockDynamic = false;
    m_buttockRoot = -1;
    m_buttockAllowed.clear();
    if (!m_ikRig || !m_ikRig->roles().built() || m_bones.empty() || m_bodyMesh.empty()) {
        return;
    }
    static const bool kTrace = std::getenv("POSESTUDIO_BREAST_TRACE") != nullptr;
    const RigRoles& roles = m_ikRig->roles();
    const float     scale = m_ikRig->sizeScale() > 0.0f ? m_ikRig->sizeScale() : 1.0f;
    const int       root = m_ikRig->rootNode();
    if (root < 0 || static_cast<std::size_t>(root) >= m_bones.size()) {
        return;
    }
    const std::vector<glm::vec3> bind = bindJointPositions(); // (model space; binds are translation-only)
    const glm::vec3              hips = bind[static_cast<std::size_t>(root)];
    // THE SKIN THAT MAY CARRY THE FIELD: the hips' and the pelvis bone's (either cheek), each
    // side's thigh and its twist helpers (that side's cheek). 1 = either side, 2 = left, 3 = right.
    m_buttockAllowed.assign(m_bones.size(), 0);
    auto mark = [&](RigRole role, RigSide side, unsigned char value) {
        for (const int b : roles.all(role, side)) {
            if (b >= 0 && static_cast<std::size_t>(b) < m_bones.size()) {
                m_buttockAllowed[static_cast<std::size_t>(b)] = value;
            }
        }
    };
    mark(RigRole::Hips, RigSide::Centre, 1);
    mark(RigRole::Pelvis, RigSide::Centre, 1);
    mark(RigRole::Thigh, RigSide::Left, 2);
    mark(RigRole::ThighTwist, RigSide::Left, 2);
    mark(RigRole::Thigh, RigSide::Right, 3);
    mark(RigRole::ThighTwist, RigSide::Right, 3);
    m_buttockRadius = kButtockRadius * scale;
    m_buttockFront = hips.z + kButtockBehind * scale;
    // THE APEX per side: the rearmost skin point of the allowed bones on that side (her left is
    // +x), behind the hip joint and within the glute's band of heights — the cheek's fullest
    // point, which the hamstring never outreaches.
    for (const float side : {1.0f, -1.0f}) {
        Buttock cheek;
        cheek.side = side;
        float zMin = std::numeric_limits<float>::max();
        int   points = 0;
        for (const BodyMeshPoint& p : m_bodyMesh) {
            if (p.bone < 0 || static_cast<std::size_t>(p.bone) >= m_buttockAllowed.size()) {
                continue;
            }
            const unsigned char allowed = m_buttockAllowed[static_cast<std::size_t>(p.bone)];
            const unsigned char want = side > 0.0f ? 2 : 3;
            if (allowed == 0 || (allowed != 1 && allowed != want)) {
                continue;
            }
            if (p.pos.x * side < 0.0f || p.pos.z > hips.z || p.pos.y < hips.y - kButtockApexBand * scale || p.pos.y > hips.y + kButtockApexAbove * scale) {
                continue;
            }
            ++points;
            if (p.pos.z < zMin) {
                zMin = p.pos.z;
                cheek.apexBind = p.pos;
            }
        }
        if (points == 0) {
            continue;
        }
        if (kTrace) {
            std::fprintf(stderr, "[buttock] %s apex %.1f mm behind and %.1f mm below the hip joint, %.1f mm out (%d points), field radius %.0f mm\n",
                         side > 0.0f ? "L" : "R", (hips.z - cheek.apexBind.z) * 1000.0f, (hips.y - cheek.apexBind.y) * 1000.0f,
                         std::abs(cheek.apexBind.x) * 1000.0f, points, m_buttockRadius * 1000.0f);
        }
        m_buttocks.push_back(cheek);
    }
    if (m_buttocks.size() == 2) {
        m_buttockRoot = root;
        m_buttocksReady = true;
    } else {
        m_buttocks.clear();
    }
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

float Armature::softBodyFieldWeight(const glm::vec3& bindPos, int dominantBone, int& side) const {
    side = -1;
    if (!m_buttocksReady || dominantBone < 0 || static_cast<std::size_t>(dominantBone) >= m_buttockAllowed.size()) {
        return 0.0f;
    }
    const unsigned char allowed = m_buttockAllowed[static_cast<std::size_t>(dominantBone)];
    if (allowed == 0 || bindPos.z > m_buttockFront) {
        return 0.0f;
    }
    float best = 0.0f;
    for (std::size_t i = 0; i < m_buttocks.size(); ++i) {
        const Buttock& cheek = m_buttocks[i];
        const unsigned char want = cheek.side > 0.0f ? 2 : 3;
        if (allowed != 1 && allowed != want) {
            continue;
        }
        const float d = glm::length(bindPos - cheek.apexBind) / m_buttockRadius;
        if (d >= 1.0f) {
            continue;
        }
        const float t = 1.0f - d * d;
        const float w = t * t; // (a quartic bump: smooth to the edge)
        if (w > best) {
            best = w;
            side = static_cast<int>(i);
        }
    }
    return best;
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

void Armature::tickButtocks(float dt, bool& movingOut) {
    if (!softbody::buttockMotion() || !m_buttocksReady || m_buttocks.empty() || m_buttockRoot < 0 ||
        static_cast<std::size_t>(m_buttockRoot) >= m_poseGlobal.size()) {
        return;
    }
    if (!m_buttockDynamic) {
        m_buttockReader.reset(); // (the first tick of a sequence: no velocity to read yet)
        for (Buttock& b : m_buttocks) {
            b.disp = glm::vec3(0.0f);
            b.vel = glm::vec3(0.0f);
        }
    }
    // THE HIPS' ACCELERATION, read as the breasts read the chest's: the hip joint's velocity
    // smoothed before it is differenced, the acceleration smoothed after, the dead band, the cap.
    float           mag = 0.0f;
    const glm::vec3 felt = m_buttockReader.sample(glm::vec3(m_transform * m_poseGlobal[static_cast<std::size_t>(m_buttockRoot)][3]), dt,
                                                  kButtockVelSmooth, kButtockAccelSmooth, kButtockAccelDeadG, kButtockAccelMaxG, mag);
    (void)mag;
    // Into the hips' frame (the model transform taken off first): the rest the mass is drawn to is
    // the acceleration's alone — a pose changes nothing — sideways and along the trunk.
    const glm::mat3 toModel = glm::inverse(glm::mat3(m_transform));
    const glm::mat3 frame = normalizedFrame(m_poseGlobal[static_cast<std::size_t>(m_buttockRoot)]);
    const glm::vec3 aFrame = glm::transpose(frame) * (toModel * felt) * kButtockJoltShare;
    const float     strength = softbody::buttockStrength(); // (the tab's Softness of the buttocks' own; the breasts' until 2026-11-07)
    const glm::vec3 restBase(-kButtockSide * aFrame.x * strength, -kButtockDown * aFrame.y * strength, 0.0f);
    const glm::vec3 damping(kButtockDamping, kButtockDamping, kButtockDamping);
    const float     h = dt / static_cast<float>(kButtockSubsteps);
    bool            moving = false;
    for (Buttock& b : m_buttocks) {
        const float omega = 2.0f * 3.14159265f * kButtockHz * (b.side < 0.0f ? kButtockDetune : 1.0f);
        glm::vec3   rest = restBase;
        for (int k = 0; k < kButtockSubsteps; ++k) {
            rest = restBase;
            rest.x += b.side * kButtockSplay * (0.0f - b.disp.y); // (the figure of eight: a drop spreads the cheeks)
            const glm::vec3 excursion = b.disp - rest;
            const float     stiffen = std::min(kButtockStiffenMax, 1.0f + glm::dot(excursion, excursion) / (kButtockCap * kButtockCap));
            b.vel += (omega * omega * stiffen * (rest - b.disp) - 2.0f * damping * omega * b.vel) * h;
            b.disp += b.vel * h;
            b.disp.z = 0.0f; // (nothing in and out)
            b.vel.z = 0.0f;
        }
        if (!std::isfinite(b.disp.x + b.disp.y + b.vel.x + b.vel.y)) {
            b.disp = glm::vec3(0.0f);
            b.vel = glm::vec3(0.0f);
        }
        if (glm::length(rest - b.disp) > 0.0003f || glm::length(b.vel) > 0.003f) {
            moving = true;
        }
    }
    m_buttockDynamic = true;
    movingOut = movingOut || moving;
    static const bool kTickTrace = std::getenv("POSESTUDIO_BREAST_TRACE") && std::getenv("POSESTUDIO_BREAST_TRACE")[0] == '2';
    if (kTickTrace) {
        const Buttock& b = m_buttocks.front();
        std::fprintf(stderr, "[buttock-tick] accel %.2f g | disp %+.1f %+.1f mm (rest %+.1f %+.1f)\n", mag * kButtockJoltShare,
                     b.disp.x * 1000.0f, b.disp.y * 1000.0f, restBase.x * 1000.0f, restBase.y * 1000.0f);
    }
}

void Armature::kickButtocks(float impactSpeed) {
    if (!softbody::buttockMotion() || !m_buttocksReady || m_buttocks.empty() || m_buttockRoot < 0 || impactSpeed <= 0.0f ||
        static_cast<std::size_t>(m_buttockRoot) >= m_poseGlobal.size()) {
        return;
    }
    if (!m_buttockDynamic) {
        for (Buttock& b : m_buttocks) {
            b.disp = glm::vec3(0.0f);
            b.vel = glm::vec3(0.0f);
        }
        m_buttockDynamic = true;
    }
    // The kick is the landing's whole deceleration: the reader forgets the fall.
    m_buttockReader.reset();
    const glm::mat3 toModel = glm::inverse(glm::mat3(m_transform));
    const glm::mat3 frame = normalizedFrame(m_poseGlobal[static_cast<std::size_t>(m_buttockRoot)]);
    const glm::vec3 downFrame = glm::transpose(frame) * (toModel * glm::vec3(0.0f, -1.0f, 0.0f));
    for (Buttock& b : m_buttocks) {
        b.vel += glm::vec3(downFrame.x, downFrame.y, 0.0f) * (impactSpeed * kButtockImpactShare * softbody::buttockStrength());
    }
}

void Armature::settleButtocks() {
    m_buttockDynamic = false;
    m_buttockReader.reset();
    for (Buttock& b : m_buttocks) {
        b.disp = glm::vec3(0.0f);
        b.vel = glm::vec3(0.0f);
    }
}

bool Armature::buttocksMoving() const {
    if (!m_buttockDynamic || !softbody::buttockMotion()) {
        return false;
    }
    for (const Buttock& b : m_buttocks) {
        if (glm::length(b.disp) > 0.0003f || glm::length(b.vel) > 0.003f) {
            return true;
        }
    }
    return false;
}

float Armature::buttockMoveMm(int side) const {
    if (!softbody::buttockMotion() || !m_buttocksReady || !m_buttockDynamic) {
        return 0.0f;
    }
    for (const Buttock& b : m_buttocks) {
        if ((side == 0) == (b.side > 0.0f)) {
            return 1000.0f * glm::length(b.disp);
        }
    }
    return 0.0f;
}

} // namespace pose

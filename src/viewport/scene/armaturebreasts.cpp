/**
 * @file armaturebreasts.cpp
 * @brief SOFT BREASTS (2026-11-04; the user: "natural breasts will hang and move based on the
 *        position of the torso and gravity, and depending on their size"): the pectoral bones —
 *        the rig standard's Pectoral role, which every native generation and the character-
 *        creation rigs carry with the breast's skin on them — are turned by what GRAVITY does to
 *        a breast that the sculpt has at rest STANDING, and sized by the breast the figure
 *        actually has.
 *
 * THE MODEL (2026-11-05, the user's fourth look with four animated references: "breasts,
 * especially larger ones, bounce and jiggle in a particular way"): a breast is a point MASS on
 * anisotropic springs in the chest's frame — x sideways, y along the chest, z the tether out of
 * it (the figure facing +z). Standing, gravity pulls it down the chest and the sculpt already
 * shows that; what a pose or a move changes is the gravity the mass feels IN THE CHEST'S FRAME,
 * EFFECTIVE gravity (g less the chest's acceleration), and its resting displacement from the
 * sculpt is that change times a COMPLIANCE per axis (kBreastDown, kBreastSide, kBreastHangOut:
 * metres per g at fullness 1, by the breast's weight): softest along the chest — the bounce —
 * firm sideways, and out along the tether the hang, which never goes INTO the chest (lying on
 * the back gravity presses the breast flat, a morph's job). Rendered, a displacement ACROSS the
 * tether is a swing about the breast's upper attachment (kBreastLever over the nipple on the
 * chest wall: the pendulum's point) that carries the nipple there, one ALONG it a translation;
 * a dropping breast also splays outward by kBreastSplay of its drop — the figure of eight a
 * bouncing breast traces. Bowed forward the sag toward the belly is relieved and the breast is
 * drawn out toward the floor, the nipple leading; lying on the back it lifts toward the chin; on
 * the side it falls to the lower side; upside down it falls toward the head.
 *
 * THE SIZE: the body mesh sample (the skin the pectoral bone dominates) says how FULL the breast
 * is — its PROFILE, the skin's bulge beyond the chest wall integrated over its height at the
 * nipple's x (kBreastAreaFlat .. kBreastAreaFull is flat .. the base female, and it runs on to
 * kBreastFullnessMax for a large bust), or for a game rig's dome-alone patch its depth. The hang
 * and the swing scale with that fullness to kBreastFullnessPower: a large breast hangs and
 * bounces a great deal, a small one little, a flat chest (a male figure, a child) not at all.
 * One pectoral per side carries the breast (the one with the biggest patch of skin: a rib twist
 * beside it owns a few points); the rest ride as before.
 *
 * THE MOTION: while the engine ticks (a drag, its settle's tail, the Ground button's fall and
 * landing bounce) the mass is a damped spring toward its resting displacement (kBreastHz, a
 * fuller breast slower; kBreastDamping 0.25: three or four visible swings after a QUICK move,
 * none after a slow one — the bounce is speed-dependent because what drives it is the chest's
 * ACCELERATION, read off the chest joint's motion tick to tick up to kBreastAccelMaxG; a slow bow
 * moves the rest slowly and the mass follows it), STIFFENING past kBreastBounceCap (tissue runs
 * out), the right breast a little slower than the left (kBreastDetune: they never move quite
 * together). A falling figure's breasts float up weightless; at the LANDING they keep
 * kBreastImpactShare of the impact speed as the chest stops (kickSoftBody, from
 * beginLandingBounce) and drop a hand's breadth on a large bust, then swing three or four times.
 * Outside the ticks (a dial, a pose file, undo) the resting displacement applies at once.
 * Nothing of it is in the pose: the channels, the pose files, the undo stack, the harness's
 * measures and the joints' positions read as they did; balance, the IK and the volumes never see
 * it, and the IK settle does not wait for it (it ticks the breasts alone once the pose is at
 * rest). OFF unless the Posing preference turns it on (softbody::breastMotion): the harness runs
 * the engine as it was. POSESTUDIO_BREAST_TRACE=1 prints each candidate's profile and the
 * fullness it was given; =2 the displacement per tick against its rest.
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

constexpr float kBreastAreaFlat = 12.0f;      ///< THE BREAST'S PROFILE (cm², figure-scaled: the skin's bulge beyond the chest wall, integrated over the breast's height at the nipple's x) under which the chest is flat: no motion (the base males read 6-10, the androgynous base 9).
constexpr float kBreastAreaFull = 39.0f;      ///< ... at which the breast is the base female's: fullness 1. It is NOT capped there — a large-busted character reads 52-64 (fullness 1.5-1.9).
constexpr float kBreastFullnessMax = 3.0f;    ///< Fullness runs on to this.
constexpr float kBreastFullnessPower = 1.5f;  ///< The hang and the swing scale with fullness to this power (a larger breast is heavier for its stiffness: the user's "more pronounced if the breasts are larger, none for very small").
constexpr float kBulgeBand = 0.03f;           ///< The profile is read within this of the nipple's x, the chest wall within this above and below the patch (m, figure-scaled).
constexpr float kDomeFlat = 0.036f;           ///< A dome-alone patch's depth under which the chest is flat (a game rig's male reads 39).
constexpr float kDomeFull = 0.054f;           ///< ... over which it swings the whole amount (a game rig's woman reads 54).
constexpr float kFlushBand = 0.012f;          ///< A patch whose bottom row stands within this of the chest wall reaches the ribs (the native figures').
constexpr float kWallBand = 0.035f;           ///< The sternum: the chest wall's skin within this of the midline (m, figure-scaled).
constexpr float kWallHeight = 0.02f;          ///< ... and within this of the nipple's height.
constexpr float kFoldBand = 0.015f;           ///< The fold: the breast patch's skin within this of its lowest point (m, figure-scaled).
// THE MASS: a breast is a point mass on anisotropic springs in the chest's frame (x lateral, y along
// the chest, z the tether out of it). What a pose or a move changes is the gravity it feels there,
// EFFECTIVE gravity (g less the chest's acceleration), and the mass's resting displacement from the
// sculpt is that change times a COMPLIANCE per axis — metres per g at fullness 1, scaled by the
// breast's weight (its fullness to kBreastFullnessPower): softest along the chest (the bounce),
// firm sideways, and out along the tether the hang, which never goes INTO the chest (lying on the
// back gravity presses the breast flat, which is a morph's job, not a translation's).
constexpr float kBreastDownDefault = 0.015f;        ///< Compliance along the chest (m per g): a landing's 2 g drops the base female's breast 30 mm, a large bust 50.
constexpr float kBreastSideDefault = 0.020f;        ///< Compliance sideways (m per g): on her side the breasts fall toward the lower side — the upper one in toward the sternum, the lower one out onto the surface (8 until the fifth look: "laying on her side should cause the breasts to fall naturally").
constexpr float kBreastHangOutDefault = 0.030f;     ///< Compliance out along the tether (m per g): a level-chested bow hangs the base female's breast 30 mm (25 and 35 looked at, 30 between).
constexpr float kBreastSplayDefault = 0.10f;        ///< (0.35, then 0.25: the sideways slosh of a dropping breast read as a sack of fluid — the user's sixth look.)        ///< Sideways OUTWARD displacement per unit of downward displacement: a breast that drops splays, one that rises comes in — the figure of eight a bouncing breast traces.
constexpr float kBreastLeverDefault = 0.050f;       ///< The swing's pivot stands this far over the nipple on the chest wall (m, figure-scaled): the breast's upper attachment, the pendulum's point. A displacement across the tether is rendered as a swing about it (the nipple follows the mass), one along the tether as a translation.
constexpr float kBreastBounceCapDefault = 0.030f;   ///< A stiffening spring: the displacement from the sculpt costs more past this (m at fullness 1, by the square root of the weight) — tissue runs out, and a 3 m/s landing would otherwise fling the mass a quarter of a metre.
constexpr float kBreastHzDefault = 2.4f;            ///< The bounce's frequency at fullness 1; a fuller breast slower by the fourth root of its weight (a large bust at 2.1 Hz, a small one at 3.8). Tissue answers quickly and stops; fluid sloshes slowly (2.0 read as a sack of fluid).
constexpr float kBreastDampingDefault = 0.6f;       ///< Its damping ratio across the tether: one swing past the rest and a settle — tissue, not gel (0.25 rang three or four times and read as jello, the user's fifth look: "breasts are a bit more solid than a springy gel"; 0.45 still read as "sacks filled with fluid", the sixth; 0.6 at the old joint pivot had shown nothing, but that model moved a tenth as far).
constexpr float kBreastTetherDamping = 0.85f;       ///< ... and ALONG the tether, heavier still: a breast hardly stretches and recoils in its length.
constexpr float kBaseBelowFold = 0.03f;             ///< The chest wall under the breast: the wall bone's skin within this below the patch's bottom (m, figure-scaled).
constexpr float kBreastImpactShare = 0.3f;         ///< THE LANDING'S KICK: the share of the impact speed the breasts keep as the chest stops (the whole of it flung a large bust a hand's length down the chest, past what tissue allows; 0.3 drops it 5 cm and bounces it three times).
constexpr float kBreastDetune = 0.985f;              ///< The right breast's frequency against the left's: the two never move quite together.
constexpr float kBreastAccelMaxG = 2.5f;            ///< The chest's acceleration is read up to this, in g (a quick drag's start and stop, a landing's deceleration are what set the breasts bouncing).
constexpr float kBreastGravitySmooth = 0.35f;       ///< The effective gravity's low-pass per tick (a cursor's jitter is not a force; a drag's jolt is; the IK's tick-to-tick tremble of the chest under a landing is not either).
constexpr float kBreastAccelDeadG = 0.05f;          ///< Accelerations under this (g) are read as none.
// ON HER BACK (the fifth look, with photographs: "when a character is laying flat on their back,
// the breasts should flatten a bit and fall a bit to the sides"): gravity INTO the chest presses
// the breast back toward the wall — the nipple by kBreastPressShare of the breast's own depth per
// g, never past kBreastPressMax of it (a rigid skin cannot flatten; pushed back a third of its
// depth with the chest wall's skin staying, the breast reads flatter and wider) — and spreads it
// outward by kBreastSpreadOnBack per g, both by the weight.
constexpr float kBreastPressShare = 0.30f;
constexpr float kBreastPressMax = 0.40f;
constexpr float kBreastSpreadOnBackDefault = 0.018f;

/// A/B levers for the look (mm per g: POSESTUDIO_BREAST_DOWN, _SIDE, _HANG; _SPLAY share; _LEVER mm; _CAP mm; _HZ; _DAMPING): read once.
float envOrF(const char* name, float fallback) {
    const char* v = std::getenv(name);
    return v && *v ? static_cast<float>(std::atof(v)) : fallback;
}
const float kBreastDown = envOrF("POSESTUDIO_BREAST_DOWN", kBreastDownDefault * 1000.0f) / 1000.0f;
const float kBreastSide = envOrF("POSESTUDIO_BREAST_SIDE", kBreastSideDefault * 1000.0f) / 1000.0f;
const float kBreastHangOut = envOrF("POSESTUDIO_BREAST_HANG", kBreastHangOutDefault * 1000.0f) / 1000.0f;
const float kBreastSplay = envOrF("POSESTUDIO_BREAST_SPLAY", kBreastSplayDefault);
const float kBreastSpreadOnBack = envOrF("POSESTUDIO_BREAST_SPREAD", kBreastSpreadOnBackDefault * 1000.0f) / 1000.0f;
const float kBreastLever = envOrF("POSESTUDIO_BREAST_LEVER", kBreastLeverDefault * 1000.0f) / 1000.0f;
const float kBreastBounceCap = envOrF("POSESTUDIO_BREAST_CAP", kBreastBounceCapDefault * 1000.0f) / 1000.0f;
const float kBreastHz = envOrF("POSESTUDIO_BREAST_HZ", kBreastHzDefault);
const float kBreastDamping = envOrF("POSESTUDIO_BREAST_DAMPING", kBreastDampingDefault);

/// The weight a breast's motion is scaled by: its fullness (0 flat .. 1 the base female .. 3) to kBreastFullnessPower.
float breastWeight(float fullness) {
    return fullness <= 0.0f ? 0.0f : std::pow(fullness, kBreastFullnessPower);
}

/// The lateral displacement as rendered: the mass's own plus the splay of a dropping breast
/// (outward, by the side), the figure of eight.
float breastRenderedSide(float dispX, float dispY, float side) {
    return dispX + side * kBreastSplay * std::max(0.0f, -dispY);
}
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
    // ONE READING PER CANDIDATE, THE CHOICE AFTER. A side may carry more than one Pectoral-role
    // bone (a game rig's rib twist beside its breast bone), and the breast is the one with the
    // BIGGEST PATCH of skin — 175 points against the twist's 17 on one character-creation rig,
    // 3,436 on another; a native pectoral is its side's only candidate. Chosen by the largest
    // protrusion instead (until 2026-11-05), a 17-point rib twist's "overhang" of 13 mm read as a
    // full breast on one side while the breast bone on the other read a quarter: the two
    // measures (an overhang, a depth) are not commensurate, and a tiny patch's extremes are noise.
    struct Reading {
        int   bone = -1, parent = -1, points = 0, underPoints = 0, wallPoints = 0;
        float overhang = 0.0f, depth = 0.0f, zUnder = 0.0f, wallZ = 0.0f, bindZ = 0.0f, xTip = 0.0f, yTip = 0.0f;
        float zMax = 0.0f, yMin = 0.0f, yMax = 0.0f;
        float area = 0.0f; ///< THE PROFILE: the skin's bulge beyond the chest wall integrated over the breast's height at the nipple's x (cm², figure-scaled).
        float baseZ = -std::numeric_limits<float>::max(); ///< The chest wall's skin at the breast's x just below the patch (the ribs under the fold), where found.
        int   basePoints = 0;
        bool  flush = true; ///< The patch's bottom row within kFlushBand of the chest wall (or no wall / no bottom row read).
    };
    Reading chosen[2];
    for (const RigSide side : {RigSide::Left, RigSide::Right}) {
        Reading& pick = chosen[side == RigSide::Left ? 0 : 1];
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
            Reading r;
            r.bone = bone;
            r.parent = m_bones[static_cast<std::size_t>(bone)].parent;
            r.bindZ = bind[static_cast<std::size_t>(bone)].z;
            float zMin = std::numeric_limits<float>::max(), zMax = -std::numeric_limits<float>::max();
            float yMin = std::numeric_limits<float>::max(), yMax = -std::numeric_limits<float>::max();
            for (const BodyMeshPoint& p : m_bodyMesh) {
                if (p.bone != bone) {
                    continue;
                }
                zMin = std::min(zMin, p.pos.z);
                yMin = std::min(yMin, p.pos.y);
                yMax = std::max(yMax, p.pos.y);
                if (p.pos.z > zMax) {
                    zMax = p.pos.z;
                    r.zMax = zMax;
                    r.yTip = p.pos.y; // (the breast's furthest point forward: the nipple)
                    r.xTip = p.pos.x;
                }
                ++r.points;
            }
            if (r.points == 0) {
                continue;
            }
            r.yMin = yMin;
            r.yMax = yMax;
            // THE OVERHANG: the breast's tip against the skin at the BOTTOM of its own patch — the
            // fold, where the breast meets the ribs. A full breast hangs over its fold by a few
            // centimetres; a man's pectoral is flush with the ribs below it.
            r.zUnder = -std::numeric_limits<float>::max();
            for (const BodyMeshPoint& p : m_bodyMesh) {
                if (p.bone != bone || p.pos.y > yMin + kFoldBand * scale) {
                    continue;
                }
                r.zUnder = std::max(r.zUnder, p.pos.z);
                ++r.underPoints;
            }
            r.overhang = r.underPoints > 0 ? std::max(0.0f, zMax - r.zUnder) : 0.0f;
            int wall = r.parent;
            while (wall >= 0 && roles.role(wall) != RigRole::Spine && m_bones[static_cast<std::size_t>(wall)].parent >= 0) {
                wall = m_bones[static_cast<std::size_t>(wall)].parent;
            }
            r.wallZ = -std::numeric_limits<float>::max();
            if (wall >= 0) {
                // THE BASE'S DEPTH: the chest wall's skin at the breast's own x just BELOW the patch —
                // the ribs under the fold, which the spine link owns on every rig. On the base
                // female they stand 25 mm behind the nipple where the fold's own skin (the patch's
                // bottom row, the first pivot depth) stands 13: a pivot at the fold tilted the
                // breast about a point a finger's breadth behind the nipple.
                for (const BodyMeshPoint& p : m_bodyMesh) {
                    if (p.bone != wall || std::abs(p.pos.x - r.xTip) > kWallHeight * scale || p.pos.y > yMin || p.pos.y < yMin - kBaseBelowFold * scale) {
                        continue;
                    }
                    r.baseZ = std::max(r.baseZ, p.pos.z);
                    ++r.basePoints;
                }
                // The sternum AT THE NIPPLE'S HEIGHT (a band of kWallHeight about it; twice that if
                // the skin is sparse there): over the breast's whole height the upper chest's bulge
                // stood in for the wall, and every figure read 1-18 mm.
                for (const float band : {kWallHeight * scale, 2.0f * kWallHeight * scale}) {
                    for (const BodyMeshPoint& p : m_bodyMesh) {
                        if (p.bone != wall || std::abs(p.pos.x) > kWallBand * scale || std::abs(p.pos.y - r.yTip) > band) {
                            continue;
                        }
                        r.wallZ = std::max(r.wallZ, p.pos.z);
                        ++r.wallPoints;
                    }
                    if (r.wallPoints > 0) {
                        break;
                    }
                }
            }
            r.depth = zMax - zMin;
            r.flush = r.wallPoints == 0 || r.underPoints == 0 || r.zUnder - r.wallZ < kFlushBand * scale;
            // THE PROFILE AREA — the breast's size as the eye reads it from the side. The chest wall
            // is a LINE through the ribs under the fold (above) and the wall bone's skin just over the
            // patch's top where it has any in front of the patch's own rearmost skin, else the patch's
            // own top rows (a native pectoral's top is the upper chest by the collarbone, flush with
            // the wall); every patch point within kBulgeBand of the nipple's x contributes what it
            // stands forward of that line, summed over the patch's height. The nipple's protrusion
            // alone under-read every heavy build (the ribs under the fold bulge forward: one read 18 mm
            // against the base female's 22 while plainly fuller, 46 cm² against 39 here) and the
            // overhang could not tell a large breast from the base at all (9.6 against 10.0 mm).
            if (r.basePoints > 0) {
                const float band = kBulgeBand * scale;
                float yUp = 0.0f, zUp = -std::numeric_limits<float>::max();
                int   upPoints = 0;
                if (wall >= 0) {
                    for (const BodyMeshPoint& p : m_bodyMesh) {
                        if (p.bone != wall || std::abs(p.pos.x - r.xTip) > band || p.pos.y < yMax || p.pos.y > yMax + band || p.pos.z <= zMin) {
                            continue;
                        }
                        yUp += p.pos.y;
                        zUp = std::max(zUp, p.pos.z);
                        ++upPoints;
                    }
                }
                if (upPoints == 0) {
                    for (const BodyMeshPoint& p : m_bodyMesh) {
                        if (p.bone != bone || std::abs(p.pos.x - r.xTip) > band || p.pos.y < yMax - 0.02f * scale) {
                            continue;
                        }
                        yUp += p.pos.y;
                        zUp = std::max(zUp, p.pos.z);
                        ++upPoints;
                    }
                }
                float yRibs = 0.0f;
                int   ribPoints = 0;
                for (const BodyMeshPoint& p : m_bodyMesh) {
                    if (p.bone != wall || std::abs(p.pos.x - r.xTip) > kWallHeight * scale || p.pos.y > yMin || p.pos.y < yMin - kBaseBelowFold * scale) {
                        continue;
                    }
                    yRibs += p.pos.y;
                    ++ribPoints;
                }
                if (upPoints > 0 && ribPoints > 0) {
                    yUp /= static_cast<float>(upPoints);
                    yRibs /= static_cast<float>(ribPoints);
                    const float zRibs = r.baseZ;
                    float       bulge = 0.0f;
                    int         bandPoints = 0;
                    for (const BodyMeshPoint& p : m_bodyMesh) {
                        if (p.bone != bone || std::abs(p.pos.x - r.xTip) > band) {
                            continue;
                        }
                        const float zLine = zRibs + (zUp - zRibs) * (p.pos.y - yRibs) / std::max(1e-4f, yUp - yRibs);
                        bulge += std::max(0.0f, p.pos.z - zLine);
                        ++bandPoints;
                    }
                    if (bandPoints > 0) {
                        r.area = bulge * ((yMax - yMin) / static_cast<float>(bandPoints)) * 1e4f / (scale * scale);
                    }
                }
            }
            if (kTrace) {
                const float beyondWall = r.wallPoints > 0 ? std::max(0.0f, zMax - r.wallZ) : 0.0f;
                std::fprintf(stderr, "[breast] %s %-24s bottom row %s: profile %.1f cm2, overhang %.1f mm (bottom row %.1f forward of the wall), depth %.1f, beyond the wall %.1f (%s), %d points, scale %.2f\n",
                             side == RigSide::Left ? "L" : "R", m_boneNames[static_cast<std::size_t>(bone)].c_str(), r.flush ? "flush with the ribs" : "forward of the wall",
                             r.area, r.overhang * 1000.0f, r.underPoints > 0 && r.wallPoints > 0 ? (r.zUnder - r.wallZ) * 1000.0f : 0.0f, r.depth * 1000.0f, beyondWall * 1000.0f,
                             wall >= 0 ? m_boneNames[static_cast<std::size_t>(wall)].c_str() : "-", r.points, scale);
            }
            if (r.points > pick.points || (r.points == pick.points && r.depth > pick.depth)) {
                pick = r;
            }
        }
    }
    // TWO KINDS OF PATCH, ONE KIND PER RIG. The native figures' pectoral owns the whole breast
    // down to the ribs (its bottom row sits FLUSH with the chest wall, within kFlushBand, on BOTH
    // sides): the OVERHANG says the size — 10 mm on the base female, 5 on the androgynous base, 0
    // on the males. A game rig's breast bone owns the dome alone (its bottom row stands a
    // centimetre and more forward of the wall: the fold is the spine's skin): there the patch's
    // DEPTH says it — a dome's skin wraps 5 cm back on a full breast, 4 on a flat chest. The kind
    // is the rig's weighting convention, so it is decided for the two sides TOGETHER: a side that
    // reads flush while the other does not is a dome patch whose lowest vertex happens to sit low
    // (one character's sides read 7 and 15 mm forward of the wall and fell on either side of the
    // band: a full breast and a flat chest on one woman).
    bool holdsFold = true;
    for (const Reading& r : chosen) {
        if (r.points > 0 && !r.flush) {
            holdsFold = false;
        }
    }
    for (const Reading& r : chosen) {
        if (r.points == 0) {
            continue;
        }
        // THE FULLNESS: a patch that reaches the ribs is read by its PROFILE AREA against the base
        // female's (1.0, running on to kBreastFullnessMax for a large bust); a dome-alone patch by its
        // DEPTH (a dome's rim is the spine's skin, so the profile under-reads it — 8 cm² on a modest
        // game-rig woman whose dome reads 0.25), capped at 1.
        Breast best;
        best.bone = r.bone;
        best.parent = r.parent;
        best.size = holdsFold ? glm::clamp((r.area - kBreastAreaFlat) / (kBreastAreaFull - kBreastAreaFlat), 0.0f, kBreastFullnessMax)
                              : glm::clamp((r.depth / scale - kDomeFlat) / (kDomeFull - kDomeFlat), 0.0f, 1.0f);
        // THE PIVOT: the breast's base on the chest wall, straight behind the nipple — at the
        // depth of the ribs its patch reaches, else of the sternum beside it. The bone's joint is
        // no pivot: a native pectoral's sits seven centimetres behind the breast by the spine, and
        // a swing about it slid the whole breast up the chest (the user's close-up: the nipple
        // along the chest, not toward the floor).
        const float wallDepth = r.basePoints > 0 ? r.baseZ : holdsFold && r.underPoints > 0 ? r.zUnder : r.wallPoints > 0 ? r.wallZ : r.bindZ;
        best.protrusion = std::max(0.005f, r.zMax - wallDepth);
        best.pivotBind = glm::vec3(r.xTip, r.yTip, wallDepth);
        // THE SWING'S PIVOT: the breast's upper attachment, kBreastLever over the nipple on the chest
        // wall — the point a hanging breast swings from; the nipple is `lever` from it.
        best.pivotTopBind = glm::vec3(r.xTip, r.yTip + kBreastLever * scale, wallDepth);
        best.lever = std::sqrt(kBreastLever * scale * kBreastLever * scale + best.protrusion * best.protrusion);
        best.side = &r == &chosen[0] ? 1.0f : -1.0f; // (+x is the figure's left: outward for the left breast)
        if (kTrace) {
            std::fprintf(stderr, "[breast] %s chosen %-24s %s -> fullness %.2f (weight %.2f); base %.1f mm behind the nipple (%s), the swing's pivot %.1f mm over it\n", &r == &chosen[0] ? "L" : "R",
                         m_boneNames[static_cast<std::size_t>(r.bone)].c_str(), holdsFold ? "reaches the ribs: the profile" : "the dome alone: its depth", best.size, breastWeight(best.size),
                         best.protrusion * 1000.0f, r.basePoints > 0 ? "the ribs under the fold" : "the fold / the sternum", kBreastLever * scale * 1000.0f);
        }
        if (best.parent >= 0 && best.size > 0.0f) {
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

glm::vec3 Armature::breastGravityInChest(const Breast& b) const {
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
    return glm::transpose(chest) * gModel;
}

glm::vec3 Armature::breastRestDisplacement(const Breast& b) const {
    if (b.parent < 0 || static_cast<std::size_t>(b.parent) >= m_poseGlobal.size()) {
        return glm::vec3(0.0f);
    }
    // What the mass feels beyond the sculpt's standing gravity, in the chest's frame, times the
    // compliance per axis: along the chest the bounce, sideways the sway, out along the tether the
    // hang (never into the chest).
    const glm::vec3 gChest = breastGravityInChest(b);
    const glm::vec3 delta = gChest - glm::vec3(0.0f, -1.0f, 0.0f);
    const float     w = breastWeight(b.size) * softbody::breastStrength();
    const float     press = std::max(0.0f, -delta.z); // (gravity into the chest: on her back)
    const float     out = delta.z > 0.0f ? kBreastHangOut * delta.z * w
                                         : -std::min(kBreastPressMax * b.protrusion, kBreastPressShare * b.protrusion * press * w);
    return glm::vec3(kBreastSide * delta.x * w + b.side * kBreastSpreadOnBack * press * w, kBreastDown * delta.y * w, out);
}

glm::mat4 Armature::breastSwingMatrix(const Breast& b, const glm::mat4& boneGlobal) const {
    const glm::vec3 d = m_breastDynamic ? b.disp : breastRestDisplacement(b);
    const glm::vec3 across(breastRenderedSide(d.x, d.y, b.side), d.y, 0.0f); // (the mass's displacement across the tether, the splay in)
    const float     acrossLen = glm::length(across);
    if ((acrossLen < 1e-5f && d.z < 1e-5f) || b.parent < 0) {
        return glm::mat4(1.0f);
    }
    glm::mat3 chest(m_poseGlobal[static_cast<std::size_t>(b.parent)]);
    for (int c = 0; c < 3; ++c) {
        const float len = glm::length(chest[c]);
        if (len > 1e-8f) {
            chest[c] /= len;
        }
    }
    // The pivot rides the chest: its bind place through the chest's skin transform; the hang is along
    // the tether (the chest's forward axis) as the chest stands.
    const Bone&     parent = m_bones[static_cast<std::size_t>(b.parent)];
    const glm::vec3 pivot(m_poseGlobal[static_cast<std::size_t>(b.parent)] * parent.inverseBind * glm::vec4(b.pivotTopBind, 1.0f));
    const glm::vec3 out = chest[2] * d.z;
    (void)boneGlobal;
    if (acrossLen < 1e-5f) {
        return glm::translate(glm::mat4(1.0f), out);
    }
    // A displacement across the tether is a SWING about the upper attachment that carries the nipple
    // there: the angle it subtends at the pivot's distance from the nipple.
    const float     angle = std::atan2(acrossLen, b.lever);
    const glm::vec3 axis = chest * glm::normalize(glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), across / acrossLen)); // (in the chest's frame: a turned chest carries it)
    return glm::translate(glm::mat4(1.0f), pivot + out) * glm::rotate(glm::mat4(1.0f), angle, axis) * glm::translate(glm::mat4(1.0f), -pivot);
}

bool Armature::tickSoftBody(float dt) {
    if (!softbody::breastMotion() || !m_breastsReady || m_breasts.empty()) {
        return false;
    }
    dt = glm::clamp(dt, 1.0f / 240.0f, 1.0f / 20.0f);
    if (!m_breastDynamic) {
        m_breastChestSamples = 0; // (the first tick of a sequence: no velocity to read yet)
        for (Breast& b : m_breasts) {
            b.disp = breastRestDisplacement(b);
            b.vel = glm::vec3(0.0f);
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
    float           mag = glm::length(accel) / g;
    if (mag < kBreastAccelDeadG) {
        accel = glm::vec3(0.0f);
        mag = 0.0f;
    } else if (mag > kBreastAccelMaxG) {
        accel *= kBreastAccelMaxG / mag;
    }
    const glm::vec3 effective = glm::vec3(0.0f, -1.0f, 0.0f) - accel / g;
    m_breastGravity = glm::mix(m_breastGravity, effective, kBreastGravitySmooth);
    // THE SPRING: the mass toward its resting displacement, stiffening past the cap (the tissue runs
    // out), a fuller breast slower, the right breast a little slower than the left.
    bool moving = false;
    for (Breast& b : m_breasts) {
        const float     w = std::max(0.05f, breastWeight(b.size));
        const float     omega = 2.0f * 3.14159265f * kBreastHz / std::pow(w, 0.25f) * (b.side < 0.0f ? kBreastDetune : 1.0f);
        const float     cap = kBreastBounceCap * std::sqrt(w);
        const glm::vec3 rest = breastRestDisplacement(b);
        // The stiffening is on the EXCURSION from the rest, not on the displacement from the sculpt:
        // on the displacement it held the mass short of its rest while it ticked (a bow's hang 37 mm
        // of 54 through the drag, snapping to 54 at the settle's end) — the rest is a static fact,
        // the cap is for what a kick or a jolt swings beyond it.
        const glm::vec3 excursion = b.disp - rest;
        const float     stiffen = 1.0f + glm::dot(excursion, excursion) / (cap * cap);
        const glm::vec3 damping(kBreastDamping, kBreastDamping, kBreastTetherDamping);
        b.vel += (omega * omega * stiffen * (rest - b.disp) - 2.0f * damping * omega * b.vel) * dt;
        b.disp += b.vel * dt;
        const float pressMost = kBreastPressMax * b.protrusion; // (the press-in never past a share of the breast's depth)
        if (b.disp.z < -pressMost) {
            b.disp.z = -pressMost;
            b.vel.z = std::max(0.0f, b.vel.z);
        }
        if (glm::length(rest - b.disp) > 0.0003f || glm::length(b.vel) > 0.003f) {
            moving = true;
        }
    }
    m_breastDynamic = true;
    computeSkinMatrices();
    static const bool kTickTrace = std::getenv("POSESTUDIO_BREAST_TRACE") && std::getenv("POSESTUDIO_BREAST_TRACE")[0] == '2';
    if (kTickTrace && !m_breasts.empty()) {
        // POSESTUDIO_BREAST_TRACE=2: the first breast per tick — the chest's acceleration (g), the
        // displacement (mm, chest frame: sideways / along the chest / out) and its rest — to read a
        // quick move's bounce against a slow one, a landing's drop.
        const Breast&   b = m_breasts.front();
        const glm::vec3 rest = breastRestDisplacement(b);
        std::fprintf(stderr, "[breast-tick] accel %.2f g | disp %+.1f %+.1f %+.1f mm (rest %+.1f %+.1f %+.1f)\n", mag,
                     b.disp.x * 1000.0f, b.disp.y * 1000.0f, b.disp.z * 1000.0f, rest.x * 1000.0f, rest.y * 1000.0f, rest.z * 1000.0f);
    }
    return moving;
}

void Armature::kickSoftBody(float impactSpeed) {
    if (!softbody::breastMotion() || !m_breastsReady || m_breasts.empty() || impactSpeed <= 0.0f) {
        return;
    }
    // The chest stops; the mass keeps falling, at a share of the impact speed, straight down.
    if (!m_breastDynamic) {
        m_breastChestSamples = 0;
        for (Breast& b : m_breasts) {
            b.disp = breastRestDisplacement(b);
            b.vel = glm::vec3(0.0f);
        }
        m_breastDynamic = true;
    }
    static const float kShare = envOrF("POSESTUDIO_BREAST_IMPACT", kBreastImpactShare);
    const glm::mat3    toModel = glm::inverse(glm::mat3(m_transform));
    const glm::vec3    downModel = toModel * glm::vec3(0.0f, -1.0f, 0.0f);
    for (Breast& b : m_breasts) {
        if (b.parent < 0 || static_cast<std::size_t>(b.parent) >= m_poseGlobal.size()) {
            continue;
        }
        glm::mat3 chest(m_poseGlobal[static_cast<std::size_t>(b.parent)]);
        for (int c = 0; c < 3; ++c) {
            const float len = glm::length(chest[c]);
            if (len > 1e-8f) {
                chest[c] /= len;
            }
        }
        b.vel += glm::transpose(chest) * downModel * (impactSpeed * kShare);
    }
}

void Armature::settleSoftBody() {
    if (!m_breastDynamic) {
        return;
    }
    m_breastDynamic = false;
    m_breastChestSamples = 0;
    m_breastGravity = glm::vec3(0.0f, -1.0f, 0.0f);
    for (Breast& b : m_breasts) {
        b.vel = glm::vec3(0.0f);
    }
    computeSkinMatrices(); // (the resting displacement)
}

bool Armature::softBodyMoving() const {
    if (!m_breastDynamic || !softbody::breastMotion()) {
        return false;
    }
    for (const Breast& b : m_breasts) {
        if (glm::length(breastRestDisplacement(b) - b.disp) > 0.0003f || glm::length(b.vel) > 0.003f) {
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
        if ((side == 0) == (b.side > 0.0f)) {
            const glm::vec3 d = m_breastDynamic ? b.disp : breastRestDisplacement(b);
            return glm::degrees(std::atan2(glm::length(glm::vec2(breastRenderedSide(d.x, d.y, b.side), d.y)), b.lever));
        }
    }
    return 0.0f;
}

float Armature::breastSideMm(int side) const {
    if (!softbody::breastMotion() || !m_breastsReady) {
        return 0.0f;
    }
    for (const Breast& b : m_breasts) {
        if ((side == 0) == (b.side > 0.0f)) {
            const glm::vec3 d = m_breastDynamic ? b.disp : breastRestDisplacement(b);
            return 1000.0f * b.side * breastRenderedSide(d.x, d.y, b.side); // (+ outward, toward the armpit)
        }
    }
    return 0.0f;
}

float Armature::breastHangMm(int side) const {
    if (!softbody::breastMotion() || !m_breastsReady) {
        return 0.0f;
    }
    for (const Breast& b : m_breasts) {
        if ((side == 0) == (b.side > 0.0f)) {
            return 1000.0f * (m_breastDynamic ? b.disp.z : breastRestDisplacement(b).z);
        }
    }
    return 0.0f;
}

} // namespace pose

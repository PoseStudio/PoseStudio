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
 * EFFECTIVE gravity (g less the chest's acceleration) ACROSS the tether and the pose's own
 * gravity ALONG it, and its resting displacement from the sculpt is that change times a
 * COMPLIANCE per axis (kBreastDown, kBreastSide, kBreastHangOut: metres per g at fullness 1, by
 * the breast's weight): along the chest the bounce, sideways the sway and the fall onto her side,
 * and out along the tether the hang, which goes INTO the chest only as a bounded press-in (on
 * her back: flatter and wider, with a spread outward). Rendered, the RESTING displacement across
 * the tether is mostly a SHIFT of the whole breast — a solid body moves as one piece — and a
 * share of it (kBreastTiltShare along the chest, kBreastTiltShareSide sideways) a tilt about the
 * breast's upper attachment (kBreastLever over the nipple on the chest wall) that carries the
 * nipple the rest of the way; the DYNAMIC excursion from the rest is a pure shift, the solid
 * mass moving inside (the eighth look: "the nipples are driving the movement too much; it should
 * resemble a solid ball in the breast moving around"); one along the tether a translation. The
 * tether answers the pose's gravity alone, stiff and critically damped — the jiggle is sideways
 * and up and down, never in and out (the same look), and a drop below the rest sways the
 * breasts apart, a rise above it together (kBreastSplay, in the sideways spring's rest: the
 * figure of eight). Rendered wholly as the tilt it swung like a bag on a strap (the user's
 * seventh look). Bowed forward the sag toward the belly is relieved and the breast is
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
 * beside it owns a few points); the rest ride as before. A RIG WITH NO BREAST BONE (a biped, a
 * motion-capture skeleton, most game and avatar rigs: the breast is the chest's own skin) gets the
 * same mass on a BONELESS breast — armaturebreastfield.cpp reads where the breast stands and how
 * full it is off the chest's skin, and the mass's displacement reaches the skin as a soft-body
 * delta field the GPU blends, where here it is a turn of the bone's skin transform.
 *
 * THE MOTION: while the engine ticks (a drag, its settle's tail, the Ground button's fall and
 * landing bounce) the mass is a damped spring toward its resting displacement (kBreastHz, a
 * fuller breast slower; kBreastDamping 0.7: tissue — a quick move sets it moving once and it
 * settles with a small overshoot, a slow move moves it not at all — the bounce is
 * speed-dependent because what drives it is the chest's ACCELERATION, read off the chest joint's
 * motion: its velocity smoothed (kBreastVelSmooth), differenced, smoothed again
 * (kBreastAccelSmooth), under kBreastAccelDeadG read as none, up to kBreastAccelMaxG, and
 * kBreastJoltShare of it felt — a trembling hand moves the chest a millimetre a tick, which raw
 * is a g and a half (the ninth look: "too sensitive … glitchy jerky movements when posing"); a
 * slow bow moves the rest slowly and the mass follows it), the right breast a little slower than
 * the left (kBreastDetune: they never move quite together). AND THE BOUNCE GOES UP AND SIDEWAYS,
 * NOT DOWN (the tenth look, 2026-11-06: "breasts are already hanging … downward movement is
 * pretty much already at its limit by default — think of something in a bag: it has nowhere to
 * go downward, but can swing side to side and can go upward"): the sculpt IS the breast at the
 * end of its stretch under standing gravity (no pose rests the mass below it), so below the
 * sculpt along the chest the mass meets a FLOOR — kBreastFloorStiffen over the bounce's spring,
 * with its own damping on the way in (kBreastFloorDamping: the hit is absorbed) and none on the
 * way out (what it stored springs the mass back up) — a few millimetres of give under a landing,
 * a fraction of one under a jolt — while above the sculpt and sideways it moves on the ordinary
 * spring. And HOWEVER LARGE THE BUST THE JIGGLE HAS A CEILING (the same look: "for very large
 * breasts there should be an upper limit to how extreme the bounce/jiggle can get"): a soft WALL
 * about the POSE's rest across the tether, at kBreastBounceCap by the square root of the weight
 * and never more than kBreastJiggleCapMax, kBreastWallStiffen past it — a jolt's rest shift or
 * a kick takes the mass to the cap and little beyond, whatever the compliance asks. A falling
 * figure's breasts float up weightless; at the LANDING they keep kBreastImpactShare of the
 * impact speed as the chest stops (kickSoftBody, from beginLandingBounce), dip into the floor
 * and spring back UP past the rest, once.
 * Outside the ticks (a dial, a pose file, undo) the resting displacement applies at once.
 * Nothing of it is in the pose: the channels, the pose files, the undo stack, the harness's
 * measures and the joints' positions read as they did; balance, the IK and the volumes never see
 * it, and the IK settle does not wait for it (it ticks the breasts alone once the pose is at
 * rest). OFF unless the Soft Body Physics tab turns it on (softbody::breastMotion): the harness runs
 * the engine as it was. POSESTUDIO_BREAST_TRACE=1 prints each candidate's profile and the
 * fullness it was given; =2 the displacement per tick against its rest.
 */

#include "armature.h"

#include "ikmath.h" // normalizedFrame
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
// breast's weight (its fullness to kBreastFullnessPower): along the chest (the bounce), sideways
// (the sway, the fall onto her side), and out along the tether the hang, which goes INTO the chest
// only as the bounded press-in below. ACROSS the tether the mass feels EFFECTIVE gravity (the
// chest's jolts set it bouncing); ALONG it the pose's gravity alone (the eighth look: "in/out
// movement feels unnatural — natural breasts have much less of this and more side by side and up
// down"): the hang and the press-in are the pose's, never a jolt's.
constexpr float kBreastDownDefault = 0.030f;        ///< Compliance along the chest (m per g) — UPWARD, in effect, since the floor holds the mass below the sculpt: a fall's weightlessness floats the base female's breast 16 mm up, a chest jolted down throws it up (15 until the eighth look, 18 until the eleventh: "a little too subtle now, let's increase the upward bounce" — the user, after the floor).
constexpr float kBreastSideDefault = 0.030f;        ///< Compliance sideways (m per g): on her side the breasts fall toward the lower side — the upper one in toward the sternum, the lower one out onto the surface (8 until the fifth look: "laying on her side should cause the breasts to fall naturally"; 20 until the eighth: "large breasts would likely hang more when on her side"); a turn or a side step sways them by the same.
constexpr float kBreastAcrossDefault = 0.020f;      ///< THE UPPER BREAST HANGS ACROSS (the thirteenth look, 2026-11-06, with a picture of a full bust on her side: "the lower breast hangs naturally, but the upper one needs to hang more — more pronounced on larger breasts"): a breast falling TOWARD THE STERNUM — the upper one on her side — drapes across the chest wall under its whole weight, where the lower one's fall outward meets the wall's curve; the fall toward the sternum gets this much MORE per g of the POSE's gravity (m, by the weight: the base female's upper breast falls 54 mm against the lower one's 32, the user's figure 90 against 54), never a jolt's — the sway stays even. POSESTUDIO_BREAST_ACROSS.
constexpr float kBreastHangOutDefault = 0.030f;     ///< Compliance out along the tether (m per g): a level-chested bow hangs the base female's breast 30 mm (25 and 35 looked at, 30 between).
constexpr float kBreastSplayDefault = 0.3f;         ///< THE FIGURE OF EIGHT: the sideways spring's rest moves OUTWARD by this share of the mass's drop below its rest (inward by its rise above it), so a bounce sways the breasts apart and together, lagging the drop by the sideways spring's own phase (the eighth look: "more side by side and up down"). It is in the DYNAMICS, not the rendering: drawn as a share of the displacement through the pendulum rendering (0.35, 0.25, 0.10 before the seventh look) it read as the slosh of a sack of fluid.
constexpr float kBreastTiltShareDefault = 0.25f;    ///< RENDERING the RESTING displacement along the chest: this share of it as a TILT about the upper attachment (the nipple leads a little in a bow), the rest as a SHIFT of the whole breast. Rendered wholly as a tilt — a pendulum from the attachment — a breast that dropped swung its lower half forward and back on a hinge, which is how a bag on a strap moves (the seventh look: "too much like a bag filled with liquid… breasts are more solid, but do move"); a solid breast shifts as one piece. The DYNAMIC excursion from the rest is rendered as a pure shift (the eighth look: "the nipples are driving the movement too much; it should resemble a solid ball in the breast moving around").
constexpr float kBreastTiltShareSideDefault = 0.4f; ///< ... and of the resting SIDEWAYS displacement: on her side the breasts roll toward the lower side as well as moving, on her back they roll outward.
constexpr float kBreastLeverDefault = 0.050f;       ///< The swing's pivot stands this far over the nipple on the chest wall (m, figure-scaled): the breast's upper attachment, the pendulum's point. A displacement across the tether is rendered as a swing about it (the nipple follows the mass), one along the tether as a translation.
constexpr float kBreastBounceCapDefault = 0.030f;   ///< THE JIGGLE'S CEILING: a soft wall about the POSE's rest, across the tether, at this radius (m at fullness 1, by the square root of the weight) — past it the tissue holds, and a jolt's rest shift or a kick takes the mass to the cap and little beyond (until the tenth look an isotropic stiffening about the EFFECTIVE rest, which the jolt itself moves: it bounded nothing but a kick's travel).
constexpr float kBreastJiggleCapMaxDefault = 0.036f; ///< ... and never more than this, however full the bust (the tenth look: "for very large breasts, there should be an upper limit to how extreme the bounce/jiggle can get"): the cap stops growing at a weight of 1.44.
constexpr float kBreastWallStiffenDefault = 15.0f;  ///< The wall's spring, as a factor over the bounce's: past the cap the mass is pushed back this many times as hard as the spring pulls it out (a 1.25 m/s kick sideways runs 20 mm past the cap and comes back).
constexpr float kBreastFloorStiffenDefault = 150.0f; ///< THE FLOOR: below the sculpt along the chest the tissue is at the end of its stretch (the tenth look: "downward movement is pretty much already at its limit by default — something in a bag has nowhere to go downward, but can swing side to side and can go upward") — a spring this many times the bounce's: a landing's kick dips 5-10 mm into it, a jolt that asks the rest below the sculpt sinks a fraction of a millimetre (60 at the tenth look; stiffer with the larger kick of the eleventh, so the dip stays a few millimetres).
constexpr float kBreastFloorDampingDefault = 0.15f; ///< ... with this damping ratio (against the floor's own frequency) on the way IN — a little of the hit absorbed — and none on the way out, so what the floor stored springs the mass back UP past the rest: the bounce the user described goes up (0.35 at the tenth look; less absorbed since the eleventh, for a higher bounce).
constexpr float kBreastHzDefault = 3.2f;            ///< The bounce's frequency at fullness 1; a fuller breast slower by the fourth root of its weight (a large bust at 2.8 Hz, a small one at 5). Tissue answers quickly and stops; fluid sloshes slowly (2.0 read as a sack of fluid; 2.4 until the fourteenth look, 2026-11-07: "the bounce feels a little laggy, sluggish … needs to bounce faster when movement drives them" — at 2.4 the mass took six ticks to arrive where the rest went).
constexpr float kBreastDampingDefault = 0.7f;       ///< Its damping ratio across the tether: a small overshoot past the rest and a settle — tissue, not gel (0.25 rang three or four times and read as jello, the user's fifth look: "breasts are a bit more solid than a springy gel"; 0.45 still read as "sacks filled with fluid", the sixth; 0.6 — one swing past the rest — "too much elasticity, too much bounce, breasts aren't so springy", the tenth; 0.8 until the fourteenth, which read as sluggish: 0.7 comes back a little quicker with one small overshoot).
constexpr float kBreastTetherDamping = 1.0f;        ///< ... and ALONG the tether critically damped, at one and a half times the frequency: the hang follows the pose and never bounces in its length.
constexpr float kBaseBelowFold = 0.03f;             ///< The chest wall under the breast: the wall bone's skin within this below the patch's bottom (m, figure-scaled).
constexpr float kBreastImpactShare = 0.6f;         ///< THE LANDING'S KICK: the share of the impact speed the breasts keep as the chest stops (the whole of it flung a large bust a hand's length down the chest, past what tissue allows; 0.3 while the acceleration reader counted the stop as well, 0.5 once the kick was the whole of it; 0.4 at the tenth look, when the floor took the kick — the share sets how far the mass springs back UP — and 0.6 at the eleventh, for a higher bounce: a 50 cm landing dips a full bust 5-10 mm into the floor and lifts it 25-35 above the rest, once, the wall rounding the fullest).
constexpr float kBreastKickSizeMax = 1.3f;          ///< The kick grows with the bust — by the square root of the weight — up to this (1.0, the base female's, until the eleventh look: a fuller breast bounced no higher than hers; the wall keeps the fullest under its ceiling).
constexpr float kBreastDetune = 0.985f;              ///< The right breast's frequency against the left's: the two never move quite together.
constexpr int   kBreastSubsteps = 10;               ///< The spring is integrated in at least this many substeps per 60 Hz tick — more for a longer tick, so a substep never exceeds 1/600 s (stability: semi-implicit Euler holds while omega × step is well under 2, and the floor's and the wall's springs together are sqrt(1 + kBreastWallStiffen + kBreastFloorStiffen) times the bounce's — at the weight floor, omega 42 at 3.2 Hz, 550/s: 0.9 a substep; a tick clamped at 1/20 s in ten substeps was 2.7, a runaway; see tickBreasts).
constexpr float kBreastAccelMaxG = 1.5f;            ///< The chest's acceleration is read up to this, in g (a quick drag's start and stop are what set the breasts bouncing; a landing's deceleration is the KICK's, below; 2.5 until the ninth look).
constexpr float kBreastVelSmoothDefault = 0.55f;    ///< THE CHEST'S ACCELERATION IS READ OFF A SMOOTHED VELOCITY: the chest joint's velocity low-passed by this per tick, differenced, and the acceleration low-passed by kBreastAccelSmooth. A trembling hand (and the IK's own tick-to-tick tremble) moves the chest a millimetre a tick, which raw is 4 mm/tick² = 1.5 g — the "jumping around" of the ninth look; through the two filters it is under a tenth of a g, under the dead band, while a real jolt keeps its impulse and lands two ticks later (0.4 and 0.3 until the fourteenth look: the two filters put a real jolt four ticks late, the first of the lag the user felt; a 1 mm tremble reads 0.08 g through 0.55 / 0.45, a 2 mm one 0.16 — under the dead band both). POSESTUDIO_BREAST_VEL_SMOOTH / _ACCEL_SMOOTH / _DEAD_G.
constexpr float kBreastAccelSmoothDefault = 0.45f;  ///< ... (0.35 on the raw acceleration until the ninth look: a cursor's jitter is not a force; a drag's jolt is).
constexpr float kBreastAccelDeadGDefault = 0.18f;   ///< Accelerations under this (g) are read as none (0.05 until the ninth look, 0.15 until the fourteenth).
constexpr float kBreastJoltShareDefault = 0.3f;     ///< (0.5 until the fourteenth look: the lighter filters pass twice the acceleration they did and the quicker spring follows it further, so the share came down to hold the amplitudes the user had called right — the fastest carry feels 0.9 g where it felt 1.0, a medium move 0.5 where it felt 0.45.) The share of the chest's acceleration the mass feels: the compliances are sized for the POSE (a bust falling onto her side), and a drag's jolts — a mouse moving a chest 15 cm in a tenth of a second is 1.5 g — would throw the mass further than a body's (0.7 until the tenth look: "too much bounce"). POSESTUDIO_BREAST_JOLT.
constexpr float kBreastJoltGainGDefault = 1.5f;     ///< THE FAST MOTION'S EMPHASIS (the twelfth look: "fast motions aren't bouncing the breasts enough … keep the way it moves the same, just more on faster motion"): the share grows with the acceleration — × (1 + a / this, in g) — so a medium move (0.3 g) feels a fifth more than the share alone and the sharpest carry a mouse makes (1.1 g through the tremble filter, whose smoothing is what shaves a fast move, the 1.5 g cap never binding) three quarters more; the cap's 1.5 g feels twice the share. POSESTUDIO_BREAST_JOLT_GAIN.
// ON HER BACK (the fifth look, with photographs: "when a character is laying flat on their back,
// the breasts should flatten a bit and fall a bit to the sides"): gravity INTO the chest presses
// the breast back toward the wall — the nipple by kBreastPressShare of the breast's own depth per
// g, never past kBreastPressMax of it (a rigid skin cannot flatten; pushed back a third of its
// depth with the chest wall's skin staying, the breast reads flatter and wider) — and spreads it
// outward by kBreastSpreadOnBack per g, both by the weight.
constexpr float kBreastPressShare = 0.55f;          // (0.30 until the eighth look: "when laying on her back the breasts should flatten out more")
constexpr float kBreastPressMax = 0.65f;            // (0.40 until the same look; a rigid patch sunk further than this into the chest wall reads as a crease, not a flatter breast)
constexpr float kBreastSpreadOnBackDefault = 0.030f;  // (0.018 until the same look)

/// A/B levers for the look (mm per g: POSESTUDIO_BREAST_DOWN, _SIDE, _HANG; _SPLAY share; _LEVER mm; _CAP and _CAPMAX mm; _WALL, _FLOOR and _FLOOR_DAMPING; _HZ; _DAMPING): read once.
float envOrF(const char* name, float fallback) {
    const char* v = std::getenv(name);
    return v && *v ? static_cast<float>(std::atof(v)) : fallback;
}
const float kBreastDown = envOrF("POSESTUDIO_BREAST_DOWN", kBreastDownDefault * 1000.0f) / 1000.0f;
const float kBreastSide = envOrF("POSESTUDIO_BREAST_SIDE", kBreastSideDefault * 1000.0f) / 1000.0f;
const float kBreastHangOut = envOrF("POSESTUDIO_BREAST_HANG", kBreastHangOutDefault * 1000.0f) / 1000.0f;
const float kBreastSplay = envOrF("POSESTUDIO_BREAST_SPLAY", kBreastSplayDefault);
const float kBreastTiltShare = envOrF("POSESTUDIO_BREAST_TILT", kBreastTiltShareDefault);
const float kBreastTiltShareSide = envOrF("POSESTUDIO_BREAST_TILT_SIDE", kBreastTiltShareSideDefault);
const float kBreastSpreadOnBack = envOrF("POSESTUDIO_BREAST_SPREAD", kBreastSpreadOnBackDefault * 1000.0f) / 1000.0f;
const float kBreastAcross = envOrF("POSESTUDIO_BREAST_ACROSS", kBreastAcrossDefault * 1000.0f) / 1000.0f;
const float kBreastLever = envOrF("POSESTUDIO_BREAST_LEVER", kBreastLeverDefault * 1000.0f) / 1000.0f;
const float kBreastBounceCap = envOrF("POSESTUDIO_BREAST_CAP", kBreastBounceCapDefault * 1000.0f) / 1000.0f;
const float kBreastJiggleCapMax = envOrF("POSESTUDIO_BREAST_CAPMAX", kBreastJiggleCapMaxDefault * 1000.0f) / 1000.0f;
const float kBreastWallStiffen = envOrF("POSESTUDIO_BREAST_WALL", kBreastWallStiffenDefault);
const float kBreastFloorStiffen = envOrF("POSESTUDIO_BREAST_FLOOR", kBreastFloorStiffenDefault);
const float kBreastFloorDamping = envOrF("POSESTUDIO_BREAST_FLOOR_DAMPING", kBreastFloorDampingDefault);
const float kBreastHz = envOrF("POSESTUDIO_BREAST_HZ", kBreastHzDefault);
const float kBreastDamping = envOrF("POSESTUDIO_BREAST_DAMPING", kBreastDampingDefault);
const float kBreastJoltShare = envOrF("POSESTUDIO_BREAST_JOLT", kBreastJoltShareDefault);
const float kBreastJoltGainG = envOrF("POSESTUDIO_BREAST_JOLT_GAIN", kBreastJoltGainGDefault);
const float kBreastVelSmooth = envOrF("POSESTUDIO_BREAST_VEL_SMOOTH", kBreastVelSmoothDefault);
const float kBreastAccelSmooth = envOrF("POSESTUDIO_BREAST_ACCEL_SMOOTH", kBreastAccelSmoothDefault);
const float kBreastAccelDeadG = envOrF("POSESTUDIO_BREAST_DEAD_G", kBreastAccelDeadGDefault);

/// The weight a breast's motion is scaled by: its fullness (0 flat .. 1 the base female .. 3) to kBreastFullnessPower.
float breastWeight(float fullness) {
    return fullness <= 0.0f ? 0.0f : std::pow(fullness, kBreastFullnessPower);
}

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
    // A BONELESS BREAST'S FIELD (armaturebreastfield.cpp) is read with the model's vertex data at
    // the import; a figure whose mesh came in another way (a dump) reads it here.
    if (!m_breastFieldsReady) {
        setupBreastFields();
    }
    const std::vector<glm::vec3> bind = bindJointPositions(); // (model space; binds are translation-only)
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
        // A BONELESS BREAST (armaturebreastfield.cpp): no pectoral bone of this side carries the
        // breast's apex — the rig has none, or only helpers beside it — and the chest's own skin
        // is full enough to move: the mass is this file's, and its displacement reaches the skin as
        // a delta field (bone -1: no skin transform is turned).
        const int          sideIndex = &r == &chosen[0] ? 0 : 1;
        const BreastField& field = m_breastFields[sideIndex];
        if (field.valid) {
            Breast b;
            b.bone = -1;
            b.field = sideIndex;
            b.parent = field.chest;
            b.size = field.size;
            b.protrusion = std::max(0.005f, field.apexBind.z - field.wallZ);
            b.pivotBind = glm::vec3(field.apexBind.x, field.apexBind.y, field.wallZ);
            b.pivotTopBind = glm::vec3(field.apexBind.x, field.apexBind.y + kBreastLever * scale, field.wallZ);
            b.lever = std::sqrt(kBreastLever * scale * kBreastLever * scale + b.protrusion * b.protrusion);
            b.side = sideIndex == 0 ? 1.0f : -1.0f;
            if (kTrace) {
                std::fprintf(stderr, "[breast] %s boneless: the chest's own skin on %s, moved as a field -> fullness %.2f (weight %.2f); the wall %.1f mm behind the apex\n",
                             sideIndex == 0 ? "L" : "R", b.parent >= 0 ? m_boneNames[static_cast<std::size_t>(b.parent)].c_str() : "-", b.size,
                             breastWeight(b.size), b.protrusion * 1000.0f);
            }
            if (b.parent >= 0) {
                m_breasts.push_back(b);
            }
            continue;
        }
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
    m_breastReader.reset();
    m_breastGravity = glm::vec3(0.0f, -1.0f, 0.0f);
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

glm::vec3 Armature::breastGravityInChest(const Breast& b, bool effective) const {
    // Gravity (in g) in the chest's frame — EFFECTIVE (g less the chest's acceleration, smoothed)
    // or the pose's own; the chest's rotation is its world rotation away from the bind (binds are
    // translation-only), the model transform taken off first.
    const glm::mat3 toModel = glm::inverse(glm::mat3(m_transform));
    const glm::vec3 gModel = toModel * (effective ? m_breastGravity : glm::vec3(0.0f, -1.0f, 0.0f));
    const glm::mat3 chest = normalizedFrame(m_poseGlobal[static_cast<std::size_t>(b.parent)]);
    return glm::transpose(chest) * gModel;
}

glm::vec3 Armature::breastRestDisplacement(const Breast& b, bool effective) const {
    if (b.parent < 0 || static_cast<std::size_t>(b.parent) >= m_poseGlobal.size()) {
        return glm::vec3(0.0f);
    }
    // What the mass feels beyond the sculpt's standing gravity, in the chest's frame, times the
    // compliance per axis: along the chest the bounce, sideways the sway, out along the tether the
    // hang (into the chest only as the bounded press-in). ACROSS the tether that is EFFECTIVE
    // gravity — the chest's jolts set the mass bouncing sideways and along the chest; ALONG it
    // the pose's gravity alone: the hang and the press-in are the pose's, never a jolt's (the
    // eighth look: "in/out movement feels unnatural").
    const glm::vec3 gChest = breastGravityInChest(b, effective);
    const glm::vec3 delta = gChest - glm::vec3(0.0f, -1.0f, 0.0f);
    const glm::vec3 gPose = breastGravityInChest(b, false);
    const float     along = gPose.z; // (the standing pull has no part along the tether)
    const float     w = breastWeight(b.size) * softbody::breastStrength();
    const float     press = std::max(0.0f, -along); // (gravity into the chest: on her back)
    const float     out = along > 0.0f ? kBreastHangOut * along * w
                                       : -std::min(kBreastPressMax * b.protrusion, kBreastPressShare * b.protrusion * press * w);
    // THE UPPER BREAST HANGS ACROSS: the pose's gravity toward the sternum (on her side, the upper
    // breast's) drapes it across the chest wall by kBreastAcross more per g than the sideways
    // compliance alone — the pose's gravity, so a jolt's sway stays even either way.
    const float inward = std::max(0.0f, -b.side * gPose.x); // (g toward the sternum: + for either breast)
    return glm::vec3(kBreastSide * delta.x * w - b.side * kBreastAcross * inward * w + b.side * kBreastSpreadOnBack * press * w,
                     kBreastDown * delta.y * w, out);
}

glm::mat4 Armature::breastSwingMatrix(const Breast& b) const {
    const glm::vec3 rest = breastRestDisplacement(b, false); // (the POSE's: the tilt is the static hang's, never a jolt's — tilted up by a jolt about the attachment the nipple swung FORWARD, the ninth look)
    const glm::vec3 d = m_breastDynamic ? b.disp : breastRestDisplacement(b);
    const glm::vec3 across(d.x, d.y, 0.0f); // (the mass's displacement across the tether)
    const float     acrossLen = glm::length(across);
    if ((acrossLen < 1e-5f && std::abs(d.z) < 1e-5f) || b.parent < 0) {
        return glm::mat4(1.0f);
    }
    const glm::mat3 chest = normalizedFrame(m_poseGlobal[static_cast<std::size_t>(b.parent)]);
    // The pivot rides the chest: its bind place through the chest's skin transform; the hang is along
    // the tether (the chest's forward axis) as the chest stands.
    const Bone&     parent = m_bones[static_cast<std::size_t>(b.parent)];
    const glm::vec3 pivot(m_poseGlobal[static_cast<std::size_t>(b.parent)] * parent.inverseBind * glm::vec4(b.pivotTopBind, 1.0f));
    // The RESTING displacement across the tether is mostly a SHIFT of the whole breast (a solid body
    // moves as one piece) and in part a TILT about the upper attachment that carries the nipple the
    // rest of the way (kBreastTiltShare along the chest, kBreastTiltShareSide sideways); the DYNAMIC
    // excursion from the rest is a pure shift — the solid mass moving inside — and a displacement
    // along the tether a translation.
    const glm::vec3 tiltPart(kBreastTiltShareSide * rest.x, kBreastTiltShare * rest.y, 0.0f);
    const float     tiltLen = glm::length(tiltPart);
    const glm::vec3 shift = chest * (across - tiltPart) + chest[2] * d.z;
    if (tiltLen < 1e-5f) {
        return glm::translate(glm::mat4(1.0f), shift);
    }
    const float     angle = std::atan2(tiltLen, b.lever);
    const glm::vec3 axis = chest * glm::normalize(glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), tiltPart / tiltLen)); // (in the chest's frame: a turned chest carries it)
    return glm::translate(glm::mat4(1.0f), pivot + shift) * glm::rotate(glm::mat4(1.0f), angle, axis) * glm::translate(glm::mat4(1.0f), -pivot);
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

glm::vec3 Armature::SoftBodyAccelReader::sample(const glm::vec3& p, float dt, float velSmooth, float accelSmooth, float deadG,
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

void Armature::tickBreasts(float dt, bool& movingOut) {
    if (!m_breastDynamic) {
        m_breastReader.reset(); // (the first tick of a sequence: no velocity to read yet, no acceleration carried over)
        for (Breast& b : m_breasts) {
            b.disp = breastRestDisplacement(b);
            b.vel = glm::vec3(0.0f);
        }
    }
    // EFFECTIVE GRAVITY: g less the chest's acceleration, off the chest joint's motion — the
    // velocity SMOOTHED before it is differenced, and the acceleration smoothed after (a millimetre
    // of tick-to-tick tremble is a g and a half raw; the two filters keep a jolt's impulse and lose
    // the tremble), the dead band, the cap, and the share of it the mass feels.
    const std::size_t chestBone = static_cast<std::size_t>(m_breasts.front().parent);
    float             mag = 0.0f;
    const glm::vec3   felt = m_breastReader.sample(glm::vec3(m_transform * m_poseGlobal[chestBone][3]), dt, kBreastVelSmooth,
                                                   kBreastAccelSmooth, kBreastAccelDeadG, kBreastAccelMaxG, mag);
    // The share, and the fast motion's emphasis on top of it: the faster the move, the more of its
    // acceleration the mass feels (kBreastJoltGainG; a slow move is under the dead band and feels
    // nothing, as before).
    const float joltGain = kBreastJoltShare * softbody::breastBounce() * (1.0f + (kBreastJoltGainG > 0.0f ? mag / kBreastJoltGainG : 0.0f)); // (the tab's Bounce scales the dynamics alone)
    m_breastGravity = glm::vec3(0.0f, -1.0f, 0.0f) - felt * joltGain;
    // THE SPRING: the mass toward its resting displacement, a fuller breast slower, the right breast a
    // little slower than the left; a FLOOR below the sculpt along the chest and a WALL about the pose's
    // rest across the tether (both below).
    bool  moving = false;
    float traceFloor = 0.0f, traceOverCap = 0.0f;
    for (Breast& b : m_breasts) {
        const float     w = std::max(0.05f, breastWeight(b.size));
        const float     omega = 2.0f * 3.14159265f * kBreastHz / std::pow(w, 0.25f) * (b.side < 0.0f ? kBreastDetune : 1.0f);
        // THE JIGGLE'S CEILING: the wall's radius about the POSE's rest — kBreastBounceCap by the square
        // root of the weight, and never more than kBreastJiggleCapMax (the upper limit for a very large
        // bust, the tenth look).
        const float     cap = std::min(kBreastBounceCap * std::sqrt(w), kBreastJiggleCapMax);
        const glm::vec3 restEffective = breastRestDisplacement(b);
        const glm::vec3 restPose = breastRestDisplacement(b, false);
        const float     restPoseY = restPose.y;
        // ALONG the tether the spring is stiffer and critically damped: its rest is the pose's hang,
        // which it follows and never bounces about (the eighth look: no in/out jiggle).
        const glm::vec3 damping(kBreastDamping, kBreastDamping, kBreastTetherDamping);
        const glm::vec3 omegaAxis(omega, omega, 1.5f * omega);
        const float     pressMost = kBreastPressMax * b.protrusion; // (the press-in never past a share of the breast's depth)
        // INTEGRATED IN SUBSTEPS: semi-implicit Euler holds only while omega × step is well under 2,
        // and a small bust's spring (omega 42 at the weight floor, 3.2 Hz) under the floor's and the
        // wall's factors is 550/s — one 60 Hz step blew the first stiffening up, the displacement
        // running away to infinity and the skin with it. Ten substeps a 60 Hz tick keep it at 0.9,
        // and a longer tick (the fall's and the landing's wall-clock dt, clamped at 1/20 s) takes
        // proportionally more, so a frame stall never lengthens a substep (at ten it was 2.7: the
        // mass ejected from the floor at several times its entry speed, 2026-11-07).
        const int   substeps = std::max(kBreastSubsteps, static_cast<int>(std::ceil(dt * 60.0f * static_cast<float>(kBreastSubsteps))));
        const float h = dt / static_cast<float>(substeps);
        glm::vec3   rest = restEffective;
        for (int k = 0; k < substeps; ++k) {
            // THE FIGURE OF EIGHT: the sideways rest follows the mass's drop below (rise above) the
            // POSE's rest, outward (inward) by kBreastSplay — a bounce sways the breasts apart and
            // together, the sideways spring lagging the drop by its own phase. The pose's rest, not
            // the effective one: under a landing the effective rest dives ahead of the mass, and read
            // against it a dropping breast came INWARD.
            rest = restEffective;
            rest.x += b.side * kBreastSplay * (restPoseY - b.disp.y);
            glm::vec3 acc = omegaAxis * omegaAxis * (rest - b.disp) - 2.0f * damping * omegaAxis * b.vel;
            // THE WALL: past `cap` from the POSE's rest across the tether the mass is pushed back
            // kBreastWallStiffen times as hard as the spring pulls it out, so a jolt's rest shift or
            // a kick takes it to the cap and a little beyond, however large the bust. About the
            // POSE's rest, not the effective one: the jolt MOVES the effective rest, and a stiffening
            // about it (the first cut) bounded nothing but a kick's travel — it only quickened the
            // follow. The rest is a static fact of the pose; the cap is for what moves about it.
            const glm::vec2 e(b.disp.x - restPose.x, b.disp.y - restPose.y);
            const float     eLen = glm::length(e);
            if (eLen > cap) {
                const glm::vec2 push = e * (-omega * omega * kBreastWallStiffen * (eLen - cap) / eLen);
                acc.x += push.x;
                acc.y += push.y;
                if (&b == &m_breasts.front()) {
                    traceOverCap = eLen - cap;
                }
            }
            // THE FLOOR: below the sculpt along the chest the tissue is at the end of its stretch —
            // a spring kBreastFloorStiffen times the bounce's, damped on the way IN (the hit is
            // absorbed) and not on the way out (what it stored springs the mass back UP): a landing
            // dips a few millimetres and bounces up, a jolt that asks the rest below the sculpt sinks
            // a fraction of one. (No pose rests the mass below the sculpt: the standing pull is the
            // most gravity ever has along the chest, so the floor is never where a rest is.)
            if (b.disp.y < 0.0f) {
                const float omegaFloor = omega * std::sqrt(kBreastFloorStiffen);
                acc.y += omegaFloor * omegaFloor * (-b.disp.y) - 2.0f * kBreastFloorDamping * omegaFloor * std::min(0.0f, b.vel.y);
                if (&b == &m_breasts.front()) {
                    traceFloor = std::max(traceFloor, -b.disp.y);
                }
            }
            b.vel += acc * h;
            b.disp += b.vel * h;
            if (b.disp.z < -pressMost) {
                b.disp.z = -pressMost;
                b.vel.z = std::max(0.0f, b.vel.z);
            }
        }
        if (!std::isfinite(b.disp.x + b.disp.y + b.disp.z + b.vel.x + b.vel.y + b.vel.z)) {
            // Never on a finite rest with the substeps above; the guard keeps a figure whose numbers
            // went wrong (a NaN bone, a zero-length chest axis) from carrying it into the skin.
            b.disp = restEffective;
            b.vel = glm::vec3(0.0f);
        }
        if (glm::length(rest - b.disp) > 0.0003f || glm::length(b.vel) > 0.003f) {
            moving = true;
        }
    }
    m_breastDynamic = true;
    movingOut = movingOut || moving;
    static const bool kTickTrace = std::getenv("POSESTUDIO_BREAST_TRACE") && std::getenv("POSESTUDIO_BREAST_TRACE")[0] == '2';
    if (kTickTrace && !m_breasts.empty()) {
        // POSESTUDIO_BREAST_TRACE=2: the first breast per tick — the chest's acceleration (g), the
        // displacement (mm, chest frame: sideways / along the chest / out) and its rest — to read a
        // quick move's bounce against a slow one, a landing's drop.
        const Breast&   b = m_breasts.front();
        const glm::vec3 rest = breastRestDisplacement(b);
        std::fprintf(stderr, "[breast-tick] accel %.2f g | disp %+.1f %+.1f %+.1f mm (rest %+.1f %+.1f %+.1f) floor %.1f over the cap %.1f\n", mag * joltGain,
                     b.disp.x * 1000.0f, b.disp.y * 1000.0f, b.disp.z * 1000.0f, rest.x * 1000.0f, rest.y * 1000.0f, rest.z * 1000.0f,
                     traceFloor * 1000.0f, traceOverCap * 1000.0f);
    }
}

void Armature::kickSoftBody(float impactSpeed) {
    kickBreasts(impactSpeed);
    kickButtocks(impactSpeed);
}

void Armature::kickBreasts(float impactSpeed) {
    if (!softbody::breastMotion() || !m_breastsReady || m_breasts.empty() || impactSpeed <= 0.0f) {
        return;
    }
    // The chest stops; the mass keeps falling, at a share of the impact speed, straight down.
    if (!m_breastDynamic) {
        for (Breast& b : m_breasts) {
            b.disp = breastRestDisplacement(b);
            b.vel = glm::vec3(0.0f);
        }
        m_breastDynamic = true;
    }
    // THE KICK IS THE LANDING'S DECELERATION: the acceleration reader forgets the fall (its
    // smoothed velocity would otherwise read the stop as a g and more over the next ten ticks, on
    // top of the kick — a 50 cm landing dropped a large bust 47 mm where the kick alone drops it
    // 30) and the float's weightlessness ends with the impact.
    m_breastReader.reset();
    m_breastGravity = glm::vec3(0.0f, -1.0f, 0.0f);
    static const float kShare = envOrF("POSESTUDIO_BREAST_IMPACT", kBreastImpactShare);
    const glm::mat3    toModel = glm::inverse(glm::mat3(m_transform));
    const glm::vec3    downModel = toModel * glm::vec3(0.0f, -1.0f, 0.0f);
    for (Breast& b : m_breasts) {
        if (b.parent < 0 || static_cast<std::size_t>(b.parent) >= m_poseGlobal.size()) {
            continue;
        }
        const glm::mat3 chest = normalizedFrame(m_poseGlobal[static_cast<std::size_t>(b.parent)]);
        // By the breast's weight (to its square root, up to kBreastKickSizeMax): a kick is a
        // velocity, and resolved in substeps at one and the same for every bust it dropped a modest
        // bust (weight 0.15) 20 mm against a large one's 36 — "none for very small" is the user's
        // rule — and bounced a full bust no higher than the base female's; a fuller breast bounces
        // more, the wall its ceiling.
        const float bySize = std::min(kBreastKickSizeMax, std::sqrt(std::max(0.0f, breastWeight(b.size))));
        b.vel += glm::transpose(chest) * downModel * (impactSpeed * kShare * bySize * softbody::breastStrength() * softbody::breastBounce());
    }
}

void Armature::settleSoftBody() {
    const bool was = m_breastDynamic || m_buttockDynamic;
    settleBreasts();
    settleButtocks();
    if (was) {
        computeSkinMatrices(); // (the resting displacement; the buttocks' weights back to 0)
    }
}

void Armature::settleBreasts() {
    if (!m_breastDynamic) {
        return;
    }
    m_breastDynamic = false;
    m_breastReader.reset();
    m_breastGravity = glm::vec3(0.0f, -1.0f, 0.0f);
    for (Breast& b : m_breasts) {
        b.vel = glm::vec3(0.0f);
    }
}

bool Armature::softBodyMoving() const {
    return breastsMoving() || buttocksMoving();
}

bool Armature::breastsMoving() const {
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
            return glm::degrees(std::atan2(glm::length(glm::vec2(d.x, d.y)), b.lever));
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
            return 1000.0f * b.side * d.x; // (+ outward, toward the armpit)
        }
    }
    return 0.0f;
}

float Armature::breastUpMm(int side) const {
    if (!softbody::breastMotion() || !m_breastsReady) {
        return 0.0f;
    }
    for (const Breast& b : m_breasts) {
        if ((side == 0) == (b.side > 0.0f)) {
            const glm::vec3 d = m_breastDynamic ? b.disp : breastRestDisplacement(b);
            return 1000.0f * d.y; // (+ up the chest toward the head; below 0 is the floor's give)
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
